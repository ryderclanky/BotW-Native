// Host window, event loop and input mapping (host main thread). The renderer presents into the
// window from the GPU thread (a CAMetalLayer on Apple, a Vulkan swapchain elsewhere).
#pragma once
#include <stdint.h>

#include <string>

struct SDL_Window;

namespace window {
void create();                   // main thread, before the game starts: window + renderer
void run();                      // event loop; returns when the app should quit
void drawable_size(int* w, int* h);
// Pixel size of the display the window is on (for the automatic render scale).
void display_pixel_size(int* w, int* h);
void set_fullscreen(bool on);  // --fullscreen: same as set_window_mode(1)
// 0 windowed, 1 borderless fullscreen (desktop resolution), 2 exclusive fullscreen (display's native
// mode). Any thread; the change is applied on the event loop thread. F11 and Alt+Enter toggle.
void set_window_mode(int mode);
void set_gyro(bool on);        // controller gyro -> GamePad motion (default on)
void set_rumble(bool on);
// -1: automatic (Nintendo layout on Switch controllers, labels on Xbox/PlayStation),
//  0: A = right face button (Nintendo positions), 1: A = bottom face button.
void set_layout(int layout);
void set_script(const std::string& s);  // "F:key:NAME,F:hold:NAME:frames,F:stick:l:x:y:frames,F:quit"
void on_frame(uint64_t frame);   // called by the renderer after each presented frame
bool quit_requested();
SDL_Window* g_win_ptr();
}  // namespace window
