// Guest address -> host function, HLE thunks, traps.
#include "botw_target.h"
#if defined(__APPLE__) || defined(__linux__)
#include <execinfo.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "guest_table.h"
#include "diag.h"
#include "hle.h"
#include "thread.h"

std::atomic<bool> g_has_guest_fn_overrides{false};

namespace {
std::map<std::string, GuestFn>& registry() {
    static std::map<std::string, GuestFn> r;
    return r;
}
std::string key(const char* lib, const char* name) { return std::string(lib) + "." + name; }

#if defined(_WIN32)
extern "C" char __ImageBase[];
// Static 32-bit RVA table in BSS (38.7 MB vs 76.6 MB 64-bit pointer table): halves L3/DTLB footprint
// and uses RIP-relative indexing on the hot indirect-call path.
constexpr uint32_t kMaxTextSpan = 0x02500000u;
alignas(64) static uint32_t g_text_rva[kMaxTextSpan >> 2];
#endif
GuestFn* g_text_table = nullptr;  // (addr - kText) >> 2
uint32_t g_text_end = 0;
uint32_t g_text_span = 0;
std::shared_mutex g_lock;
GuestFn g_thunks[mem::kThunkSize / 4];
const char* g_thunk_names[mem::kThunkSize / 4];
uint32_t g_next_thunk = 0;
// RPX import stubs live at 0xC0000000+; cover 1 MB (262,144 entries) with a flat O(1) table.
constexpr uint32_t kStubBase = 0xC0000000u;
constexpr uint32_t kStubSize = 0x00100000u;
GuestFn g_stub_table[kStubSize / 4];
std::unordered_map<uint32_t, GuestFn> g_stub_fns;
std::unordered_map<uint32_t, std::string> g_stub_names;
std::map<std::string, uint32_t> g_export_thunks;

struct CacheEntry {
    uint32_t addr;
    GuestFn fn;
};
thread_local CacheEntry t_cache[1024];
thread_local uint32_t t_current_dispatch_addr = 0;

struct SavedFnOverride {
    uint32_t idx = 0;
    GuestFn orig_fn = nullptr;
    uint32_t orig_rva = 0;
};
std::vector<SavedFnOverride> g_saved_fn_overrides;
std::unordered_map<uint32_t, uint32_t> g_fn_ret_imms;
std::unordered_map<uint32_t, uint32_t> g_fn_redirects;

void stub_fn_nop(PPCContext* /*c*/) {}
void stub_fn_ret_0(PPCContext* c) { c->r[3] = 0; }
void stub_fn_ret_1(PPCContext* c) { c->r[3] = 1; }
void stub_fn_ret_minus1(PPCContext* c) { c->r[3] = 0xFFFFFFFFu; }
void stub_fn_ret_imm(PPCContext* c) {
    std::shared_lock<std::shared_mutex> l(g_lock);
    auto it = g_fn_ret_imms.find(t_current_dispatch_addr);
    c->r[3] = (it != g_fn_ret_imms.end()) ? it->second : 0u;
}
void stub_fn_redirect(PPCContext* c) {
    uint32_t target = 0;
    {
        std::shared_lock<std::shared_mutex> l(g_lock);
        auto it = g_fn_redirects.find(t_current_dispatch_addr);
        if (it == g_fn_redirects.end()) return;
        target = it->second;
    }
    rt_call(c, target);
}

__attribute__((noinline)) GuestFn lookup_slow(uint32_t addr) {
    t_current_dispatch_addr = addr;
    uint32_t off = addr - mem::kText;
    if (g_text_table && off < g_text_span) {
        GuestFn fn = g_text_table[off >> 2];
        if (fn) return fn;
    }
    if (addr - mem::kThunkBase < mem::kThunkSize) return g_thunks[(addr - mem::kThunkBase) >> 2];
    if (addr - kStubBase < kStubSize) {
        GuestFn fn = g_stub_table[(addr - kStubBase) >> 2];
        if (fn) return fn;
    }
    CacheEntry& ce = t_cache[(addr >> 2) & 1023];
    if (ce.addr == addr && ce.fn) return ce.fn;
    std::shared_lock<std::shared_mutex> l(g_lock);
    auto it = g_stub_fns.find(addr);
    if (it == g_stub_fns.end()) return nullptr;
    ce = {addr, it->second};
    return it->second;
}
}  // namespace

