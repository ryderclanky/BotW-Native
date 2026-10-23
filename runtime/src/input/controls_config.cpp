#include "input/controls_config.h"

#include "input/controls_file.h"
#include "input/input.h"
#include "../log.h"
#include "../os.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace controls {
namespace {

struct PadSource {
    enum Type { None, Button, Axis, Trigger, Auto } type = None;
    int button = -1;
    int axis = -1;
    int sign = 0;  // player space: +1 right/up, -1 left/down
};

struct Resolved {
    const char* id = "";
    int stick = 0;
    int sign = 0;
    uint32_t bit = 0;
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    SDL_Scancode key2 = SDL_SCANCODE_UNKNOWN;
    PadSource pad;
};

enum class State { Untried, Off, On };
State g_state = State::Untried;
std::vector<Resolved> g_binds;
std::string g_path;
bool g_logged = false;

uint32_t bit_for(const std::string& id) {
    if (id == "a") return input::BTN_A;
    if (id == "b") return input::BTN_B;
    if (id == "x") return input::BTN_X;
    if (id == "y") return input::BTN_Y;
    if (id == "l") return input::BTN_L;
    if (id == "r") return input::BTN_R;
    if (id == "zl") return input::BTN_ZL;
    if (id == "zr") return input::BTN_ZR;
    if (id == "plus") return input::BTN_PLUS;
    if (id == "minus") return input::BTN_MINUS;
    if (id == "up") return input::BTN_UP;
    if (id == "down") return input::BTN_DOWN;
    if (id == "left") return input::BTN_LEFT;
    if (id == "right") return input::BTN_RIGHT;
    if (id == "stickl") return input::BTN_STICK_L;
    if (id == "stickr") return input::BTN_STICK_R;
    if (id == "home") return input::BTN_HOME;
    if (id == "sync") return input::BTN_SYNC;
    if (id == "tv") return input::BTN_TV;
    return 0;
}

int button_token(const std::string& t) {
    if (t == "south") return SDL_GAMEPAD_BUTTON_SOUTH;
    if (t == "east") return SDL_GAMEPAD_BUTTON_EAST;
    if (t == "west") return SDL_GAMEPAD_BUTTON_WEST;
    if (t == "north") return SDL_GAMEPAD_BUTTON_NORTH;
    if (t == "back") return SDL_GAMEPAD_BUTTON_BACK;
    if (t == "start") return SDL_GAMEPAD_BUTTON_START;
    if (t == "guide") return SDL_GAMEPAD_BUTTON_GUIDE;
    if (t == "leftstick") return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    if (t == "rightstick") return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    if (t == "leftshoulder") return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    if (t == "rightshoulder") return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    if (t == "dpup") return SDL_GAMEPAD_BUTTON_DPAD_UP;
    if (t == "dpdown") return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    if (t == "dpleft") return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    if (t == "dpright") return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    if (t == "misc1") return SDL_GAMEPAD_BUTTON_MISC1;
    if (t == "misc2") return SDL_GAMEPAD_BUTTON_MISC2;
    if (t == "misc3") return SDL_GAMEPAD_BUTTON_MISC3;
    if (t == "misc4") return SDL_GAMEPAD_BUTTON_MISC4;
    if (t == "misc5") return SDL_GAMEPAD_BUTTON_MISC5;
    if (t == "misc6") return SDL_GAMEPAD_BUTTON_MISC6;
    if (t == "touchpad") return SDL_GAMEPAD_BUTTON_TOUCHPAD;
    if (t == "paddle1") return SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1;
    if (t == "paddle2") return SDL_GAMEPAD_BUTTON_LEFT_PADDLE1;
    if (t == "paddle3") return SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2;
    if (t == "paddle4") return SDL_GAMEPAD_BUTTON_LEFT_PADDLE2;
    return -1;
}

PadSource pad_from(const std::string& t) {
    PadSource p;
    if (t == "auto") {
        p.type = PadSource::Auto;
        return p;
    }
    auto axis = [&](int ax, int sign) {
        p.type = PadSource::Axis;
        p.axis = ax;
        p.sign = sign;
    };
    if (t == "leftx-") axis(SDL_GAMEPAD_AXIS_LEFTX, -1);
    else if (t == "leftx+") axis(SDL_GAMEPAD_AXIS_LEFTX, +1);
    else if (t == "lefty-") axis(SDL_GAMEPAD_AXIS_LEFTY, -1);
    else if (t == "lefty+") axis(SDL_GAMEPAD_AXIS_LEFTY, +1);
    else if (t == "rightx-") axis(SDL_GAMEPAD_AXIS_RIGHTX, -1);
    else if (t == "rightx+") axis(SDL_GAMEPAD_AXIS_RIGHTX, +1);
    else if (t == "righty-") axis(SDL_GAMEPAD_AXIS_RIGHTY, -1);
    else if (t == "righty+") axis(SDL_GAMEPAD_AXIS_RIGHTY, +1);
    else if (t == "lefttrigger") {
        p.type = PadSource::Trigger;
        p.axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    } else if (t == "righttrigger") {
        p.type = PadSource::Trigger;
        p.axis = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    } else {
        int button = button_token(t);
        if (button >= 0) {
            p.type = PadSource::Button;
            p.button = button;
        }
    }
    return p;
}

SDL_Scancode scancode_of(const std::string& name, const std::string& id) {
    if (name.empty()) return SDL_SCANCODE_UNKNOWN;
    SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) LOG("controls: unknown key \"%s\" for %s", name.c_str(), id.c_str());
    return sc;
}

