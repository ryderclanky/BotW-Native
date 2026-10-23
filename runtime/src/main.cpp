// BotWRecomp entry: map guest memory, load U-King.rpx, initialise Cafe OS state and run the game's
// entry point on its own host thread. The host main thread stays free for the window/event loop.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <exception>
#include <filesystem>
#include <string>

#include "hle.h"
#include "loader.h"
#include "os.h"
#include "settings.h"
#include "thread.h"
#include "host/window.h"
#include "latte/latte.h"

extern "C" int g_trace_hle;
std::string g_game_root;   // contains code/, content/, meta/
std::string g_user_root;   // saves, logs, shader cache (see default_user_root)
bool g_headless = false;
bool g_mute = false;       // no audio output (on-screen test runs)
int g_run_seconds = 0;     // --seconds N (N > 0): exit after N seconds. 0 runs until the window closes.
float g_render_scale = 0;  // --scale N: render targets N times the native 1280x720 (graphic_pack_shim.cpp); 0 = fit the display
int g_target_fps = 60;       // --fps 30: the game's own pacing (game/fps.cpp)
uint64_t g_trace_from = 0, g_trace_to = 0;  // --trace-frames A-B: HLE call trace for frames [A, B)

static std::string default_user_root() {
#if defined(_WIN32)
    if (const char* appdata = getenv("APPDATA"); appdata && appdata[0]) return std::string(appdata) + "\\BotWRecomp";
    return "BotWRecomp";
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/BotWRecomp";
#else
    if (const char* xdg = getenv("XDG_DATA_HOME"); xdg && xdg[0]) return std::string(xdg) + "/BotWRecomp";
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share/BotWRecomp";
#endif
}

#if defined(__linux__) && defined(ENABLE_VULKAN)
int BreathOfTheWildChildProcessMain();
#endif

extern "C" PPC_NORETURN void rt_fatal_exit(void) {
    fflush(stderr);
    _exit(3);
}

bool debug_watch_fault(void* addr, void* uctx);
void debug_watch_init();

#if defined(_WIN32)
static void log_windows_fault(DWORD code, void* fault_addr) {
    uintptr_t a = (uintptr_t)fault_addr, base = (uintptr_t)g_mem;
    if (g_mem && a >= base && a < base + (1ull << 32) + 0x10000)
        LOG("FATAL: exception %08lx: guest memory access at %08x", (unsigned long)code, (uint32_t)(a - base));
    else
        LOG("FATAL: exception %08lx: host access at %p", (unsigned long)code, fault_addr);
}

