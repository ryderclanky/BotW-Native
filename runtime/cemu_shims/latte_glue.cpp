// BotW-Native: glue between this runtime and the vendored Cemu Latte GPU (Cemu environment).
// Implements runtime/src/latte/latte.h on top of Cemu's command processor:
//  - GX2 submissions (latte::submit) become ring words: INDIRECT_BUFFER + EVENT_WRITE_EOP(timestamp),
//    the same pair Cemu's TCL queues; Cemu's CP drains them through TCL::TCLGPUReadRBWord.
//  - retirement, flips and vsync are reported back through TCL/GX2 notification hooks.
//
// The Metal renderer (Apple) and the Vulkan renderer (Windows, Linux, and optional on Apple) share
// this file. Apple window handles are filled in by the host; Vulkan reads them back out of
// WindowSystem when it creates the device surface and the swapchain.
//
// The native-Metal switch TU renames the latte:: entry points with macros before including this
// file. Those names (`init`, `submit`, ...) also occur in the headers below, so suspend the macros
// until the definitions.
#ifdef create_renderer
#pragma push_macro("create_renderer")
#pragma push_macro("resize_window")
#pragma push_macro("set_scanout_srgb")
#pragma push_macro("set_display_mode")
#pragma push_macro("init")
#pragma push_macro("set_capture")
#pragma push_macro("submit")
#pragma push_macro("last_submitted")
#pragma push_macro("retired")
#pragma push_macro("wait_retired")
#pragma push_macro("flip_requests")
#pragma push_macro("flips_executed")
#pragma push_macro("add_flip_request")
#pragma push_macro("wait_for_flip")
#pragma push_macro("wait_for_vsync")
#pragma push_macro("last_flip_time")
#pragma push_macro("last_vsync_time")
#pragma push_macro("swap_interval")
#pragma push_macro("set_swap_interval")
#pragma push_macro("set_event_sink")
#undef create_renderer
#undef resize_window
#undef set_scanout_srgb
#undef set_display_mode
#undef init
#undef set_capture
#undef submit
#undef last_submitted
#undef retired
#undef wait_retired
#undef flip_requests
#undef flips_executed
#undef add_flip_request
#undef wait_for_flip
#undef wait_for_vsync
#undef last_flip_time
#undef last_vsync_time
#undef swap_interval
#undef set_swap_interval
#undef set_event_sink
#define BOTW_GLUE_MACROS_SUSPENDED 1
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/gx2/GX2_Event.h"
#include "WindowSystem.h"
#include "config/CemuConfig.h"
#include "../src/latte/latte.h"

#ifdef ENABLE_METAL
#include "Cafe/HW/Latte/Renderer/Metal/LatteTextureMtl.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#endif
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#endif

void log_write(const char* fmt, ...);
void Latte_Start();
namespace mem { uint32_t sys_alloc(uint32_t size, uint32_t align); }

namespace {
std::mutex g_rb_lock;
std::deque<uint32> g_rb;
std::condition_variable g_rb_cv;  // the idle GPU thread waits here (TCLGPUWaitForWork)
bool g_rb_waiting = false;        // under g_rb_lock
std::atomic<uint64> g_last_submitted{0}, g_retired{0};
std::mutex g_retire_lock;
std::condition_variable g_retire_cv;
uint32 g_retire_slot = 0;
std::function<void(latte::Event)> g_sink;
std::mutex g_sync_lock;
std::condition_variable g_flip_cv, g_vsync_cv;
uint64 g_vsync_count = 0;
std::atomic<uint64> g_last_flip_time{0}, g_last_vsync_time{0};
std::atomic<uint64> g_frames{0};
int g_capture_every = 0;
std::atomic<uint64> g_capture_pending{0};
std::string g_capture_dir;

latte::HostSurface g_kind = latte::HostSurface::Cocoa;
void* g_display = nullptr;
void* g_surface = nullptr;
bool g_vulkan =
#if defined(ENABLE_VULKAN) && !defined(ENABLE_METAL)
    true;
#else
    false;
#endif

WindowSystem::WindowHandleInfo::Backend to_backend(latte::HostSurface kind) {
    using B = WindowSystem::WindowHandleInfo::Backend;
    switch (kind) {
        case latte::HostSurface::X11: return B::X11;
        case latte::HostSurface::Wayland: return B::Wayland;
        case latte::HostSurface::Windows: return B::Windows;
        case latte::HostSurface::Cocoa: return B::Cocoa;
    }
    return B::Cocoa;
}
}  // namespace