bool ensure() {
    if (g_state != State::Untried) return g_state == State::On;
    g_state = State::Off;
    if (g_user_root.empty()) return false;
    std::filesystem::path file = std::filesystem::path(g_user_root) / "controls.ini";
    g_path = file.string();
    std::vector<controlsfile::Binding> rows;
    std::string err;
    auto st = controlsfile::load(file, rows, err);
    if (st == controlsfile::LoadStatus::Missing) return false;
    if (st == controlsfile::LoadStatus::Error) {
        LOG("controls: %s", err.c_str());
        return false;
    }
    if (rows.empty()) {
        LOG("controls: %s has no bindings; using built-in defaults", g_path.c_str());
        return false;
    }
    for (const auto& b : rows) {
        const controlsfile::Action* action = controlsfile::find_action(b.id);
        if (!action) continue;
        Resolved r;
        r.id = action->id;
        r.stick = action->stick;
        r.sign = action->sign;
        r.bit = bit_for(b.id);
        r.key = scancode_of(b.key, b.id);
        r.key2 = scancode_of(b.key2, b.id);
        if (!b.pad.empty()) r.pad = pad_from(b.pad);
        g_binds.push_back(r);
    }
    if (g_binds.empty()) {
        LOG("controls: %s had no usable bindings; using built-in defaults", g_path.c_str());
        return false;
    }
    g_state = State::On;
    if (!g_logged) {
        g_logged = true;
        LOG("controls: loaded %s (%d bindings). Keyboard uses this file. Controller buttons marked auto follow the pad; a position overrides that.",
            g_path.c_str(), static_cast<int>(g_binds.size()));
    }
    return true;
}

// Same radial deadzone as window.cpp (edge of the zone is zero, then rescaled).
void radial_deadzone(float& x, float& y) {
    float m = std::sqrt(x * x + y * y);
    if (m <= 0.15f) {
        x = y = 0;
        return;
    }
    float s = (m - 0.15f) / ((1.f - 0.15f) * m);
    x *= s;
    y *= s;
}

// Player space: +X right, +Y up. No deadzone; stick pairs are deadzoned together.
float raw_player_axis(SDL_Gamepad* pad, int axis) {
    float f = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(axis)) / 32767.0f;
    if (axis == SDL_GAMEPAD_AXIS_LEFTY || axis == SDL_GAMEPAD_AXIS_RIGHTY) f = -f;
    return f;
}

void add_stick(input::PadState& s, int stick, float v) {
    if (stick == 1) s.lx += v;
    else if (stick == 2) s.ly += v;
    else if (stick == 3) s.rx += v;
    else if (stick == 4) s.ry += v;
}

