// BotW-Native: small symbols the vendored GPU code expects from parts of Cemu that are not vendored.
#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

#include "Cafe/HW/Latte/Core/LatteOverlay.h"

void log_write(const char* fmt, ...);
namespace mem { uint32_t sys_alloc(uint32_t size, uint32_t align); void sys_free(uint32_t addr); }

std::atomic_int g_compiling_pipelines = 0;
std::atomic_int g_compiling_pipelines_async = 0;
std::atomic_uint64_t g_compiling_pipelines_syncTimeSum = 0;

// Cemu's SysAllocator: guest memory owned by the "OS"
MPTR coreinit_allocFromSysArea(uint32 size, uint32 alignment) { return mem::sys_alloc(size, alignment < 16 ? 16 : alignment); }
void coreinit_freeToSysArea(MPTR mem) { mem::sys_free(mem); }

uint64 PPCTimer_getRawTsc() {
#if defined(__APPLE__)
    return mach_absolute_time();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64)ts.tv_sec * 1000000000ull + (uint64)ts.tv_nsec;
#endif
}

void LatteOverlay_pushNotification(const std::string& msg, sint32) { log_write("[gpu] %s", msg.c_str()); }
