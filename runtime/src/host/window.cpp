#include "window.h"

#include "botw_target.h"
#include <SDL3/SDL.h>
#if defined(__linux__)
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "../input/controls_config.h"
#include "../input/input.h"
#include "../log.h"
#include "../os.h"
#include "../latte/latte.h"

#include <cstdio>

extern std::atomic<pthread_t> g_gpu_thread;  // cemu_shims/overlay_menu.cpp: Cemu's GPU thread
#include "menu.h"
#include "touch.h"
#include "../thread.h"

extern "C" int g_trace_hle;

namespace window {
namespace {
SDL_Window* g_win = nullptr;
std::atomic<bool> g_quit{false};
std::atomic<uint64_t> g_frame{0};
bool g_use_gyro = true;  // --no-gyro / settings
bool g_use_rumble = true;
bool g_background = false;  // iOS app lifecycle
bool g_start_fullscreen = false;  // --fullscreen
// -1 auto (per controller family), 0 Nintendo positions, 1 Xbox/PlayStation labels
int g_layout = -1;
std::atomic<int> g_draw_w{1280}, g_draw_h{720};

// keyboard -> GamePad
struct KeyMap {
    SDL_Scancode sc;
    uint32_t button;
};
const KeyMap kKeys[] = {
    {SDL_SCANCODE_E, input::BTN_A},       {SDL_SCANCODE_LSHIFT, input::BTN_B},  {SDL_SCANCODE_RSHIFT, input::BTN_B},
    {SDL_SCANCODE_SPACE, input::BTN_X},   {SDL_SCANCODE_F, input::BTN_Y},       {SDL_SCANCODE_Q, input::BTN_ZL},
    {SDL_SCANCODE_R, input::BTN_ZR},      {SDL_SCANCODE_1, input::BTN_L},       {SDL_SCANCODE_3, input::BTN_R},
    {SDL_SCANCODE_RETURN, input::BTN_PLUS}, {SDL_SCANCODE_TAB, input::BTN_MINUS},
    {SDL_SCANCODE_UP, input::BTN_UP},     {SDL_SCANCODE_DOWN, input::BTN_DOWN},
    {SDL_SCANCODE_LEFT, input::BTN_LEFT}, {SDL_SCANCODE_RIGHT, input::BTN_RIGHT},
    {SDL_SCANCODE_C, input::BTN_STICK_L}, {SDL_SCANCODE_V, input::BTN_STICK_R},
    {SDL_SCANCODE_H, input::BTN_HOME},    {SDL_SCANCODE_HOME, input::BTN_HOME},
    {SDL_SCANCODE_G, input::BTN_SYNC},    {SDL_SCANCODE_T, input::BTN_TV},
};

// scripted input for headless tests: per-frame events
struct ScriptEvent {
    uint64_t frame;
    std::string kind, a;
    float x = 0, y = 0;
    int frames = 6;
};
std::mutex g_script_lock;
std::vector<ScriptEvent> g_script;
struct Active {
    uint32_t buttons;
    float lx, ly, rx, ry;
    uint64_t until;
};
std::vector<Active> g_active;

uint32_t button_by_name(const std::string& n) {
    static const std::pair<const char*, uint32_t> names[] = {
        {"a", input::BTN_A},    {"b", input::BTN_B},       {"x", input::BTN_X},     {"y", input::BTN_Y},
        {"l", input::BTN_L},    {"r", input::BTN_R},       {"zl", input::BTN_ZL},   {"zr", input::BTN_ZR},
        {"plus", input::BTN_PLUS}, {"minus", input::BTN_MINUS}, {"up", input::BTN_UP}, {"down", input::BTN_DOWN},
        {"left", input::BTN_LEFT}, {"right", input::BTN_RIGHT}, {"home", input::BTN_HOME},
        {"stickl", input::BTN_STICK_L}, {"stickr", input::BTN_STICK_R},
        {"sync", input::BTN_SYNC}, {"tv", input::BTN_TV},
    };
    for (auto& [k, v] : names)
        if (n == k) return v;
    return 0;
}

// A tap can go down and up between two polls of SDL_GetKeyboardState. Remember
// the press long enough for VPADRead to sample it. Level state still covers a hold.
struct KeyPulse {
    uint32_t buttons;
    uint64_t until;
};
std::vector<KeyPulse> g_key_pulses;

uint32_t button_for_scancode(SDL_Scancode sc) {
    for (auto& k : kKeys)
        if (k.sc == sc) return k.button;
    return 0;
}
void note_key_down(SDL_Scancode sc) {
    // Level state (SDL_GetKeyboardState) misses a tap that is already up by the
    // next poll. OS key-repeat events are not new presses; the level bit covers a hold.
    if (menu::is_open()) return;
    // controls.ini replaces the built-in scancode table, including this tap latch.
    uint32_t b = controls::loaded() ? controls::button_for_scancode(sc) : button_for_scancode(sc);
    if (b) g_key_pulses.push_back({b, SDL_GetTicks() + 50});
}

constexpr float kStickDeadzone = 0.15f;
constexpr int kTriggerOn = 8000;  // about a quarter of full travel; ZL/ZR are digital on the GamePad

struct Controller {
    SDL_Gamepad* pad = nullptr;
    SDL_JoystickID id = 0;
    uint64_t last_used = 0;
    uint64_t connect_seq = 0;
};
std::vector<Controller> g_ctrls;
uint64_t g_connect_seq = 0;

int find_ctrl(SDL_JoystickID id) {
    for (int i = 0; i < (int)g_ctrls.size(); i++)
        if (g_ctrls[i].id == id) return i;
    return -1;
}
int active_index() {
    int best = -1;
    for (int i = 0; i < (int)g_ctrls.size(); i++) {
        if (best < 0) {
            best = i;
            continue;
        }
        const Controller& c = g_ctrls[i];
        const Controller& b = g_ctrls[best];
        if (c.last_used > b.last_used || (c.last_used == b.last_used && c.connect_seq > b.connect_seq)) best = i;
    }
    return best;
}
SDL_Gamepad* active_pad() {
    int i = active_index();
    return i >= 0 ? g_ctrls[i].pad : nullptr;
}
const char* type_name(SDL_GamepadType t) {
    switch (t) {
        case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
        case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One/Series";
        case SDL_GAMEPAD_TYPE_PS3: return "PS3";
        case SDL_GAMEPAD_TYPE_PS4: return "DualShock 4";
        case SDL_GAMEPAD_TYPE_PS5: return "DualSense";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con L";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con R";
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con pair";
        case SDL_GAMEPAD_TYPE_GAMECUBE: return "GameCube";
        case SDL_GAMEPAD_TYPE_STANDARD: return "standard";
        default: return "unknown";
    }
}
const char* pad_name(SDL_Gamepad* pad) {
    const char* n = pad ? SDL_GetGamepadName(pad) : nullptr;
    return n ? n : "controller";
}
// Wii U A/B/X/Y. Auto follows the label SDL reports for that family (Switch A is
// the right button, Xbox A and PlayStation Cross are the bottom button).
SDL_GamepadButton wii_face(SDL_Gamepad* pad, int which) {
    const SDL_GamepadButton nintendo[] = {SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_NORTH,
                                          SDL_GAMEPAD_BUTTON_WEST};
    const SDL_GamepadButton labels[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                        SDL_GAMEPAD_BUTTON_NORTH};
    if (g_layout == 0) return nintendo[which];
    if (g_layout > 0) return labels[which];
    const SDL_GamepadButton faces[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
                                       SDL_GAMEPAD_BUTTON_NORTH};
    for (SDL_GamepadButton b : faces) {
        SDL_GamepadButtonLabel lab = SDL_GetGamepadButtonLabel(pad, b);
        bool match = false;
        if (which == 0) match = lab == SDL_GAMEPAD_BUTTON_LABEL_A || lab == SDL_GAMEPAD_BUTTON_LABEL_CROSS;
        else if (which == 1) match = lab == SDL_GAMEPAD_BUTTON_LABEL_B || lab == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE;
        else if (which == 2) match = lab == SDL_GAMEPAD_BUTTON_LABEL_X || lab == SDL_GAMEPAD_BUTTON_LABEL_SQUARE;
        else match = lab == SDL_GAMEPAD_BUTTON_LABEL_Y || lab == SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE;
        if (match) return b;
    }
    SDL_GamepadType t = pad ? SDL_GetGamepadType(pad) : SDL_GAMEPAD_TYPE_UNKNOWN;
    bool nintendo_family = t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
                           t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT || t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    return nintendo_family ? nintendo[which] : labels[which];
}
const char* face_phrase(SDL_Gamepad* pad) {
    SDL_GamepadButton a = wii_face(pad, 0);
    if (a == SDL_GAMEPAD_BUTTON_EAST) return "A = right";
    if (a == SDL_GAMEPAD_BUTTON_SOUTH) return "A = bottom";
    if (a == SDL_GAMEPAD_BUTTON_WEST) return "A = left";
    if (a == SDL_GAMEPAD_BUTTON_NORTH) return "A = top";
    return "A mapped";
}
bool enable_motion(SDL_Gamepad* pad) {
    if (!g_use_gyro || !pad) return false;
    if (!SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) || !SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) return false;
    return SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
}
void log_active() {
    int i = active_index();
    if (i < 0) return;
    SDL_Gamepad* pad = g_ctrls[i].pad;
    LOG("controller active: %s (%s)", pad_name(pad), type_name(SDL_GetGamepadType(pad)));
}
void open_controller(SDL_JoystickID id) {
    if (find_ctrl(id) >= 0) return;
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        const char* n = SDL_GetGamepadNameForID(id);
        LOG("controller: could not open %s (%s)", n ? n : "controller", SDL_GetError());
        return;
    }
    bool gyro = enable_motion(pad);
    SDL_JoystickID prev = 0;
    if (int p = active_index(); p >= 0) prev = g_ctrls[p].id;
    g_ctrls.push_back(Controller{pad, SDL_GetGamepadID(pad), 0, ++g_connect_seq});
    LOG("controller connected: %s (%s%s, %s)", pad_name(pad), type_name(SDL_GetGamepadType(pad)), gyro ? ", gyro" : "",
        face_phrase(pad));
    touch::set_controller_connected(true);
    if (int n = active_index(); n >= 0 && g_ctrls[n].id != prev) {
        if (prev) input::motion_reset();
        log_active();
    }
}
void close_controller(SDL_JoystickID id) {
    int i = find_ctrl(id);
    if (i < 0) return;
    SDL_JoystickID prev = 0;
    if (int p = active_index(); p >= 0) prev = g_ctrls[p].id;
    char name[128];
    char kind[64];
    snprintf(name, sizeof name, "%s", pad_name(g_ctrls[i].pad));
    snprintf(kind, sizeof kind, "%s", type_name(SDL_GetGamepadType(g_ctrls[i].pad)));
    SDL_CloseGamepad(g_ctrls[i].pad);
    g_ctrls.erase(g_ctrls.begin() + i);
    LOG("controller disconnected: %s (%s)", name, kind);
    input::motion_reset();
    touch::set_controller_connected(!g_ctrls.empty());
    if (int n = active_index(); n >= 0 && g_ctrls[n].id != prev) log_active();
}
void note_used(SDL_JoystickID id) {
    int i = find_ctrl(id);
    if (i < 0) return;
    SDL_JoystickID prev = 0;
    if (int p = active_index(); p >= 0) prev = g_ctrls[p].id;
    uint64_t now = SDL_GetTicks();
    g_ctrls[i].last_used = now ? now : 1;
    if (int n = active_index(); n >= 0 && g_ctrls[n].id != prev) {
        if (int o = find_ctrl(prev); o >= 0) SDL_RumbleGamepad(g_ctrls[o].pad, 0, 0, 0);
        input::motion_reset();
        log_active();
    }
}
void stick_deadzone(float& x, float& y) {
    float m = std::sqrt(x * x + y * y);
    if (m <= kStickDeadzone) {
        x = y = 0;
        return;
    }
    float s = (m - kStickDeadzone) / ((1.f - kStickDeadzone) * m);
    x *= s;
    y *= s;
}
void controller_hints() {
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_8BITDO, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "1");
#if defined(_WIN32)
    // Xbox stays on RawInput + XInput. HIDAPI for those pads as well opens each
    // one twice. RawInput claims the IG_ device; DirectInput then skips it.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_360, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX_ONE, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
