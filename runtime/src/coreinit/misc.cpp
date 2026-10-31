// coreinit odds and ends: logging (OSReport & co, with a guest printf), OSDynLoad, TLS,
// system information, exit, user/system config.
#include <stdarg.h>
#include <stdio.h>

#include <string>
#include <unordered_map>

#include "hle.h"
#include "loader.h"
#include "os.h"
#include "region.h"
#include "thread.h"

namespace {
// Guest printf. Integer varargs come from GPRs starting at `gpr` then the stack overflow area;
// doubles from f1.. (PPC SysV). For va_list-based calls the caller supplies an accessor.
struct VaSource {
    PPCContext* c;
    int gpr;          // next GPR index (3..10)
    int fpr;          // next FPR index (1..8)
    uint32_t stack;   // overflow area
    bool va;          // read through a guest va_list instead
    uint32_t va_ptr;
    uint32_t next_u32() {
        if (va) {
            uint8_t g = mem::r8(va_ptr);
            if (g < 8) {
                mem::w8(va_ptr, g + 1);
                return mem::r32(mem::r32(va_ptr + 8) + 4 * g);
            }
            uint32_t a = mem::r32(va_ptr + 4);
            mem::w32(va_ptr + 4, a + 4);
            return mem::r32(a);
        }
        if (gpr <= 10) return c->r[gpr++];
        uint32_t v = mem::r32(stack);
        stack += 4;
        return v;
    }
    uint64_t next_u64() {
        if (va) {
            uint8_t g = mem::r8(va_ptr);
            if (g & 1) g++;
            if (g < 7) {
                mem::w8(va_ptr, g + 2);
                uint32_t base = mem::r32(va_ptr + 8);
                return ((uint64_t)mem::r32(base + 4 * g) << 32) | mem::r32(base + 4 * g + 4);
            }
            mem::w8(va_ptr, 8);
            uint32_t a = (mem::r32(va_ptr + 4) + 7) & ~7u;
            mem::w32(va_ptr + 4, a + 8);
            return mem::r64(a);
        }
        if (gpr % 2 == 0) gpr++;  // 64-bit values start at an odd register
        if (gpr <= 9) {
            uint64_t v = ((uint64_t)c->r[gpr] << 32) | c->r[gpr + 1];
            gpr += 2;
            return v;
        }
        stack = (stack + 7) & ~7u;
        uint64_t v = mem::r64(stack);
        stack += 8;
        return v;
    }
    double next_double() {
        if (va) {
            uint8_t f = mem::r8(va_ptr + 1);
            if (f < 8) {
                mem::w8(va_ptr + 1, f + 1);
                return bitsd(mem::r64(mem::r32(va_ptr + 8) + 32 + 8 * f));
            }
            uint32_t a = (mem::r32(va_ptr + 4) + 7) & ~7u;
            mem::w32(va_ptr + 4, a + 8);
            return bitsd(mem::r64(a));
        }
        if (fpr <= 8) return c->f[fpr++];
        stack = (stack + 7) & ~7u;
        double v = bitsd(mem::r64(stack));
        stack += 8;
        return v;
    }
};

std::string guest_format(const char* fmt, VaSource& src) {
    std::string out;
    if (!fmt) return out;
    char buf[512];
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            out += *p;
            continue;
        }
        const char* start = p++;
        while (*p && strchr("-+ #0", *p)) p++;
        int star_w = -1, star_p = -1;
        if (*p == '*') { star_w = (int)src.next_u32(); p++; }
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') {
            p++;
            if (*p == '*') { star_p = (int)src.next_u32(); p++; }
            while (*p >= '0' && *p <= '9') p++;
        }
        int longs = 0;
        while (*p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'q' || *p == 'L') {
            if (*p == 'l' || *p == 'q' || *p == 'j') longs++;
            p++;
        }
        char conv = *p;
        if (!conv) break;
        std::string spec(start, p - start);
        std::string clean;  // strip length modifiers, re-add what host printf needs
        for (char ch : spec)
            if (!strchr("lhzjtqL", ch)) clean += ch;
        if (star_w >= 0 || star_p >= 0) {
            // expand '*' into literal numbers
            std::string e;
            bool prec = false;
            for (char ch : clean) {
                if (ch == '.') prec = true;
                if (ch == '*') e += std::to_string(prec ? star_p : star_w);
                else e += ch;
            }
            clean = e;
        }
        switch (conv) {
            case 'd': case 'i':
                if (longs >= 2) snprintf(buf, sizeof buf, (clean + "lld").c_str(), (long long)src.next_u64());
                else snprintf(buf, sizeof buf, (clean + "d").c_str(), (int)src.next_u32());
                out += buf;
                break;
            case 'u': case 'x': case 'X': case 'o':
                if (longs >= 2) snprintf(buf, sizeof buf, (clean + "ll" + conv).c_str(), (unsigned long long)src.next_u64());
                else snprintf(buf, sizeof buf, (clean + conv).c_str(), src.next_u32());
                out += buf;
                break;
            case 'c':
                snprintf(buf, sizeof buf, (clean + "c").c_str(), (int)src.next_u32());
                out += buf;
                break;
            case 'p':
                snprintf(buf, sizeof buf, "%08x", src.next_u32());
                out += buf;
                break;
            case 's': {
                uint32_t a = src.next_u32();
                snprintf(buf, sizeof buf, (clean + "s").c_str(), a ? (const char*)mem::host(a) : "(null)");
                out += buf;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
                snprintf(buf, sizeof buf, (clean + conv).c_str(), src.next_double());
                out += buf;
                break;
            case '%':
                out += '%';
                break;
            case 'n':
                src.next_u32();
                break;
            default:
                out += spec + conv;
        }
    }
    return out;
}