void add_axis_stick(int stick, float v, float& lx, float& ly, float& rx, float& ry) {
    if (stick == 1) lx += v;
    else if (stick == 2) ly += v;
    else if (stick == 3) rx += v;
    else if (stick == 4) ry += v;
}

int face_index(const char* id) {
    if (!id) return -1;
    if (std::strcmp(id, "a") == 0) return 0;
    if (std::strcmp(id, "b") == 0) return 1;
    if (std::strcmp(id, "x") == 0) return 2;
    if (std::strcmp(id, "y") == 0) return 3;
    return -1;
}

const Resolved* find_resolved(const char* action_id) {
    if (!action_id) return nullptr;
    for (const Resolved& r : g_binds)
        if (std::strcmp(r.id, action_id) == 0) return &r;
    return nullptr;
}

bool key_down(const Resolved& r, const bool* keys) {
    if (!keys) return false;
    if (r.key != SDL_SCANCODE_UNKNOWN && keys[r.key]) return true;
    if (r.key2 != SDL_SCANCODE_UNKNOWN && keys[r.key2]) return true;
    return false;
}

}  // namespace

bool loaded() { return ensure(); }

bool apply_keyboard(SDL_Window* win, input::PadState& s) {
    if (!ensure()) return false;
    const bool* keys = nullptr;
    if (win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) keys = SDL_GetKeyboardState(nullptr);
    for (const Resolved& r : g_binds) {
        if (!key_down(r, keys)) continue;
        if (r.stick == 0) s.buttons |= r.bit;
        else add_stick(s, r.stick, static_cast<float>(r.sign));
    }
    return true;
}

bool apply_pad(SDL_Gamepad* pad, input::PadState& s, SDL_GamepadButton (*face)(SDL_Gamepad*, int)) {
    if (!ensure()) return false;
    if (!pad) return true;
    // Same quarter-pull as window.cpp's kTriggerOn, so a remapped ZL/ZR still trips the dialogue latch.
    constexpr int kTriggerOn = 8000;
    float alx = 0, aly = 0, arx = 0, ary = 0;
    for (const Resolved& r : g_binds) {
        bool digital = false;
        bool axis_for_stick = false;
        float axis_v = 0;
        if (r.pad.type == PadSource::Auto) {
            int which = face_index(r.id);
            if (which >= 0 && face && SDL_GetGamepadButton(pad, face(pad, which))) digital = true;
        } else if (r.pad.type == PadSource::Button) {
            if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(r.pad.button))) digital = true;
        } else if (r.pad.type == PadSource::Trigger) {
            if (SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(r.pad.axis)) > kTriggerOn) digital = true;
        } else if (r.pad.type == PadSource::Axis) {
            float player = raw_player_axis(pad, r.pad.axis);
            bool side = (r.pad.sign > 0 && player > 0.f) || (r.pad.sign < 0 && player < 0.f);
            if (side && r.stick == 0 && std::fabs(player) > 0.15f) digital = true;
            else if (side && r.stick != 0) {
                axis_for_stick = true;
                axis_v = player;
            }
        }
        if (r.stick == 0) {
            if (digital) s.buttons |= r.bit;
        } else if (digital) {
            add_stick(s, r.stick, static_cast<float>(r.sign));
        } else if (axis_for_stick) {
            add_axis_stick(r.stick, axis_v, alx, aly, arx, ary);
        }
    }
    radial_deadzone(alx, aly);
    radial_deadzone(arx, ary);
    s.lx += alx;
    s.ly += aly;
    s.rx += arx;
    s.ry += ary;
    return true;
}

uint32_t button_for_scancode(SDL_Scancode sc) {
    if (!ensure() || sc == SDL_SCANCODE_UNKNOWN) return 0;
    uint32_t bits = 0;
    for (const Resolved& r : g_binds) {
        if (r.stick != 0 || r.bit == 0) continue;
        if (r.key == sc || r.key2 == sc) bits |= r.bit;
    }
    return bits;
}

int mapped_button(const char* action_id) {
    if (!ensure()) return -1;
    const Resolved* r = find_resolved(action_id);
    if (!r || r->pad.type != PadSource::Button) return -1;
    return r->pad.button;
}

bool log_if_loaded() { return ensure(); }

}  // namespace controls