#endif
}
bool load_mapping_file(const std::string& path, const char* what) {
    if (path.empty()) return false;
    int n = SDL_AddGamepadMappingsFromFile(path.c_str());
    if (n < 0) return false;
    LOG("controller mappings: %s (%d new) %s", what, n, path.c_str());
    return true;
}
void load_mappings() {
    std::string next_to_exe;
    if (const char* base = SDL_GetBasePath()) {
        next_to_exe = std::string(base) + "gamecontrollerdb.txt";
        SDL_free((void*)base);
    }
    bool bundled = load_mapping_file(next_to_exe, "bundled");
#ifdef BOTW_GAMECONTROLLER_DB
    if (!bundled) bundled = load_mapping_file(BOTW_GAMECONTROLLER_DB, "bundled");
#endif
    if (!bundled) LOG("controller mappings: bundled gamecontrollerdb.txt not found");
    if (!g_user_root.empty()) {
        std::string user = g_user_root + "/gamecontrollerdb.txt";
        if (user != next_to_exe) load_mapping_file(user, "user");
    }
    if (const char* f = getenv("SDL_GAMECONTROLLERCONFIG_FILE")) load_mapping_file(f, "SDL_GAMECONTROLLERCONFIG_FILE");
    if (const char* c = getenv("SDL_GAMECONTROLLERCONFIG"); c && c[0])
        LOG("controller mappings: SDL_GAMECONTROLLERCONFIG is set");
}
void scan_controllers() {
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; i++) open_controller(ids[i]);
    SDL_free(ids);
}

