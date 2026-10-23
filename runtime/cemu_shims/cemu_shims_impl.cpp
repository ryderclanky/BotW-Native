// BotW-Native: implementations of the Cemu subsystem shims used by the vendored GPU code.
#include <time.h>

#include "Cafe/CafeSystem.h"
#include "Cafe/GameProfile/GameProfile.h"
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "WindowSystem.h"
#include "config/ActiveSettings.h"

extern "C" uint64_t rt_timebase(void);
extern std::string g_user_root;

// ---- configuration --------------------------------------------------------------------------
CemuConfig& GetConfig() {
    static CemuConfig c;
    return c;
}
static GameProfile s_profile;
GameProfile* g_current_game_profile = &s_profile;
std::atomic_bool g_isGPUInitFinished = false;

fs::path botw_user_root() { return fs::path(g_user_root); }
fs::path botw_cache_root() {
    // Cemu's launcher creates these; the Metal shader / pipeline caches are written there
    static bool made = [] {
        std::error_code ec;
        fs::create_directories(fs::path(g_user_root) / "cache/shaderCache/transferable", ec);
        fs::create_directories(fs::path(g_user_root) / "cache/shaderCache/precompiled", ec);
        return true;
    }();
    (void)made;
    return fs::path(g_user_root) / "cache";
}
bool ActiveSettings::DumpShadersEnabled() { return getenv("BOTW_DUMP_SHADERS") != nullptr; }
std::string CafeSystem::GetMlcStoragePath(uint64_t) { return g_user_root + "/mlc"; }