HleRegistration::HleRegistration(const char* lib, const char* name, GuestFn fn) { registry()[key(lib, name)] = fn; }

GuestFn hle::find(const char* lib, const char* name) {
    auto it = registry().find(key(lib, name));
    return it == registry().end() ? nullptr : it->second;
}

namespace dispatch {
void init() {
    if (g_guest_function_count == 0) {
        g_text_end = mem::kText;
        g_text_span = 0;
        LOG("dispatch: no recompiled guest functions (build without generated/)");
        return;
    }
    uint32_t hi = 0;
    for (uint32_t i = 0; i < g_guest_function_count; i++) hi = std::max(hi, g_guest_functions[i].addr);
    g_text_end = hi + 4;
    g_text_span = g_text_end - mem::kText;
    size_t n = g_text_span >> 2;
    g_text_table = (GuestFn*)calloc(n, sizeof(GuestFn));
    for (uint32_t i = 0; i < g_guest_function_count; i++) {
        uint32_t idx = (g_guest_functions[i].addr - mem::kText) >> 2;
        GuestFn fn = g_guest_functions[i].fn;
        g_text_table[idx] = fn;
#if defined(_WIN32)
        if ((idx << 2) < kMaxTextSpan && fn) {
            uintptr_t diff = (uintptr_t)fn - (uintptr_t)__ImageBase;
            if (diff <= 0xFFFFFFFFu) g_text_rva[idx] = (uint32_t)diff;
        }
#endif
    }
    LOG("dispatch: %u guest functions, %zu HLE exports registered", g_guest_function_count, registry().size());
}

GuestFn lookup(uint32_t addr) {
    t_current_dispatch_addr = addr;
    uint32_t off = addr - mem::kText;
#if defined(_WIN32)
    if (__builtin_expect(off < kMaxTextSpan, 1)) {
        uint32_t rva = g_text_rva[off >> 2];
        if (__builtin_expect(rva != 0, 1)) return (GuestFn)(__ImageBase + rva);
        if (g_text_table && off < g_text_span && g_text_table[off >> 2]) return g_text_table[off >> 2];
        return nullptr;
    }
#else
    if (__builtin_expect(off < g_text_span, 1)) return g_text_table[off >> 2];
#endif
    return lookup_slow(addr);
}

uint32_t thunk(GuestFn fn, const char* name) {
    std::unique_lock<std::shared_mutex> l(g_lock);
    uint32_t i = g_next_thunk++;
    if (i >= mem::kThunkSize / 4) {
        LOG("FATAL: out of HLE thunks");
        abort();
    }
    g_thunks[i] = fn;
    g_thunk_names[i] = name;
    uint32_t a = mem::kThunkBase + i * 4;
    mem::w32(a, 0x4E800020);  // blr, in case anything reads the code
    return a;
}

static void trap_unimplemented_thunk(PPCContext* c);
static thread_local const char* t_last_unimpl = nullptr;

uint32_t export_thunk(const char* lib, const char* name) {
    std::string k = key(lib, name);
    {
        std::shared_lock<std::shared_mutex> l(g_lock);
        auto it = g_export_thunks.find(k);
        if (it != g_export_thunks.end()) return it->second;
    }
    GuestFn fn = hle::find(lib, name);
    const char* nm = strdup(k.c_str());
    uint32_t a = thunk(fn ? fn : trap_unimplemented_thunk, nm);
    std::unique_lock<std::shared_mutex> l(g_lock);
    g_export_thunks[k] = a;
    return a;
}

const char* thunk_name(uint32_t addr) {
    if (addr >= mem::kThunkBase && addr < mem::kThunkBase + mem::kThunkSize) {
        uint32_t i = (addr - mem::kThunkBase) >> 2;
        return i < g_next_thunk ? g_thunk_names[i] : nullptr;
    }
    std::shared_lock<std::shared_mutex> l(g_lock);
    auto it = g_stub_names.find(addr);
    return it == g_stub_names.end() ? nullptr : it->second.c_str();
}

void add_import_stub(uint32_t addr, GuestFn fn, const char* lib, const char* name) {
    std::unique_lock<std::shared_mutex> l(g_lock);
    if (addr - kStubBase < kStubSize) {
        g_stub_table[(addr - kStubBase) >> 2] = fn;
    }
    g_stub_fns[addr] = fn;
    g_stub_names[addr] = key(lib, name);
}

static void save_and_set_fn_override_locked(uint32_t idx, GuestFn new_fn) {
    bool already_saved = false;
    for (const auto& s : g_saved_fn_overrides) {
        if (s.idx == idx) {
            already_saved = true;
            break;
        }
    }
    if (!already_saved) {
        SavedFnOverride sv;
        sv.idx = idx;
        sv.orig_fn = g_text_table[idx];
#if defined(_WIN32)
        if ((idx << 2) < kMaxTextSpan) sv.orig_rva = g_text_rva[idx];
#endif
        g_saved_fn_overrides.push_back(sv);
    }
    g_text_table[idx] = new_fn;
#if defined(_WIN32)
    if ((idx << 2) < kMaxTextSpan) {
        uintptr_t diff = (uintptr_t)new_fn - (uintptr_t)__ImageBase;
        g_text_rva[idx] = (new_fn && diff <= 0xFFFFFFFFu) ? (uint32_t)diff : 0u;
    }
#endif
}

bool is_function_entry(uint32_t addr) {
    if (!g_text_table || (addr & 3) != 0) return false;
    uint32_t off = addr - mem::kText;
    if (off >= g_text_span) return false;
    return g_text_table[off >> 2] != nullptr;
}

void override_fn_nop(uint32_t addr) {
    if (!is_function_entry(addr)) return;
    std::unique_lock<std::shared_mutex> l(g_lock);
    g_fn_redirects.erase(addr);
    g_fn_ret_imms.erase(addr);
    save_and_set_fn_override_locked((addr - mem::kText) >> 2, stub_fn_nop);
    g_has_guest_fn_overrides.store(true, std::memory_order_release);
}

void override_fn_ret_imm(uint32_t addr, uint32_t imm) {
    if (!is_function_entry(addr)) return;
    std::unique_lock<std::shared_mutex> l(g_lock);
    g_fn_redirects.erase(addr);
    g_fn_ret_imms.erase(addr);
    GuestFn fn = stub_fn_ret_imm;
    if (imm == 0) fn = stub_fn_ret_0;
    else if (imm == 1) fn = stub_fn_ret_1;
    else if (imm == 0xFFFFFFFFu) fn = stub_fn_ret_minus1;
    else g_fn_ret_imms[addr] = imm;
    save_and_set_fn_override_locked((addr - mem::kText) >> 2, fn);
    g_has_guest_fn_overrides.store(true, std::memory_order_release);
}

bool redirect_fn(uint32_t addr, uint32_t target) {
    if (!is_function_entry(addr) || addr == target) return false;
    GuestFn target_fn = lookup(target);
    if (!target_fn) return false;
    std::unique_lock<std::shared_mutex> l(g_lock);
    uint32_t next = target;
    for (size_t steps = 0; steps <= g_fn_redirects.size(); ++steps) {
        if (next == addr) return false;
        auto it = g_fn_redirects.find(next);
        if (it == g_fn_redirects.end()) break;
        next = it->second;
        if (steps == g_fn_redirects.size()) return false;
    }
    g_fn_ret_imms.erase(addr);
    g_fn_redirects[addr] = target;
    save_and_set_fn_override_locked((addr - mem::kText) >> 2, stub_fn_redirect);
    g_has_guest_fn_overrides.store(true, std::memory_order_release);
    return true;
}

void restore_fn_overrides() {
    std::unique_lock<std::shared_mutex> l(g_lock);
    if (g_text_table) {
        for (auto it = g_saved_fn_overrides.rbegin(); it != g_saved_fn_overrides.rend(); ++it) {
            g_text_table[it->idx] = it->orig_fn;
#if defined(_WIN32)
            if ((it->idx << 2) < kMaxTextSpan) g_text_rva[it->idx] = it->orig_rva;
#endif
        }
    }
    g_saved_fn_overrides.clear();
    g_fn_ret_imms.clear();
    g_fn_redirects.clear();
    g_has_guest_fn_overrides.store(false, std::memory_order_release);
}

static void trap_unimplemented_thunk(PPCContext* c) {
    rt_trap(c, c->lr, "call to an unimplemented dynamically-loaded export (see thunk name above)");
}
}  // namespace dispatch