// Publish the window's safe-area insets (in drawable pixels) for the overlays.
void update_safe_area() {
    if (!g_win) return;
    SDL_Rect r;
    int w, h;
    if (!SDL_GetWindowSafeArea(g_win, &r) || !SDL_GetWindowSize(g_win, &w, &h)) return;
    float d = SDL_GetWindowPixelDensity(g_win);
    menu::set_safe_area(r.x * d, r.y * d, (w - r.x - r.w) * d, (h - r.y - r.h) * d);
}

void apply_input() {
    input::PadState s;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    // controls.ini replaces keyboard and overrides controller buttons. With no file, the
    // blocks below are the auto-layout map, including the tap latch applied just after.
    const bool file_controls = controls::loaded();
    if (file_controls) controls::apply_keyboard(g_win, s);
    else if (g_win && (SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS)) {
        for (auto& k : kKeys)
            if (keys[k.sc]) s.buttons |= k.button;
        if (keys[SDL_SCANCODE_A]) s.lx -= 1;
        if (keys[SDL_SCANCODE_D]) s.lx += 1;
        if (keys[SDL_SCANCODE_W]) s.ly += 1;
        if (keys[SDL_SCANCODE_S]) s.ly -= 1;
        if (keys[SDL_SCANCODE_J]) s.rx -= 1;
        if (keys[SDL_SCANCODE_L]) s.rx += 1;
        if (keys[SDL_SCANCODE_I]) s.ry += 1;
        if (keys[SDL_SCANCODE_K]) s.ry -= 1;
    }
    uint64_t pulse_now = SDL_GetTicks();
    for (auto it = g_key_pulses.begin(); it != g_key_pulses.end();) {
        if (it->until < pulse_now) {
            it = g_key_pulses.erase(it);
            continue;
        }
        s.buttons |= it->buttons;
        ++it;
    }
    if (SDL_Gamepad* pad = active_pad()) {
        if (file_controls) {
            // Face buttons marked auto still go through wii_face. A position in the file wins.
            controls::apply_pad(pad, s, wii_face);
        } else {
            float lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            float ly = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            float rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
            float ry = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
            stick_deadzone(lx, ly);
            stick_deadzone(rx, ry);
            s.lx += lx;
            s.ly += ly;
            s.rx += rx;
            s.ry += ry;
            const std::pair<SDL_GamepadButton, uint32_t> btns[] = {
                {wii_face(pad, 0), input::BTN_A},
                {wii_face(pad, 1), input::BTN_B},
                {wii_face(pad, 2), input::BTN_X},
                {wii_face(pad, 3), input::BTN_Y},
                {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, input::BTN_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, input::BTN_R},
                {SDL_GAMEPAD_BUTTON_START, input::BTN_PLUS},    {SDL_GAMEPAD_BUTTON_BACK, input::BTN_MINUS},
                {SDL_GAMEPAD_BUTTON_DPAD_UP, input::BTN_UP},    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, input::BTN_DOWN},
                {SDL_GAMEPAD_BUTTON_DPAD_LEFT, input::BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, input::BTN_RIGHT},
                {SDL_GAMEPAD_BUTTON_LEFT_STICK, input::BTN_STICK_L}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, input::BTN_STICK_R},
                {SDL_GAMEPAD_BUTTON_GUIDE, input::BTN_HOME},
                {SDL_GAMEPAD_BUTTON_MISC1, input::BTN_TV},    // Share / Capture
                {SDL_GAMEPAD_BUTTON_MISC2, input::BTN_SYNC},
            };
            for (auto& [b, v] : btns)
                if (SDL_GetGamepadButton(pad, b)) s.buttons |= v;
            if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kTriggerOn) s.buttons |= input::BTN_ZL;
            if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kTriggerOn) s.buttons |= input::BTN_ZR;
        }
    }
    touch::apply(s);  // on-screen controls (s is cleared below while the menu is open)
    if (menu::is_open()) {  // the menu owns the controls; the game sees nothing pressed
        g_key_pulses.clear();
        s = input::PadState{};
        if (SDL_Gamepad* pad = active_pad()) {  // left stick navigates the menu
            static int last_y = 0, last_x = 0;
            float y = -SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            float x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            int dy = y > 0.6f ? 1 : y < -0.6f ? -1 : 0, dx = x > 0.6f ? 1 : x < -0.6f ? -1 : 0;
            if (dy && dy != last_y) menu::move(-dy);
            if (dx && dx != last_x) menu::change(dx);
            last_y = dy, last_x = dx;
        }
    }
    // scripted input
    {
        std::lock_guard<std::mutex> l(g_script_lock);
        uint64_t f = g_frame;
        for (auto it = g_script.begin(); it != g_script.end();) {
            if (it->frame > f) {
                ++it;
                continue;
            }
            if (it->kind == "quit") g_quit = true;
            else if (it->kind == "dump") threads::dump(true);  // tests: F:dump (watchdog output)
            else if (it->kind == "menu") {  // tests: F:menu:toggle|up|down|left|right|ok
                if (it->a == "toggle") menu::toggle();
                else if (it->a == "up") menu::move(-1);
                else if (it->a == "down") menu::move(+1);
                else if (it->a == "left") menu::change(-1);
                else if (it->a == "right") menu::change(+1);
                else if (it->a == "ok") menu::activate();
            }
            else if (it->kind == "key" || it->kind == "hold")
                g_active.push_back({button_by_name(it->a), 0, 0, 0, 0, f + (uint64_t)it->frames});
            else if (it->kind == "stick")
                g_active.push_back({0, it->a == "l" ? it->x : 0, it->a == "l" ? it->y : 0, it->a == "r" ? it->x : 0,
                                    it->a == "r" ? it->y : 0, f + (uint64_t)it->frames});
            it = g_script.erase(it);
        }
        for (auto it = g_active.begin(); it != g_active.end();) {
            if (it->until <= f) {
                it = g_active.erase(it);
                continue;
            }
            s.buttons |= it->buttons;
            s.lx += it->lx;
            s.ly += it->ly;
            s.rx += it->rx;
            s.ry += it->ry;
            ++it;
        }
    }
    s.lx = std::clamp(s.lx, -1.f, 1.f);
    s.ly = std::clamp(s.ly, -1.f, 1.f);
    s.rx = std::clamp(s.rx, -1.f, 1.f);
    s.ry = std::clamp(s.ry, -1.f, 1.f);
    input::set_gamepad(s);
}