namespace window { void on_frame(uint64_t frame); }

#ifdef ENABLE_METAL
void botw_capture_hook_metal(LatteTextureView* view, bool isPadView, const std::string& path);
#endif

void TCL::TCLGPUWaitForWork(int max_us) {
    std::unique_lock<std::mutex> l(g_rb_lock);
    if (!g_rb.empty()) return;
    g_rb_waiting = true;
    g_rb_cv.wait_for(l, std::chrono::microseconds(max_us), [] { return !g_rb.empty(); });
    g_rb_waiting = false;
}
bool TCL::TCLGPUReadRBWord(uint32& w) {
    std::lock_guard<std::mutex> l(g_rb_lock);
    if (g_rb.empty()) return false;
    w = g_rb.front();
    g_rb.pop_front();
    return true;
}
void TCL::TCLGPUNotifyNewRetirementTimestamp() {
    uint64 ts = memory_readU64(g_retire_slot);
    {
        std::lock_guard<std::mutex> l(g_retire_lock);
        if (ts > g_retired) g_retired = ts;
    }
    g_retire_cv.notify_all();
}

namespace GX2 {
void __GX2NotifyNewRetirementTimestamp(uint64) {}
void __GX2NotifyEvent(GX2CallbackEventType e) {
    if (e == GX2CallbackEventType::VSYNC) {
        g_last_vsync_time = coreinit::OSGetSystemTime();
        std::lock_guard<std::mutex> l(g_sync_lock);
        g_vsync_count++;
        g_vsync_cv.notify_all();
    } else if (e == GX2CallbackEventType::FLIP) {
        g_last_flip_time = coreinit::OSGetSystemTime();
        uint64 f = ++g_frames;
        window::on_frame(f);
        if (g_capture_every && f % g_capture_every == 0) g_capture_pending = f;
        if (f % 300 == 0) log_write("frame %llu", (unsigned long long)f);
        std::lock_guard<std::mutex> l(g_sync_lock);
        g_flip_cv.notify_all();
    }
    if (g_sink) g_sink((latte::Event)(uint32)e);
}
}  // namespace GX2

void botw_capture_hook(LatteTextureView* view, bool isPadView) {
    uint64 f = g_capture_pending.exchange(0);
    if (!f || isPadView || !g_renderer) {
        if (f && isPadView) g_capture_pending = f;
        return;
    }
#if defined(ENABLE_METAL)
    if (g_renderer->GetType() == RendererAPI::Metal) {
        std::string path = fmt::format("{}/frame_{:06}.png", g_capture_dir, f);
        botw_capture_hook_metal(view, isPadView, path);
        return;
    }
#else
    (void)view;
#endif
    static bool once = false;
    if (!once) {
        once = true;
        log_write("capture: PNG readback is implemented for the Metal backend only");
    }
}

#ifdef BOTW_GLUE_MACROS_SUSPENDED
#pragma pop_macro("set_event_sink")
#pragma pop_macro("set_swap_interval")
#pragma pop_macro("swap_interval")
#pragma pop_macro("last_vsync_time")
#pragma pop_macro("last_flip_time")
#pragma pop_macro("wait_for_vsync")
#pragma pop_macro("wait_for_flip")
#pragma pop_macro("add_flip_request")
#pragma pop_macro("flips_executed")
#pragma pop_macro("flip_requests")
#pragma pop_macro("wait_retired")
#pragma pop_macro("retired")
#pragma pop_macro("last_submitted")
#pragma pop_macro("submit")
#pragma pop_macro("set_capture")
#pragma pop_macro("init")
#pragma pop_macro("set_display_mode")
#pragma pop_macro("set_scanout_srgb")
#pragma pop_macro("resize_window")
#pragma pop_macro("create_renderer")
#endif