extern "C" bool rt_try_override(PPCContext* c, uint32_t addr, GuestFn original) {
    GuestFn selected = nullptr;
    {
        std::shared_lock<std::shared_mutex> l(g_lock);
        uint32_t off = addr - mem::kText;
        if (g_text_table && off < g_text_span && (addr & 3u) == 0)
            selected = g_text_table[off >> 2];
    }
    if (!selected || selected == original) return false;
    t_current_dispatch_addr = addr;
    selected(c);
    return true;
}

// ---- guest calls -----------------------------------------------------------------------------
// Save and restore only the volatile state that a PowerPC EABI callee may clobber (r0, r3..r12,
// f0..f13, ps1[0..13], cr[0..1], cr[5..7], lr, ctr, xer, fpscr, res_addr/res_val), rather than
// copying the entire 1080-byte PPCContext on every host->guest callback.
uint32_t guest_call(PPCContext* c, uint32_t fn, std::initializer_list<uint32_t> args) {
    PPCContext saved = *c;
    int k = 3;
    for (uint32_t a : args) c->r[k++] = a;
    c->lr = 0;  // returns come back here through the C++ call stack, never via LR
    rt_call(c, fn);
    uint32_t r = c->r[3];
    *c = saved;
    return r;
}

// ---- runtime entry points used by generated code -----------------------------------------------
static __attribute__((noinline)) void rt_call_slow(PPCContext* c, uint32_t target) {
    GuestFn fn = lookup_slow(target);
    if (__builtin_expect(!fn, 0)) rt_trap(c, target, "indirect call to an address with no recompiled function");
    fn(c);
}