void log_controls() {
    if (controls::log_if_loaded()) return;
    LOG("controls: keyboard  E=A  Shift=B  Space=X  F=Y  Q=ZL  R=ZR  1=L  3=R  Enter=Plus  Tab=Minus");
    LOG("controls: keyboard  arrows=D-pad  C=left stick click  V=right stick click  H or Home=Home  G=Sync  T=TV");
    LOG("controls: keyboard  WASD=left stick  J/L=right stick left/right  I/K=right stick up/down  (+Y is up)");
    const char* layout = g_layout < 0 ? "auto (Switch A on the right, Xbox A / PlayStation Cross on the bottom)"
                         : g_layout > 0 ? "A = bottom, Cross / Xbox A"
                                        : "A = right, Circle / Xbox B";
    LOG("controls: gamepad face buttons %s", layout);
    LOG("controls: gamepad  L/R shoulders  triggers=ZL/ZR past about a quarter pull  Start=Plus  Back=Minus");
    LOG("controls: gamepad  Guide=Home  Share/Capture=TV  extra button=Sync  sticks +Y up, radial deadzone 0.15");
    LOG("controls: keyboard and the active controller are read together; the controller last used is active");
    LOG("controls: F1, both stick clicks, or the touchpad opens the menu; Escape closes it; F11 fullscreen");
}
}  // namespace

