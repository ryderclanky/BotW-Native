// BotW-Native shim: Cemu's window-system interface backed by the SDL window (runtime/src/host).
#pragma once
#include <atomic>
#include <stdint.h>

namespace WindowSystem {
struct WindowHandleInfo {
    enum class Backend { X11, Wayland, Cocoa, Windows } backend = Backend::Cocoa;
    void* display = nullptr;
    void* surface = nullptr;  // CAMetalLayer* here
};
enum struct PlatformKeyCodes : uint32_t { LCONTROL, RCONTROL, TAB, ESCAPE };
struct WindowInfo {
    WindowHandleInfo window_main;
    WindowHandleInfo window_pad;
    // Cemu's Vulkan swapchain is created from the canvas handle (same native window here).
    WindowHandleInfo canvas_main;
    WindowHandleInfo canvas_pad;
};
WindowInfo& GetWindowInfo();
void GetWindowPhysSize(int& w, int& h);
inline bool IsPadWindowOpen() { return false; }
inline void GetPadWindowPhysSize(int& w, int& h) { w = h = 0; }
inline double GetWindowDPIScale() { return 1.0; }
inline double GetPadDPIScale() { return 1.0; }
inline void UpdateWindowTitles(bool, bool, double) {}
inline bool IsKeyDown(PlatformKeyCodes) { return false; }
}  // namespace WindowSystem