extern "C" void rt_call(PPCContext* c, uint32_t target) {
    t_current_dispatch_addr = target;
    uint32_t off = target - mem::kText;
#if defined(_WIN32)
    if (__builtin_expect(off < kMaxTextSpan, 1)) {
        uint32_t rva = g_text_rva[off >> 2];
        if (__builtin_expect(rva != 0, 1)) {
            GuestFn fn = (GuestFn)(__ImageBase + rva);
            fn(c);
            return;
        }
    }
#else
    if (__builtin_expect(off < g_text_span, 1)) {
        GuestFn fn = g_text_table[off >> 2];
        if (__builtin_expect(fn != nullptr, 1)) {
            fn(c);
            return;
        }
    }
#endif
    rt_call_slow(c, target);
}

static void dump_context(PPCContext* c) {
    if (!c) return;
    for (int i = 0; i < 32; i += 8)
        LOG("  r%-2d %08x %08x %08x %08x %08x %08x %08x %08x", i, c->r[i], c->r[i + 1], c->r[i + 2], c->r[i + 3],
            c->r[i + 4], c->r[i + 5], c->r[i + 6], c->r[i + 7]);
    LOG("  lr %08x ctr %08x", c->lr, c->ctr);
    // guest stack back-chain: [sp] = caller's sp, [caller sp + 4] = saved LR
    uint32_t sp = c->r[1];
    for (int i = 0; i < 24 && sp > 0x10000 && sp < 0xFFFF0000u; i++) {
        uint32_t next = mem::r32(sp);
        if (next <= sp || next - sp > 0x100000) break;
        uint32_t lr = mem::r32(next + 4);
        LOG("  guest frame %2d: sp %08x saved lr %08x", i, next, lr);
        sp = next;
    }
}