void set_script(const std::string& s) {
    std::lock_guard<std::mutex> l(g_script_lock);
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        std::string tok = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? s.size() : j + 1;
        std::vector<std::string> p;
        size_t k = 0;
        while (k <= tok.size()) {
            size_t m = tok.find(':', k);
            p.push_back(tok.substr(k, m == std::string::npos ? std::string::npos : m - k));
            if (m == std::string::npos) break;
            k = m + 1;
        }
        if (p.size() < 2) continue;
        ScriptEvent e;
        e.frame = strtoull(p[0].c_str(), nullptr, 10);
        e.kind = p[1];
        if (p.size() > 2) e.a = p[2];
        if (e.kind == "hold" && p.size() > 3) e.frames = atoi(p[3].c_str());
        if (e.kind == "stick" && p.size() > 5) {
            e.x = (float)atof(p[3].c_str());
            e.y = (float)atof(p[4].c_str());
            e.frames = atoi(p[5].c_str());
        }
        g_script.push_back(e);
    }
}

void set_gyro(bool on) {
    if (on == g_use_gyro) return;
    g_use_gyro = on;
    for (auto& c : g_ctrls) {
        if (!SDL_GamepadHasSensor(c.pad, SDL_SENSOR_GYRO) || !SDL_GamepadHasSensor(c.pad, SDL_SENSOR_ACCEL)) continue;
        SDL_SetGamepadSensorEnabled(c.pad, SDL_SENSOR_GYRO, on);
        SDL_SetGamepadSensorEnabled(c.pad, SDL_SENSOR_ACCEL, on);
    }
    input::motion_reset();
}
void set_rumble(bool on) {
    g_use_rumble = on;
    if (!on)
        for (auto& c : g_ctrls) SDL_RumbleGamepad(c.pad, 0, 0, 0);
}
void set_layout(int layout) { g_layout = layout; }
void set_fullscreen(bool on) { g_start_fullscreen = on; }

