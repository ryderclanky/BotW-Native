// User settings: changed from the in-game menu (host/menu.cpp) and kept in settings.ini in the user
// folder. Command-line flags override them for one run.
#pragma once
#include "botw_target.h"

#include <string>

struct Settings {
    int fps = 60;          // 30 or 60
#if TARGET_OS_IOS
    float scale = 1;       // render scale (phones: native 720p); 0 = fit the display; applies at the next launch
#else
    float scale = 0;       // render scale; 0 = fit the display (takes effect at the next launch)
#endif
    int display = 0;       // 0 fit (letterbox), 1 stretch, 2 fill (crop)
    int filter = 1;        // output scaling: 0 bilinear, 1 bicubic, 2 hermite, 3 nearest
    bool gyro = true;      // controller gyro -> GamePad motion
    bool rumble = true;
    int layout = -1;       // -1 automatic per controller, 0 Nintendo positions, 1 Xbox/PlayStation labels
    bool show_fps = false;
    bool gamepad_view = false;  // show the GamePad screen beside the TV image (F2, Guide button, or the menu)
    int volume = 100;      // percent
#if TARGET_OS_IOS
    int touch = 1;         // on-screen controls: 0 auto (while no controller), 1 on, 2 off
#else
    int touch = 0;         // on-screen controls: 0 auto (while no controller), 1 on, 2 off
#endif
    bool touch_hidden = false;  // collapsed to the show/hide button (that button toggles it)
    std::string gpu;       // Vulkan device UUID (hex) of the chosen GPU; empty = automatic (best GPU)
    std::string gpu_name;  // its name, used when the UUID is not found
};

namespace settings {
Settings get();
void set(const Settings& s, bool save = true);  // apply to the running game (and save to settings.ini)
void load(const std::string& path);  // and apply; missing file = defaults
void apply();                         // push the current values into the runtime
}  // namespace settings