void emit(const char* tag, std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (!s.empty()) LOG("[%s] %s", tag, s.c_str());
}
}  // namespace

HLE_EXPORT(coreinit, OSReport) {
    VaSource src{c, 4, 1, c->r[1] + 8, false, 0};
    emit("OSReport", guest_format(mem::str(ARG(0)), src));
}
HLE_EXPORT(coreinit, OSVReport) {
    VaSource src{c, 0, 0, 0, true, ARG(1)};
    emit("OSReport", guest_format(mem::str(ARG(0)), src));
}
HLE_EXPORT(coreinit, COSWarn) {
    VaSource src{c, 5, 1, c->r[1] + 8, false, 0};
    emit("COSWarn", guest_format(mem::str(ARG(1)), src));
}
HLE_EXPORT(coreinit, OSConsoleWrite) {
    emit("console", std::string(mem::str(ARG(0)), ARG(1)));
}
HLE_EXPORT(coreinit, OSPanic) {
    VaSource src{c, 6, 1, c->r[1] + 8, false, 0};
    LOG("FATAL: OSPanic %s:%u: %s", mem::str(ARG(0)), ARG(1), guest_format(mem::str(ARG(2)), src).c_str());
    rt_trap(c, c->lr, "OSPanic");
}
HLE_EXPORT(coreinit, OSFatal) {
    LOG("FATAL: OSFatal: %s", mem::str(ARG(0)));
    rt_trap(c, c->lr, "OSFatal");
}

HLE_EXPORT(coreinit, OSIsDebuggerPresent) { RET(0); }
HLE_EXPORT(coreinit, OSIsDebuggerInitialized) { RET(0); }
HLE_EXPORT(coreinit, OSGetShutdownReason) { RET(0); }
HLE_EXPORT(coreinit, OSSavesDone_ReadyToRelease) {}
HLE_EXPORT(coreinit, OSEnableHomeButtonMenu) { RET(1); }
HLE_EXPORT(coreinit, OSIsHomeButtonMenuEnabled) { RET(1); }
HLE_EXPORT(coreinit, IMEnableDim) { RET(0); }
HLE_EXPORT(coreinit, IMDisableDim) { RET(0); }

// OSSystemInfo: +0 bus clock, +4 core clock, +8 ticks at boot (u64), +0x10 L2 sizes...
HLE_EXPORT(coreinit, OSGetSystemInfo) {
    static uint32_t info = 0;
    if (!info) {
        info = mem::sys_alloc(0x20, 8);
        mem::w32(info + 0, 248625000);
        mem::w32(info + 4, 1243125000);
        mem::w64(info + 8, 0);
        mem::w32(info + 0x10, 0x80000);
        mem::w32(info + 0x14, 0x200000);
        mem::w32(info + 0x18, 0x80000);
    }
    RET(info);
}

// int ENVGetEnvironmentVariable(const char* name, char* buf, uint32_t size): nothing is set
HLE_EXPORT(coreinit, ENVGetEnvironmentVariable) {
    if (ARG(1) && ARG(2)) mem::w8(ARG(1), 0);
    RET(1);
}

HLE_EXPORT(coreinit, __gh_set_errno) {
    threads::GuestThread* t = c && c->thread ? (threads::GuestThread*)c->thread : threads::current();
    if (t) mem::w32(t->os + 0x300, ARG(0));
}

static __attribute__((noinline)) uint32_t alloc_tls_block(threads::GuestThread* t) {
    const loader::Module& m = loader::main_module();
    uint32_t block = mem::sys_alloc(std::max<uint32_t>(m.tls_size, 16) + 16, 16);
    if (m.tls_image) {
        mem::commit(block, m.tls_image_size, true);
        mem::commit(m.tls_image, m.tls_image_size, false);
        memcpy(mem::host(block), mem::host(m.tls_image), m.tls_image_size);
    }
    if (t) t->tls_block = block;
    return block;
}

// void* __tls_get_addr(tls_index* ti): ti = {module, offset}
HLE_EXPORT(coreinit, __tls_get_addr) {
    threads::GuestThread* t = c && c->thread ? (threads::GuestThread*)c->thread : threads::current();
    uint32_t block = t ? t->tls_block : 0;
    if (__builtin_expect(!block, 0)) block = alloc_tls_block(t);
    RET(block + mem::r32(ARG(0) + 4));
}

