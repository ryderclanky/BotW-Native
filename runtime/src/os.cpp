#include "os.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "coreinit/mods.h"
#include "hle.h"
#include "host/window.h"
#include "thread.h"

std::atomic<uint64_t> g_game_frames{0};

namespace os {
namespace {
std::atomic<int> g_exit_code{-1};
}

void init() {
    init_heaps();
    init_time();
}

uint32_t build_argv(const char* rpx) {
    uint32_t argv = mem::sys_alloc(16, 4);
    mem::w32(argv, mem::sys_strdup(rpx));
    mem::w32(argv + 4, 0);
    return argv;
}

void request_exit(int code) {
    g_exit_code = code;
    mods::log_shutdown();
    // give the log a moment, then leave without running host static destructors
    fflush(stderr);
    _exit(code);
}

void host_main_loop() {
    window::run();
    if (g_run_seconds) threads::dump();  // test runs: where every thread was at the end
    mods::log_shutdown();
    fflush(stderr);
    _exit(0);
}
}  // namespace os