// ---- memory (1:1 virtual/physical like Cemu) ------------------------------------------------
std::vector<struct MMURange*> g_mmuRanges;
MMURange::MMURange(const uint32 baseAddress, const uint32 size, MMU_MEM_AREA_ID areaId, const std::string_view name, MFLAG flags)
    : baseAddress(baseAddress), size(size), initSize(size), areaId(areaId), name(name), flags(flags) {
    g_mmuRanges.emplace_back(this);
    m_isMapped = true;  // the whole 4 GB guest space is mapped up front (runtime/src/mem.cpp)
}
void MMURange::mapMem() { m_isMapped = true; }
void MMURange::unmapMem() {}
MMURange mmuRange_LOW0{0x00010000, 0x000F0000, MMU_MEM_AREA_ID::CODE_LOW0, "CODE_LOW0"};
MMURange mmuRange_TRAMPOLINE_AREA{0x00E00000, 0x00200000, MMU_MEM_AREA_ID::CODE_TRAMPOLINE, "TRAMPOLINE_AREA"};
MMURange mmuRange_CODECAVE{0x01800000, 0x00400000, MMU_MEM_AREA_ID::CODE_CAVE, "CODECAVE"};
MMURange mmuRange_TEXT_AREA{0x02000000, 0x0C000000, MMU_MEM_AREA_ID::CODE_MAIN, "TEXT_AREA"};
MMURange mmuRange_CEMU_AREA{0x0E000000, 0x02000000, MMU_MEM_AREA_ID::CEMU_PRIVATE, "CEMU_AREA"};
MMURange mmuRange_MEM2{0x10000000, 0x40000000, MMU_MEM_AREA_ID::MEM2_DATA, "MEM2"};
MMURange mmuRange_OVERLAY_AREA{0xA0000000, 0x1C000000, MMU_MEM_AREA_ID::OVERLAY, "OVERLAY_AREA"};
MMURange mmuRange_FGBUCKET{0xE0000000, 0x04000000, MMU_MEM_AREA_ID::FGBUCKET, "FGBUCKET"};
MMURange mmuRange_TILINGAPERTURE{0xE8000000, 0x02000000, MMU_MEM_AREA_ID::TILING_APERATURE, "TILINGAPERTURE"};
MMURange mmuRange_MEM1{0xF4000000, 0x02000000, MMU_MEM_AREA_ID::MEM1, "MEM1"};
MMURange mmuRange_RPLLOADER{0xF6000000, 0x02000000, MMU_MEM_AREA_ID::RPLLOADER, "RPLLOADER_AREA"};
MMURange mmuRange_SHARED_AREA{0xF8000000, 0x02000000, MMU_MEM_AREA_ID::SHAREDDATA, "SHARED_AREA"};
MMURange mmuRange_CORE0_LC{0xFFC00000, 0x00005000, MMU_MEM_AREA_ID::CPU_LC0, "CORE0_LC"};
MMURange mmuRange_CORE1_LC{0xFFC40000, 0x00005000, MMU_MEM_AREA_ID::CPU_LC1, "CORE1_LC"};
MMURange mmuRange_CORE2_LC{0xFFC80000, 0x00005000, MMU_MEM_AREA_ID::CPU_LC2, "CORE2_LC"};
std::vector<MMURange*> memory_getMMURanges() { return g_mmuRanges; }
MMURange* memory_getMMURangeByAddress(MPTR address) {
    for (auto& r : g_mmuRanges)
        if (address >= r->getBase() && address < r->getEnd()) return r;
    return nullptr;
}
bool memory_isAddressRangeAccessible(MPTR a, uint32 size) { return a >= 0x10000; }
uint32 memory_virtualToPhysical(uint32 v) { return v; }
uint32 memory_physicalToVirtual(uint32 p) { return p; }
uint8* memory_getPointerFromPhysicalOffset(uint32 p) { return memory_base + p; }
uint32 memory_getVirtualOffsetFromPointer(void* ptr) { return ptr ? (uint32)((uint8*)ptr - memory_base) : 0; }
uint8* memory_getPointerFromVirtualOffset(uint32 v) { return memory_base + v; }
uint8* memory_getPointerFromVirtualOffsetAllowNull(uint32 v) { return v ? memory_base + v : nullptr; }
void memory_writeDouble(uint32 a, double v) { uint64 u; memcpy(&u, &v, 8); *(uint64*)(memory_base + a) = __builtin_bswap64(u); }
void memory_writeFloat(uint32 a, float v) { uint32 u; memcpy(&u, &v, 4); *(uint32*)(memory_base + a) = __builtin_bswap32(u); }
void memory_writeU32(uint32 a, uint32 v) { *(uint32*)(memory_base + a) = __builtin_bswap32(v); }
void memory_writeU16(uint32 a, uint16 v) { *(uint16*)(memory_base + a) = __builtin_bswap16(v); }
void memory_writeU8(uint32 a, uint8 v) { memory_base[a] = v; }
void memory_writeU64(uint32 a, uint64 v) { *(uint64*)(memory_base + a) = __builtin_bswap64(v); }
double memory_readDouble(uint32 a) { uint64 u = __builtin_bswap64(*(uint64*)(memory_base + a)); double d; memcpy(&d, &u, 8); return d; }
float memory_readFloat(uint32 a) { uint32 u = __builtin_bswap32(*(uint32*)(memory_base + a)); float f; memcpy(&f, &u, 4); return f; }
uint64 memory_readU64(uint32 a) { return __builtin_bswap64(*(uint64*)(memory_base + a)); }
uint32 memory_readU32(uint32 a) { return __builtin_bswap32(*(uint32*)(memory_base + a)); }
uint16 memory_readU16(uint32 a) { return __builtin_bswap16(*(uint16*)(memory_base + a)); }
uint8 memory_readU8(uint32 a) { return memory_base[a]; }

// ---- CPU / time ---------------------------------------------------------------------------------
uint64 PPCInterpreter_getMainCoreCycleCounter() { return rt_timebase() * 20; }  // core clock = 20x timer
uint32 PPCInterpreter_getCurrentCoreIndex() { return 1; }
uint64 coreinit::OSGetSystemTime() { return rt_timebase(); }
uint64 coreinit::OSGetTime() { return rt_timebase(); }
void DebugLogStackTrace(struct OSThread_t* thread, MPTR sp) {}

// ---- window -----------------------------------------------------------------------------------
namespace WindowSystem {
WindowInfo& GetWindowInfo() {
    static WindowInfo w;
    return w;
}
}  // namespace WindowSystem
void botw_window_phys_size(int& w, int& h);  // runtime/src/host/window.mm
void WindowSystem::GetWindowPhysSize(int& w, int& h) { botw_window_phys_size(w, h); }