HLE_EXPORT(coreinit, exit) {
    LOG("guest exit(%d)", (int)ARG(0));
    os::request_exit((int)ARG(0));
}
HLE_EXPORT(coreinit, _Exit) {
    LOG("guest _Exit(%d)", (int)ARG(0));
    os::request_exit((int)ARG(0));
}

// ---- OSDynLoad: libraries are HLE; handles are small integers naming the library -------------
namespace {
std::mutex g_dl_lock;
std::vector<std::string> g_dl_libs;
uint32_t g_dl_alloc = 0, g_dl_free = 0;
}  // namespace
// int OSDynLoad_Acquire(const char* name, OSDynLoadModule* handleOut)
HLE_EXPORT(coreinit, OSDynLoad_Acquire) {
    std::string name = mem::str(ARG(0));
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".rpl") == 0) name.resize(name.size() - 4);
    std::lock_guard<std::mutex> l(g_dl_lock);
    uint32_t h = 0;
    for (size_t i = 0; i < g_dl_libs.size(); i++)
        if (g_dl_libs[i] == name) h = 0x100 + (uint32_t)i;
    if (!h) {
        g_dl_libs.push_back(name);
        h = 0x100 + (uint32_t)g_dl_libs.size() - 1;
    }
    LOG("OSDynLoad_Acquire(%s) -> %x", name.c_str(), h);
    mem::w32(ARG(1), h);
    RET(0);
}
// int OSDynLoad_FindExport(handle, BOOL isData, const char* name, void** out)
HLE_EXPORT(coreinit, OSDynLoad_FindExport) {
    uint32_t h = ARG(0), is_data = ARG(1), out = ARG(3);
    const char* name = mem::str(ARG(2));
    std::string lib;
    {
        std::lock_guard<std::mutex> l(g_dl_lock);
        if (h >= 0x100 && h - 0x100 < g_dl_libs.size()) lib = g_dl_libs[h - 0x100];
    }
    if (lib.empty() || is_data) {
        LOG("OSDynLoad_FindExport(%s, %s, data=%u): not found", lib.c_str(), name, is_data);
        mem::w32(out, 0);
        RET(0xBAD10001);
        return;
    }
    bool have = hle::find(lib.c_str(), name) != nullptr;
    uint32_t a = dispatch::export_thunk(lib.c_str(), name);
    LOG("OSDynLoad_FindExport(%s, %s) -> %08x%s", lib.c_str(), name, a, have ? "" : " (unimplemented)");
    mem::w32(out, a);
    RET(0);
}
HLE_EXPORT(coreinit, OSDynLoad_Release) {}
HLE_EXPORT(coreinit, OSDynLoad_SetAllocator) {
    g_dl_alloc = ARG(0);
    g_dl_free = ARG(1);
    RET(0);
}

// ---- user config / MCP ---------------------------------------------------------------------------
// UCOpen() -> handle; UCReadSysConfig(handle, count, UCSysConfig* settings): entries of 0x54 bytes:
// name[64], access u32, dataType u32, error s32, dataSize u32, data ptr
HLE_EXPORT(coreinit, UCOpen) { RET(1); }
HLE_EXPORT(coreinit, UCClose) {}
HLE_EXPORT(coreinit, UCReadSysConfig) {
    uint32_t n = ARG(1), arr = ARG(2);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = arr + i * 0x54;
        std::string name = mem::str(e);
        uint32_t size = mem::r32(e + 0x4C), data = mem::r32(e + 0x50);
        uint32_t v = 0;
        bool known = true;
        if (name == "cafe.language") v = region::get().language;
        else if (name == "cafe.cntry_reg") v = region::get().country;
        else if (name == "cafe.eula_agree") v = 1;
        else if (name == "cafe.initial_launch") v = 0;
        else if (name == "parent.enable") v = 0;
        else known = false;
        if (data && size) {
            mem::commit(data, size, true);
            memset(mem::host(data), 0, size);
            if (size == 4) mem::w32(data, v);
            else if (size == 2) mem::w16(data, (uint16_t)v);
            else if (size == 1) mem::w8(data, (uint8_t)v);
        }
        mem::w32(e + 0x48, 0);
        if (!known) LOG("UCReadSysConfig: %s (size %u) -> 0", name.c_str(), size);
        else TRACE("UCReadSysConfig: %s (size %u) -> %u", name.c_str(), size, v);
    }
    RET(0);
}
HLE_EXPORT(coreinit, MCP_Open) { RET(1); }
HLE_EXPORT(coreinit, MCP_Close) {}
// MCP_GetSysProdSettings(handle, MCPSysProdSettings*): +0 platform region, +4 game region
HLE_EXPORT(coreinit, MCP_GetSysProdSettings) {
    uint32_t s = ARG(1);
    mem::commit(s, 0x46, true);
    memset(mem::host(s), 0, 0x46);
    mem::w32(s + 0, region::get().code);  // 1 Japan, 2 USA, 4 Europe
    mem::w32(s + 4, region::get().code);
    RET(0);
}