void display_pixel_size(int* w, int* h) {
    *w = 2560, *h = 1440;
    const SDL_DisplayMode* m = g_win ? SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(g_win)) : nullptr;
    if (m) {
        *w = (int)(m->w * m->pixel_density);
        *h = (int)(m->h * m->pixel_density);
    }
}

void on_frame(uint64_t f) {
    g_frame = f;
    if (g_trace_to) g_trace_hle = f >= g_trace_from && f < g_trace_to;
}
SDL_Window* g_win_ptr() { return g_win; }
bool quit_requested() { return g_quit; }
void drawable_size(int* w, int* h) {
    *w = g_draw_w;
    *h = g_draw_h;
}

// Platform window handle -> Latte renderer. Apple: window_surface_apple.mm. Everywhere else: below.
void window_attach_renderer(SDL_Window* win, int point_w, int point_h);

void create() {
#if defined(__linux__)
    // X11 is not thread-safe until this runs, and SDL opens the display inside SDL_Init.
    if (void* x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL)) {
        if (auto init = (int (*)())dlsym(x11, "XInitThreads")) init();
    }
#endif
    controller_hints();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) LOG("SDL_Init failed: %s", SDL_GetError());
    load_mappings();
#if defined(_WIN32)
    LOG("controllers: PlayStation, Switch, Joy-Con and 8BitDo use HIDAPI; Xbox uses RawInput correlated with XInput; DirectInput stays on for other pads");
#endif
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    flags |= latte::using_vulkan() ? SDL_WINDOW_VULKAN : SDL_WINDOW_METAL;
    if (g_headless) flags |= SDL_WINDOW_HIDDEN;  // tests: render into a hidden window, never on screen
    else if (g_start_fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
#if defined(_WIN32)
    const char* title = "BotW Recompiled (Windows)";
#else
    const char* title = "BotW Recompiled";
#endif
    g_win = SDL_CreateWindow(title, 1280, 720, flags);
    if (!g_win) {
        LOG("FATAL: SDL_CreateWindow failed: %s", SDL_GetError());
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }
    int w, h;
    SDL_GetWindowSizeInPixels(g_win, &w, &h);
    g_draw_w = w;
    g_draw_h = h;
    int pw, ph;
    SDL_GetWindowSize(g_win, &pw, &ph);
    window_attach_renderer(g_win, pw, ph);
    LOG("window \"%s\" %dx%d (%dx%d pixels)%s%s, renderer %s", title, pw, ph, w, h, g_headless ? ", hidden" : "",
        g_start_fullscreen && !g_headless ? ", fullscreen" : "", latte::using_vulkan() ? "vulkan" : "metal");
    // A game text box is not an OS text field. Leave SDL text input off so it cannot eat keys.
    SDL_StopTextInput(g_win);
    log_controls();
    scan_controllers();
}