namespace latte {
void select_backend(const char* name) {
    if (!name || !name[0] || strcmp(name, "native") == 0) return;
    if (strcmp(name, "vulkan") == 0) {
#if !defined(ENABLE_VULKAN)
        fprintf(stderr, "this build has no Vulkan backend (configure with -DBOTW_ENABLE_VULKAN=ON)\n");
        exit(2);
#else
        g_vulkan = true;
#endif
    } else if (strcmp(name, "metal") == 0) {
#if !defined(ENABLE_METAL)
        fprintf(stderr, "this build has no Metal backend\n");
        exit(2);
#else
        g_vulkan = false;
#endif
    } else {
        fprintf(stderr, "unknown --renderer %s (expected metal, vulkan, or native)\n", name);
        exit(2);
    }
}
bool using_vulkan() { return g_vulkan; }

void set_host_surface(HostSurface kind, void* display, void* surface) {
    g_kind = kind;
    g_display = display;
    g_surface = surface;
}

void create_renderer(void* native_view, int w, int h) {
    if (!g_surface) g_surface = native_view;
    auto& info = WindowSystem::GetWindowInfo();
    info.window_main.backend = to_backend(g_kind);
    info.window_main.display = g_display;
    info.window_main.surface = g_surface ? g_surface : native_view;
    info.canvas_main = info.window_main;
    info.window_pad = info.window_main;
    info.canvas_pad = info.window_main;
#if defined(ENABLE_VULKAN)
    if (g_vulkan) {
        GetConfig().graphic_api = GraphicAPI::kVulkan;
        // Cemu loads the Vulkan entry points before constructing the renderer. The function
        // pointers stay null until this runs, and the constructor calls them immediately.
        if (!InitializeGlobalVulkan()) {
            log_write("FATAL: Vulkan loader failed to initialise");
            throw std::runtime_error("Vulkan loader failed to initialise");
        }
        // SwapchainInfoVk looks the renderer up through the global, so it has to be
        // published before InitializeSurface. Cemu does the same from its canvas.
        g_renderer = std::make_unique<VulkanRenderer>();
        auto* vk = static_cast<VulkanRenderer*>(g_renderer.get());
        int pw = 0, ph = 0;
        WindowSystem::GetWindowPhysSize(pw, ph);
        if (pw <= 0 || ph <= 0) {
            pw = w;
            ph = h;
        }
        vk->InitializeSurface({pw, ph}, true);
        log_write("vulkan device: %s (swapchain %dx%d)", vk->GetDeviceName().c_str(), pw, ph);
        return;
    }
#endif
#if defined(ENABLE_METAL)
    GetConfig().graphic_api = GraphicAPI::kMetal;
    info.window_main.backend = WindowSystem::WindowHandleInfo::Backend::Cocoa;
    info.canvas_main.backend = info.window_main.backend;
    g_renderer = std::make_unique<MetalRenderer>();
    static_cast<MetalRenderer*>(g_renderer.get())->InitializeLayer({w, h}, true);
#else
    log_write("FATAL: no graphics backend was compiled in");
    abort();
#endif
}

void resize_window(int w, int h) {
#if defined(ENABLE_METAL)
    if (!g_vulkan && g_renderer) static_cast<MetalRenderer*>(g_renderer.get())->ResizeLayer({w, h}, true);
#else
    (void)w;
    (void)h;
#endif
}

bool present_empty_frame() {
    if (!g_renderer) return false;
    bool presented = false;
    try {
        g_renderer->Initialize();
        // DrawEmptyFrame begins a frame and presents it. A second SwapBuffers here would
        // present again without a new frame.
        g_renderer->DrawEmptyFrame(true);
        log_write("presented one empty frame on %s", g_renderer->GetDeviceName().c_str());
        presented = true;
    } catch (const std::exception& e) {
        log_write("empty-frame present failed: %s", e.what());
    }
    try {
        if (g_renderer) g_renderer->Shutdown();
    } catch (const std::exception& e) {
        log_write("renderer shutdown after empty frame failed: %s", e.what());
    }
    g_renderer.reset();
    return presented;
}

void set_display_mode(int mode) { GetConfig().fullscreen_scaling = mode; }
void set_output_filter(int f) {  // upscaling uses the choice; downscaling stays bilinear unless nearest
    GetConfig().upscale_filter = f;
    GetConfig().downscale_filter = f == kNearestNeighborFilter ? kNearestNeighborFilter : kLinearFilter;
}

void set_scanout_srgb(bool tv, bool srgb) {
    if (tv) LatteGPUState.tvBufferUsesSRGB = srgb;
    else LatteGPUState.drcBufferUsesSRGB = srgb;
}

void set_capture(int every, const char* dir) {
    g_capture_every = every;
    g_capture_dir = dir;
    fs::create_directories(dir);
}

void init() {
    static bool done = false;
    if (done) return;
    done = true;
    LatteGPUState.sharedAreaAddr = mem::sys_alloc(sizeof(gx2GPUSharedArea_t), 0x20);
    LatteGPUState.sharedArea = (gx2GPUSharedArea_t*)memory_getPointerFromVirtualOffset(LatteGPUState.sharedAreaAddr);
    LatteGPUState.sharedArea->flipRequestCountBE = 0;
    LatteGPUState.sharedArea->flipExecuteCountBE = 0;
    LatteGPUState.sharedArea->swapInterval = 1;
    g_retire_slot = mem::sys_alloc(16, 16);
    Latte_Start();  // returns once the GPU thread finished its init
    LatteGPUState.gx2InitCalled++;
}

uint64_t submit(uint32_t addr, uint32_t ndw) {
    uint64 ts = ++g_last_submitted;
    uint32 words[] = {pm4HeaderType3(IT_INDIRECT_BUFFER_PRIV, 3), addr, 0, ndw,
                      pm4HeaderType3(IT_EVENT_WRITE_EOP, 5), 0x504, g_retire_slot, 0x40000000, (uint32)ts, (uint32)(ts >> 32)};
    bool wake;
    {
        std::lock_guard<std::mutex> l(g_rb_lock);
        g_rb.insert(g_rb.end(), std::begin(words), std::end(words));
        wake = g_rb_waiting;
    }
    if (wake) g_rb_cv.notify_one();
    return ts;
}
uint64_t last_submitted() { return g_last_submitted; }
uint64_t retired() { return g_retired; }
void wait_retired(uint64_t ts) {
    std::unique_lock<std::mutex> l(g_retire_lock);
    g_retire_cv.wait(l, [&] { return g_retired >= ts; });
}

uint32_t flip_requests() { return _swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE); }
uint32_t flips_executed() { return _swapEndianU32(LatteGPUState.sharedArea->flipExecuteCountBE); }
void add_flip_request() {
    LatteGPUState.sharedArea->flipRequestCountBE = _swapEndianU32(_swapEndianU32(LatteGPUState.sharedArea->flipRequestCountBE) + 1);
}
void wait_for_flip() {
    std::unique_lock<std::mutex> l(g_sync_lock);
    if (flip_requests() == flips_executed()) return;
    g_flip_cv.wait_for(l, std::chrono::milliseconds(100));
}
void wait_for_vsync() {
    std::unique_lock<std::mutex> l(g_sync_lock);
    uint64 v = g_vsync_count;
    g_vsync_cv.wait_for(l, std::chrono::milliseconds(100), [&] { return g_vsync_count != v; });
}
uint64_t last_flip_time() { return g_last_flip_time; }
uint64_t last_vsync_time() { return g_last_vsync_time; }
uint32_t swap_interval() { return LatteGPUState.sharedArea ? LatteGPUState.sharedArea->swapInterval : 1; }
void set_swap_interval(uint32_t v) {
    if (LatteGPUState.sharedArea) LatteGPUState.sharedArea->swapInterval = v;
}
void set_event_sink(std::function<void(Event)> sink) { g_sink = std::move(sink); }
uint32_t g_regs[kRegCount];  // unused with the Cemu CP (kept for the interface)
}  // namespace latte