// First handler. Demand-commit of the reserved guest space, then the debug watch, then a trap.
static LONG CALLBACK on_vectored_exception(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    if (!rec) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = rec->ExceptionCode;
    // A Cafe assert used to DebugBreak with no log (exit 0x80000003). Record the stack. With no
    // debugger attached, step past the int3 so the process keeps running.
    if (code == EXCEPTION_BREAKPOINT && ep->ContextRecord) {
        static int logged = 0;
        if (logged < 8) {
            logged++;
            LOG("breakpoint at %p", rec->ExceptionAddress);
            threads::host_stack_log("breakpoint", ep->ContextRecord);
        }
        if (!IsDebuggerPresent()) {
            unsigned char op = 0;
            SIZE_T got = 0;
            uintptr_t pc = (uintptr_t)ep->ContextRecord->Rip;
            if (pc && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)pc, &op, 1, &got) && got == 1 && op == 0xCC) {
                ep->ContextRecord->Rip = pc + 1;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const bool access = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR;
    const bool illegal = code == EXCEPTION_ILLEGAL_INSTRUCTION;
    if (!access && !illegal) return EXCEPTION_CONTINUE_SEARCH;
    void* fault_addr = nullptr;
    if (access && rec->NumberParameters >= 2) fault_addr = (void*)rec->ExceptionInformation[1];
    if (fault_addr && mem::commit_on_fault(fault_addr)) return EXCEPTION_CONTINUE_EXECUTION;
    if (fault_addr && debug_watch_fault(fault_addr, ep->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    log_windows_fault(code, fault_addr);
    threads::GuestThread* t = threads::current();
    rt_trap(t ? &t->ctx : nullptr, 0, "windows exception (ctx registers may be stale)");
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void on_fault(int sig, siginfo_t* si, void* uctx) {
    if (debug_watch_fault(si->si_addr, uctx)) return;
    uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)g_mem;
    if (a >= base && a < base + (1ull << 32) + 0x10000)
        LOG("FATAL: signal %d: guest memory access at %08x", sig, (uint32_t)(a - base));
    else
        LOG("FATAL: signal %d: host access at %p", sig, (void*)a);
    threads::GuestThread* t = threads::current();
    rt_trap(t ? &t->ctx : nullptr, 0, "signal (ctx registers may be stale)");
}
#endif

int main(int argc, char** argv) {
#if defined(__linux__) && defined(ENABLE_VULKAN)
    // Cemu re-execs itself to see whether RADV was built with LLVM. Must run before anything else.
    if (getenv("CEMU_DETECT_RADV") != nullptr) return BreathOfTheWildChildProcessMain();
#endif
    // saves, caches, logs, settings; BOTW_USER_ROOT lets test runs use their own folder (tools/run.sh)
    if (const char* e = getenv("BOTW_USER_ROOT")) g_user_root = e;
    else g_user_root = default_user_root();
    std::error_code ec;
    std::filesystem::create_directories(g_user_root, ec);
    log_set_file((g_user_root + "/runtime.log").c_str());
#if defined(_WIN32)
    LOG("BotW Recompiled for Windows (Vulkan)");
#elif defined(__APPLE__)
    LOG("BotW Recompiled (Metal)");
#else
    LOG("BotW Recompiled (Vulkan)");
#endif
    settings::load(g_user_root + "/settings.ini");  // in-game menu settings; the flags below override them
    Settings opt = settings::get();

    int capture_every = 0;
    bool start_fullscreen = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) g_game_root = argv[++i];
        else if (a == "--trace") g_log_level = 1;
        else if (a == "--headless") g_headless = true;
        else if (a == "--mute") g_mute = true;
        else if (a == "--capture" && i + 1 < argc) capture_every = atoi(argv[++i]);
        else if (a == "--script" && i + 1 < argc) window::set_script(argv[++i]);
        else if (a == "--trace-hle") g_trace_hle = 1;
        else if (a == "--no-gyro") opt.gyro = false;
        else if (a == "--trace-frames" && i + 1 < argc) sscanf(argv[++i], "%llu-%llu", &g_trace_from, &g_trace_to);
        else if (a == "--seconds" && i + 1 < argc) {
            int n = atoi(argv[++i]);
            g_run_seconds = n > 0 ? n : 0;
        }
        else if (a == "--fps" && i + 1 < argc) opt.fps = atoi(argv[++i]) >= 60 ? 60 : 30;
        else if (a == "--renderer" && i + 1 < argc) latte::select_backend(argv[++i]);
        else if (a == "--scale" && i + 1 < argc) opt.scale = (float)atof(argv[++i]);
        else if (a == "--display" && i + 1 < argc) {  // fit (letterbox) | stretch | fill (crop to cover)
            std::string m = argv[++i];
            opt.display = m == "stretch" ? 1 : m == "fill" ? 2 : 0;
        }
        else if (a == "--fullscreen") start_fullscreen = true;
    }
    if (const char* e = getenv("BOTW_RENDERER")) latte::select_backend(e);
    settings::set(opt, false);  // this run only; the menu saves its changes
    g_render_scale = opt.scale;
    if (g_run_seconds > 0) LOG("time limit %d s", g_run_seconds);
    else LOG("no time limit; close the window to quit");
    if (g_game_root.empty()) {
        if (const char* e = getenv("BOTW_GAME")) g_game_root = e;
    }
#if defined(_WIN32)
    // Before mem::init, so the first guest touch can commit its page.
    AddVectoredExceptionHandler(1, on_vectored_exception);
#else
    struct sigaction sa = {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    signal(SIGABRT, [](int) {
        LOG("FATAL: abort()");
        _exit(6);
    });
#endif

    mem::init();
    dispatch::init();
    // The renderer does not need the game. Bring the device and swapchain up first so a machine
    // without a dump still proves the graphics backend, then stop if the RPX is missing.
    window::set_fullscreen(start_fullscreen);
    try {
        window::create();
    } catch (const std::exception& e) {
        fprintf(stderr, "graphics init failed: %s\n", e.what());
        LOG("graphics init failed: %s", e.what());
        return 1;
    }
    if (!loader::load_main(g_game_root + "/code/U-King.rpx")) {
        LOG("no game loaded. Vulkan/Metal came up; place your own dump (code/, content/, meta/) and pass --game");
        if (!latte::present_empty_frame()) LOG("device is up; presenting an empty frame did not succeed");
        fprintf(stderr, "game files required: %s --game <folder with code/ content/ meta/>\n", argv[0]);
        return 2;
    }
    os::init();
    if (g_render_scale <= 0) {  // render at the display's native resolution, so fullscreen is 1:1
        int dw, dh;
        window::display_pixel_size(&dw, &dh);
        g_render_scale = std::clamp(std::min(dw / 1280.0f, dh / 720.0f), 1.0f, 3.0f);
        LOG("render scale %.3f (%dx%d for a %dx%d display)", g_render_scale, (int)(1280 * g_render_scale),
            (int)(720 * g_render_scale), dw, dh);
    }
    debug_watch_init();
    if (capture_every) latte::set_capture(capture_every, (g_user_root + "/captures").c_str());

    uint32_t argv_g = os::build_argv("U-King.rpx");
    threads::start_main(loader::main_module().entry, 1, argv_g, loader::main_module().stack_size);
    os::host_main_loop();  // returns when the game exits or the time limit is reached
    return 0;
}