// Host backtrace: raw addresses plus the image base; tools/symbolize.sh turns them into guest
// addresses with atos (generated code carries `#line <guest address>` for every instruction).
static void host_backtrace() {
#if defined(__APPLE__) || defined(__linux__)
    void* frames[48];
    int n = backtrace(frames, 48);
    char line[2048];
    int k = snprintf(line, sizeof line, "  host frames (base 0x%llx):", (unsigned long long)(uintptr_t)botw_image_base());
    for (int i = 0; i < n && k < (int)sizeof line - 24; i++) k += snprintf(line + k, sizeof line - k, " %p", frames[i]);
    LOG("%s", line);
#elif defined(_WIN32)
    void* frames[48];
    USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    char line[2048];
    int k = snprintf(line, sizeof line, "  host frames (base 0x%llx):", (unsigned long long)(uintptr_t)botw_image_base());
    for (USHORT i = 0; i < n && k < (int)sizeof line - 24; i++) k += snprintf(line + k, sizeof line - k, " %p", frames[i]);
    LOG("%s", line);
#else
    LOG("  host frames unavailable");
#endif
}

extern "C" PPC_NORETURN void rt_fatal_exit(void);

extern "C" PPC_NORETURN void rt_trap(PPCContext* c, uint32_t addr, const char* why) {
    diag_dump("fatal");
    const char* tn = threads::current_name();
    LOG("FATAL: %s at %08x (thread %s)", why, addr, tn ? tn : "?");
    if (const char* n = dispatch::thunk_name(addr)) LOG("  target is %s", n);
    dump_context(c);
    host_backtrace();
    rt_fatal_exit();
}

extern "C" PPC_NORETURN void rt_unimplemented(PPCContext* c, const char* lib, const char* name) {
    LOG("FATAL: unimplemented import %s.%s (called from %08x)", lib, name, c->lr - 4);
    dump_context(c);
    rt_fatal_exit();
}

extern "C" PPC_NORETURN void rt_bad_return(PPCContext* c, uint32_t addr, uint32_t lr0) {
    LOG("FATAL: blr at %08x with LR %08x but the function was entered with LR %08x (computed jump/longjmp?)",
        addr, c->lr, lr0);
    dump_context(c);
    host_backtrace();
    rt_fatal_exit();
}

extern "C" void rt_bad_switch(PPCContext* c, uint32_t addr, uint32_t target) {
    rt_trap(c, addr, "switch jump to an address outside its table");
}

extern "C" void rt_probe(PPCContext* c, uint32_t addr) {
    LOG("probe %08x: r3=%08x r4=%08x r5=%08x r6=%08x r31=%08x lr=%08x", addr, c->r[3], c->r[4], c->r[5], c->r[6],
        c->r[31], c->lr);
}

// --trace-hle: every import call with arguments and result
extern "C" int g_trace_hle = 0;
extern "C" void rt_trace_hle(PPCContext* c, const char* name, int ret) {
    if (!ret)
        LOG("[%s] %s(%08x, %08x, %08x, %08x, %08x) from %08x", threads::current_name() ? threads::current_name() : "?",
            name, c->r[3], c->r[4], c->r[5], c->r[6], c->r[7], c->lr - 4);
    else
        LOG("[%s]   %s -> %08x", threads::current_name() ? threads::current_name() : "?", name, c->r[3]);
}
