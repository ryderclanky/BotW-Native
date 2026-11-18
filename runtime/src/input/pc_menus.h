// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was
// not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Derived from Zelda-on-PC src/pcedition/PCMenus.{h,cpp} and PCInput.cpp (https://github.com/mircea32000/Zelda-on-PC).
//
// Mouse pointer in the game's own menus. The menus are driven by d-pad / stick navigation: the shown layouts are
// read (game/pc_ui.cpp), the hit areas the touch screen would have used are found, and the game's cursor manager is
// asked to focus the entry under the mouse (d-pad steps where that is not possible); a click confirms it, a right
// click goes back. The world map scrolls under a fixed cursor: dragging pans it 1:1, a click puts the cursor on the
// spot and confirms (pins, fast travel), the wheel zooms. Runs per game read of the GamePad (pc_controls frame()).
#pragma once
#include <stdint.h>

namespace pcmenus {
struct FrameIO {
    // in
    bool mouse_inside = false;
    float mouse_x = 0, mouse_y = 0;  // normalized picture coordinates
    bool lmb_down = false, lmb_pressed = false, lmb_released = false;
    bool rmb_pressed = false;
    bool mmb_down = false;
    int wheel = 0;  // notches, + = up
    // keys of the pause menu pages (pressed this read)
    bool inventory_key = false, journal_key = false;
    // out
    bool pointer_mode = false;  // a menu takes the mouse: OS cursor shown, no camera, mouse buttons are the menu's
    uint32_t hold_add = 0;      // GamePad buttons pressed by the pointer this read
    bool override_l = false, override_r = false;
    float lx = 0, ly = 0, rx = 0, ry = 0;
};

void update(FrameIO& io);
}  // namespace pcmenus
