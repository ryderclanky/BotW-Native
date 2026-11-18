// PC Motion Control: the mouse tilts a virtual GamePad (motion shrines, tilt puzzles). Port of Zelda-on-PC's
// tilt emulation (src/pcedition/PCInput.cpp, MPL-2.0, https://github.com/mircea32000/Zelda-on-PC).
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was
// not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

namespace input {
struct Motion;
}

namespace pctilt {
struct Config {
    float sens = 1.0f;            // multiplier; base = 0.05 degrees per mouse count
    bool invert_x = false, invert_y = false;
    float max_angle = 60.0f;      // degrees of tilt at most (up to 180: all the way round)
    float return_speed = 0.0f;    // degrees per second it levels by itself (0 = off)
    float turn_step = 15.0f;      // degrees the wheel turns it flat (yaw) per notch
    bool invert_turn = false;
    float smoothing_ms = 35.0f;   // time constant of the spring the slate follows the mouse with (0 = at once)
    float max_speed = 300.0f;     // degrees per second it turns at most
};

// One game read. active: Motion Control is on (mouse dx/dy and wheel notches tilt / turn it, level = back to
// flat). Off, it levels smoothly. Returns true (and fills out) while the virtual GamePad is reported: while
// active, and until it is flat again after.
bool step(const Config& cfg, bool active, double mdx, double mdy, int turn_notches, bool level, float dt,
          input::Motion* out);
}  // namespace pctilt