void run() {
    auto t0 = SDL_GetTicks();
    update_safe_area();
    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) g_quit = true;
            else if (e.type == SDL_EVENT_DID_ENTER_BACKGROUND || e.type == SDL_EVENT_WILL_ENTER_FOREGROUND) {
                g_background = e.type == SDL_EVENT_DID_ENTER_BACKGROUND;
                LOG("app %s", g_background ? "entered the background" : "returning to the foreground");
            } else if (e.type == SDL_EVENT_TERMINATING) LOG("app terminating");
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED) open_controller(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) close_controller(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE) {
                if (SDL_Gamepad* pad = active_pad(); pad && e.gsensor.which == SDL_GetGamepadID(pad))
                    if (e.gsensor.sensor == SDL_SENSOR_GYRO || e.gsensor.sensor == SDL_SENSOR_ACCEL)
                        input::motion_sensor(e.gsensor.sensor == SDL_SENSOR_GYRO, e.gsensor.data, e.gsensor.timestamp);
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                note_used(e.gbutton.which);
                int ci = find_ctrl(e.gbutton.which);
                SDL_Gamepad* pad = ci >= 0 ? g_ctrls[ci].pad : nullptr;
                auto b = (SDL_GamepadButton)e.gbutton.button;
                bool both_sticks = pad && ((b == SDL_GAMEPAD_BUTTON_LEFT_STICK && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) ||
                                           (b == SDL_GAMEPAD_BUTTON_RIGHT_STICK && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK)));
                SDL_GamepadButton confirm = pad ? wii_face(pad, 0) : SDL_GAMEPAD_BUTTON_EAST;
                SDL_GamepadButton back = pad ? wii_face(pad, 1) : SDL_GAMEPAD_BUTTON_SOUTH;
                if (controls::loaded()) {
                    if (int a = controls::mapped_button("a"); a >= 0) confirm = static_cast<SDL_GamepadButton>(a);
                    if (int b = controls::mapped_button("b"); b >= 0) back = static_cast<SDL_GamepadButton>(b);
                }
                if (b == SDL_GAMEPAD_BUTTON_TOUCHPAD || both_sticks) menu::toggle();
                else if (menu::is_open()) {
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) menu::move(-1);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) menu::move(+1);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) menu::change(-1);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) menu::change(+1);
                    else if (b == confirm) menu::activate();
                    else if (b == back || b == SDL_GAMEPAD_BUTTON_START) menu::close();
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
                if (e.gaxis.value > kTriggerOn || e.gaxis.value < -kTriggerOn) note_used(e.gaxis.which);
            } else if (e.type == SDL_EVENT_TEXT_INPUT && g_win) {
                SDL_StopTextInput(g_win);
            } else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                // Record the press before the menu toggle. A tap of E can be down and
                // up inside one poll; the pulse keeps it visible to the next VPADRead.
                note_key_down(e.key.scancode);
                if (e.key.key == SDLK_F1) menu::toggle();
                else if (e.key.key == SDLK_F11 && g_win) {
                    bool fs = SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(g_win, !fs);
                } else if (menu::is_open()) {
                    switch (e.key.key) {
                        case SDLK_ESCAPE: menu::close(); break;
                        case SDLK_UP: menu::move(-1); break;
                        case SDLK_DOWN: menu::move(+1); break;
                        case SDLK_LEFT: menu::change(-1); break;
                        case SDLK_RIGHT: menu::change(+1); break;
                        case SDLK_RETURN: menu::activate(); break;
                        default: break;
                    }
                }
            } else if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION ||
                       e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
                touch::event(e);
            } else if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                       e.type == SDL_EVENT_MOUSE_BUTTON_UP) {  // touch arrives as mouse events too
                float density = g_win ? SDL_GetWindowPixelDensity(g_win) : 1.0f;
                float mx = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x;
                float my = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y;
                bool down = e.type == SDL_EVENT_MOUSE_MOTION ? (e.motion.state & SDL_BUTTON_LMASK) != 0
                                                             : e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                menu::pointer(mx * density, my * density, down);
            } else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                g_draw_w = e.window.data1;
                g_draw_h = e.window.data2;
                int pw, ph;  // the Metal layer takes points and applies the backing scale itself
                SDL_GetWindowSize(g_win, &pw, &ph);
                latte::resize_window(pw, ph);
                update_safe_area();
            } else if (e.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED) {
                update_safe_area();
            }
        }
        // A game text box is not an OS field. If text input turns on, key events
        // can stop updating the keyboard state the game is sampled from.
        if (g_win && SDL_TextInputActive(g_win)) {
            SDL_StopTextInput(g_win);
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOG("controls: SDL text input was on; stopped it so keyboard keys reach the game");
            }
        }
        apply_input();
        // Freeze watchdog: no game frame for 8 s while in the foreground -> log every thread (once a minute)
        {
            static uint64_t seen = 0, changed_ms = 0, dumped_ms = 0;
            uint64_t ms = SDL_GetTicks(), gf = g_game_frames;
            if (gf != seen || g_background) seen = gf, changed_ms = ms;
            else if (gf && ms - changed_ms > 8000 && (!dumped_ms || ms - dumped_ms > 60000)) {
                LOG("watchdog: no game frame for %llu s (frame %llu), thread dump:", (unsigned long long)(ms - changed_ms) / 1000,
                    (unsigned long long)gf);
                threads::dump(true);
                dumped_ms = ms;
            }
        }
        // Slow-frame sampler: fewer than 6 game frames in the last second (foreground) -> log the host stack of
        // Cemu's GPU thread every 200 ms, at most 40 a minute (symbolize with atos against the binary).
        {
            static uint64_t win_ms = 0, win_gf = 0, next_ms = 0, minute_ms = 0;
            static bool slow = false;
            static int taken = 0;
            uint64_t ms = SDL_GetTicks(), gf = g_game_frames;
            bool entered = false;
            if (ms - win_ms >= 1000) {
                bool now_slow = !g_background && win_gf && gf - win_gf < 6;
                if (now_slow && !slow) {
                    LOG("slow: %llu game frames in the last second, sampling the GPU thread", (unsigned long long)(gf - win_gf));
                    entered = true;
                }
                slow = now_slow, win_ms = ms, win_gf = gf;
            }
            if (ms - minute_ms > 60000) minute_ms = ms, taken = 0;
            pthread_t gpu = g_gpu_thread;
            if (slow && gpu && taken < 40 && ms >= next_ms) {
                threads::sample_host(gpu, "LatteThread");
                taken++, next_ms = ms + 200;
            }
            // After the GPU stack. A guest thread can be the one stopped while LatteThread waits.
            if (entered) threads::dump(true);
        }
        // GamePad rumble at 60 Hz steps
        static uint64_t next_rumble = 0;
        static bool rumbling = false;
        uint64_t now = SDL_GetTicksNS();
        if (now >= next_rumble) {
            next_rumble = now + 16666667;
            bool on = input::rumble_step();
            if (on != rumbling) {
                rumbling = on;
                if (SDL_Gamepad* pad = active_pad())
                    SDL_RumbleGamepad(pad, on && g_use_rumble ? 0x6000 : 0, on && g_use_rumble ? 0x6000 : 0,
                                      on && g_use_rumble ? 1000 : 0);
            }
        }
        if (g_run_seconds > 0 && SDL_GetTicks() - t0 > (Uint64)g_run_seconds * 1000) {
            LOG("time limit reached (%d s)", g_run_seconds);
            break;
        }
        SDL_Delay(4);
    }
}

#if !defined(__APPLE__)
void window_attach_renderer(SDL_Window* win, int point_w, int point_h) {
    const char* driver = SDL_GetCurrentVideoDriver();
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
    latte::HostSurface kind = latte::HostSurface::X11;
    void* display = nullptr;
    void* surface = nullptr;
    if (driver && strcmp(driver, "wayland") == 0) {
        kind = latte::HostSurface::Wayland;
        display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else if (driver && strcmp(driver, "windows") == 0) {
        kind = latte::HostSurface::Windows;
        surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    } else if (driver && strcmp(driver, "x11") == 0) {
        kind = latte::HostSurface::X11;
        display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        surface = (void*)(uintptr_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    } else {
        LOG("FATAL: SDL video driver \"%s\" has no Vulkan surface (need x11, wayland, or windows)", driver ? driver : "?");
        throw std::runtime_error("SDL video driver has no Vulkan surface");
    }
    if (!surface || (kind != latte::HostSurface::Windows && !display)) {
        LOG("FATAL: SDL did not expose a native window handle for Vulkan (driver %s)", driver);
        throw std::runtime_error("SDL did not expose a native window handle");
    }
    LOG("vulkan surface via %s", driver);
    latte::set_host_surface(kind, display, surface);
    latte::create_renderer(surface, point_w, point_h);
}
#endif
}  // namespace window

// Cemu's WindowSystem::GetWindowPhysSize (runtime/cemu_shims): window size in points, like Cemu's canvas.
void botw_window_phys_size(int& w, int& h) {
    if (window::g_win_ptr()) {
        SDL_GetWindowSizeInPixels(window::g_win_ptr(), &w, &h);  // Cemu wants physical pixels
    } else {
        w = 1280;
        h = 720;
    }
}
