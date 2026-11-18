// PC keyboard/mouse actions. Port of Zelda-on-PC's action list and default bindings
// (src/pcedition/PCSettings.cpp, MPL-2.0, https://github.com/mircea32000/Zelda-on-PC); the mapping of each
// action to GamePad input follows its PCInput.cpp. Selectors with the mouse, the wheel weapon switch, the
// Journal shortcut and Motion Control come with the gameplay extras (phase 5); their bindings are read and kept.
#include "pc_controls.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <mutex>

#include "../host/menu.h"
#include "../host/swkbd_state.h"
#include "../log.h"
#include "../os.h"
#include "../utf8_path.h"
#include "input.h"
#include "mouse.h"

bool pce_gameplay_camera();  // game/pc_camera.cpp: a player camera updated in the last 150 ms
bool pce_bow_camera();       // ... the zoomed aim camera (bow drawn)
bool pce_map_screen();       // game/pc_ui.cpp: the world map is on screen

namespace pccontrols {
namespace {

// ---- keys ------------------------------------------------------------------------------------------
struct Key {
    enum Type { None, Scan, Mouse, WheelUp, WheelDown } type = None;
    int code = 0;  // SDL_Scancode, or SDL mouse button (1 left, 2 middle, 3 right, 4 X1, 5 X2)
};

const struct {
    const char* name;
    int button;
} kMouseNames[] = {
    {"MouseLeft", SDL_BUTTON_LEFT}, {"MouseMiddle", SDL_BUTTON_MIDDLE}, {"MouseRight", SDL_BUTTON_RIGHT},
    {"Mouse4", SDL_BUTTON_X1},      {"Mouse5", SDL_BUTTON_X2},
};

bool parse_key(std::string name, Key& k) {
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r')) name.pop_back();
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
    k = {};
    if (name.empty()) return true;
    for (auto& m : kMouseNames)
        if (!SDL_strcasecmp(name.c_str(), m.name)) return k = {Key::Mouse, m.button}, true;
    if (!SDL_strcasecmp(name.c_str(), "WheelUp")) return k = {Key::WheelUp, 0}, true;
    if (!SDL_strcasecmp(name.c_str(), "WheelDown")) return k = {Key::WheelDown, 0}, true;
    SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    k = {Key::Scan, (int)sc};
    return true;
}

std::string key_name(const Key& k) {
    switch (k.type) {
        case Key::Scan: return SDL_GetScancodeName((SDL_Scancode)k.code);
        case Key::Mouse:
            for (auto& m : kMouseNames)
                if (m.button == k.code) return m.name;
            return "";
        case Key::WheelUp: return "WheelUp";
        case Key::WheelDown: return "WheelDown";
        default: return "";
    }
}

// ---- actions ---------------------------------------------------------------------------------------
enum Kind {
    Btn,       // GamePad buttons held while the action is
    MoveX,     // left stick, sign = direction
    MoveY,
    CamX,      // right stick, scaled by key_camera_speed
    CamY,
    Walk,      // left stick magnitude scaled by walk_speed
    Settings,  // opens the overlay menu (F1 menu)
    Selector,  // hold: opens a selector (frame()); bits = d-pad it holds
    WheelNext, // a wheel notch or press steps the weapon (arrow while aiming) selector
    WheelPrev,
    Later,     // journal (phase 11), motion control (phase 10)
};

struct ActionDef {
    const char* id;
    const char* name;
    Kind kind;
    uint32_t bits;  // Btn
    int sign;       // sticks
    const char* def1;
    const char* def2;
};

using namespace input;
const ActionDef kActions[] = {
    {"MoveForward", "Move Forward", MoveY, 0, +1, "W", ""},
    {"MoveBack", "Move Back", MoveY, 0, -1, "S", ""},
    {"MoveLeft", "Move Left", MoveX, 0, -1, "A", ""},
    {"MoveRight", "Move Right", MoveX, 0, +1, "D", ""},
    {"Walk", "Walk (hold)", Walk, 0, 0, "Left Ctrl", ""},
    {"Sprint", "Sprint / Cancel", Btn, BTN_B, 0, "Left Shift", ""},
    {"Jump", "Jump", Btn, BTN_X, 0, "Space", ""},
    {"Crouch", "Crouch", Btn, BTN_STICK_L, 0, "C", ""},
    {"Attack", "Attack", Btn, BTN_Y, 0, "MouseLeft", ""},
    {"Interact", "Action / Interact", Btn, BTN_A, 0, "E", ""},
    {"Bow", "Draw Bow", Btn, BTN_ZR, 0, "MouseRight", ""},
    {"Target", "Lock On / Shield", Btn, BTN_ZL, 0, "Q", "Mouse4"},
    {"UseRune", "Use Rune", Btn, BTN_L, 0, "F", ""},
    {"Throw", "Throw Weapon", Btn, BTN_R, 0, "R", ""},
    {"Scope", "Scope / Look", Btn, BTN_STICK_R, 0, "MouseMiddle", "Z"},
    {"Whistle", "Whistle", Btn, BTN_DOWN, 0, "X", ""},
    {"WeaponSelect", "Weapon Selector (hold)", Selector, BTN_RIGHT, 0, "1", ""},
    {"BowSelect", "Bow Selector (hold)", Selector, BTN_RIGHT, 0, "2", ""},  // with the aim button (ZR) first
    {"ShieldSelect", "Shield Selector (hold)", Selector, BTN_LEFT, 0, "3", ""},
    {"RuneSelect", "Rune Selector (hold)", Selector, BTN_UP, 0, "4", ""},
    {"WeaponNext", "Next Weapon / Arrow", WheelNext, 0, 0, "WheelDown", ""},
    {"WeaponPrev", "Previous Weapon / Arrow", WheelPrev, 0, 0, "WheelUp", ""},
    {"Inventory", "Inventory", Btn, BTN_PLUS, 0, "Tab", "I"},
    {"Map", "Map", Btn, BTN_MINUS, 0, "M", ""},
    {"Journal", "Adventure Log (Quests)", Later, 0, 0, "J", ""},
    {"MenuUp", "Menu Up", Btn, BTN_UP, 0, "Up", ""},
    {"MenuDown", "Menu Down", Btn, BTN_DOWN, 0, "Down", ""},
    {"MenuLeft", "Menu Left", Btn, BTN_LEFT, 0, "Left", ""},
    {"MenuRight", "Menu Right", Btn, BTN_RIGHT, 0, "Right", ""},
    {"Confirm", "Confirm", Btn, BTN_A, 0, "Return", ""},
    // Escape backs out of the game's menus. (Zelda-on-PC opens its settings with it; here that is the F1 overlay,
    // whose top row flips the frame rate with an arrow key, an easy accident when Escape meant "leave the menu".)
    {"Back", "Back", Btn, BTN_B, 0, "Backspace", "Escape"},
    {"CameraUp", "Camera Up", CamY, 0, +1, "", ""},
    {"CameraDown", "Camera Down", CamY, 0, -1, "", ""},
    {"CameraLeft", "Camera Left", CamX, 0, -1, "", ""},
    {"CameraRight", "Camera Right", CamX, 0, +1, "", ""},
    {"TiltMode", "Motion Control (Tilt)", Later, 0, 0, "T", ""},
    {"TiltLevel", "Motion Control: Level the Slate", Later, 0, 0, "MouseMiddle", ""},
    {"Settings", "Settings Menu", Settings, 0, 0, "F1", ""},
    {"MenuPrevPage", "Inventory: Previous Page", Btn, BTN_L, 0, "PageUp", ""},
    {"MenuNextPage", "Inventory: Next Page", Btn, BTN_R, 0, "PageDown", ""},
};
constexpr int kCount = (int)(sizeof(kActions) / sizeof(kActions[0]));

constexpr uint64_t kPulseMs = 50;  // a tap / click / wheel notch stays visible this long

struct Pulse {
    uint32_t bits;
    uint64_t until;
    uint64_t from = 0;  // (queued wheel notches: one press after the other)
    float rx = 0;       // right-stick flick instead of / with buttons
    float ry = 0;       // right stick held up / down (map zoom)
};

bool g_tried = false, g_on = false;
std::string g_path;
Key g_keys[kCount][2];
float g_walk_speed = 0.45f;
float g_key_camera_speed = 0.75f;
float g_wheel_release_ms = 450.0f;   // after the last wheel notch the selector closes (equips the item)
float g_selector_mouse_step = 60.0f; // mouse counts per selector step while a selector key is held
bool g_selector_invert_scroll = false;

// Host (apply) -> game reads (frame)
std::mutex g_share_lock;
struct Shared {
    bool down[kCount] = {};
    bool pressed[kCount] = {};  // went down since the last game read
    int wheel_steps = 0;        // + next, - previous
    bool enabled = false;       // window focused, no overlay menu or keyboard
} g_sh;
bool g_host_down[kCount] = {};
float g_wheel_acc = 0;
std::vector<Pulse> g_pulses;
uint32_t g_last_bits = 0;  // for the press log
const bool g_log = [] {
    const char* s = std::getenv("BOTW_PCE_INPUT_LOG");
    return s && *s && *s != '0';
}();

void set_defaults() {
    for (int i = 0; i < kCount; i++) {
        parse_key(kActions[i].def1, g_keys[i][0]);
        parse_key(kActions[i].def2, g_keys[i][1]);
    }
}

int find_action(const std::string& id) {
    for (int i = 0; i < kCount; i++)
        if (!SDL_strcasecmp(id.c_str(), kActions[i].id)) return i;
    return -1;
}

bool write_file(const std::filesystem::path& file) {
    FILE* f = botw::fopen_utf8(botw::path_to_utf8(file), "w");
    if (!f) return false;
    fprintf(f,
            "# BotW-Native PC controls (keyboard and mouse). Controllers are not affected.\n"
            "# action=key[, second key]. Keys are SDL key names (W, Space, Left Shift, Left Ctrl, Return, Tab,\n"
            "# Escape, F1, Up, PageUp, ...) or MouseLeft, MouseRight, MouseMiddle, Mouse4, Mouse5, WheelUp,\n"
            "# WheelDown. Leave empty for unbound. enabled=0 goes back to the built-in keys / controls.ini.\n"
            "enabled=%d\nwalk_speed=%g\nkey_camera_speed=%g\n"
            "# selectors: mouse counts per step while 1-4 is held; ms after the last wheel notch until it equips\n"
            "selector_mouse_step=%g\nwheel_release_ms=%g\nselector_invert_scroll=%d\n\n",
            g_on ? 1 : 0, g_walk_speed, g_key_camera_speed, g_selector_mouse_step, g_wheel_release_ms,
            g_selector_invert_scroll ? 1 : 0);
    for (int i = 0; i < kCount; i++) {
        std::string a = key_name(g_keys[i][0]), b = key_name(g_keys[i][1]);
        std::string v = a.empty() ? b : b.empty() ? a : a + ", " + b;
        fprintf(f, "%s=%s\n", kActions[i].id, v.c_str());
    }
    fclose(f);
    return true;
}

void ensure() {
    if (g_tried) return;
    g_tried = true;
    set_defaults();
    if (g_user_root.empty()) return;
    const std::filesystem::path file = botw::path_from_utf8(g_user_root) / "pc_controls.ini";
    g_path = botw::path_to_utf8(file);
    FILE* f = botw::fopen_utf8(g_path, "r");
    if (!f) {
        g_on = true;
        if (write_file(file)) LOG("pc controls: wrote defaults to %s", g_path.c_str());
        return;
    }
    g_on = true;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || !*p) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = p, v = eq + 1;
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        if (k == "enabled") g_on = atoi(v.c_str()) != 0;
        else if (k == "walk_speed") g_walk_speed = std::clamp((float)atof(v.c_str()), 0.05f, 1.0f);
        else if (k == "key_camera_speed") g_key_camera_speed = std::clamp((float)atof(v.c_str()), 0.05f, 1.0f);
        else if (k == "selector_mouse_step") g_selector_mouse_step = std::clamp((float)atof(v.c_str()), 1.0f, 10000.0f);
        else if (k == "wheel_release_ms") g_wheel_release_ms = std::clamp((float)atof(v.c_str()), 50.0f, 5000.0f);
        else if (k == "selector_invert_scroll") g_selector_invert_scroll = atoi(v.c_str()) != 0;
        else {
            int a = find_action(k);
            if (a < 0) {
                LOG("pc controls: line %d: unknown action \"%s\"", n, k.c_str());
                continue;
            }
            g_keys[a][0] = g_keys[a][1] = {};
            size_t comma = v.find(',');
            std::string k1 = v.substr(0, comma), k2 = comma == std::string::npos ? "" : v.substr(comma + 1);
            if (!parse_key(k1, g_keys[a][0])) LOG("pc controls: line %d: unknown key \"%s\" for %s", n, k1.c_str(), kActions[a].id);
            if (!parse_key(k2, g_keys[a][1])) LOG("pc controls: line %d: unknown key \"%s\" for %s", n, k2.c_str(), kActions[a].id);
        }
    }
    fclose(f);
    LOG("pc controls: %s %s", g_path.c_str(), g_on ? "(on)" : "(enabled=0, off)");
}

void pulse(uint32_t bits) {
    if (bits) g_pulses.push_back({bits, SDL_GetTicks() + kPulseMs});
}
}  // namespace

void load() { ensure(); }

bool active() {
    ensure();
    return g_on;
}

void key_down(SDL_Scancode sc) {
    if (!active()) return;
    uint32_t bits = 0;
    for (int i = 0; i < kCount; i++)
        if (kActions[i].kind == Btn)
            for (auto& k : g_keys[i])
                if (k.type == Key::Scan && k.code == (int)sc) bits |= kActions[i].bits;
    pulse(bits);
}

bool is_settings_key(SDL_Scancode sc) {
    if (!active()) return false;
    for (int i = 0; i < kCount; i++)
        if (kActions[i].kind == Settings)
            for (auto& k : g_keys[i])
                if (k.type == Key::Scan && k.code == (int)sc) return true;
    return false;
}

void clear_pulses() { g_pulses.clear(); }

void apply(SDL_Window* win, PadState& s) {
    if (!active()) return;
    const bool focused = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS);
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool alt = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
    const uint32_t mbuttons = mouse::buttons();
    const uint32_t mpressed = mouse::take_pressed();
    g_wheel_acc += mouse::take_wheel();
    const int notches = (int)g_wheel_acc;  // whole notches (high-resolution wheels send fractions)
    g_wheel_acc -= notches;
    bool sel_down[kCount] = {};
    int wheel_steps = 0;
    // Outside gameplay (inventory, map, title, cutscenes) the mouse buttons have no gameplay meaning: right click
    // would be ZR (next pause-menu page). Until the menus take the mouse (phase 11) they do nothing there, and
    // the wheel steps the inventory categories (weapons, bows, ... food): a right-stick flick per notch.
    const bool in_play = pce_gameplay_camera();

    float mx = 0, my = 0, cx = 0, cy = 0;
    bool walk = false;
    uint32_t bits = 0, tapped = 0;
    for (int i = 0; i < kCount; i++) {
        const ActionDef& a = kActions[i];
        bool down = false, tap = false;
        int wheel_n = 0;
        for (auto& k : g_keys[i]) {
            switch (k.type) {
                case Key::Scan:
                    // Alt+Tab switches windows; it is not the inventory
                    if (focused && keys[k.code] && !(alt && k.code == SDL_SCANCODE_TAB)) down = true;
                    break;
                case Key::Mouse:
                    if (!in_play) break;
                    if (mbuttons & SDL_BUTTON_MASK(k.code)) down = true;
                    if (mpressed & SDL_BUTTON_MASK(k.code)) tap = true;
                    break;
                case Key::WheelUp: if (notches > 0) tap = true, wheel_n += notches; break;
                case Key::WheelDown: if (notches < 0) tap = true, wheel_n -= notches; break;
                default: break;
            }
        }
        if (a.kind == WheelNext || a.kind == WheelPrev) {
            const int n = wheel_n + (down && !g_host_down[i] ? 1 : 0);  // a key: one step per press
            wheel_steps += a.kind == WheelNext ? n : -n;
        }
        sel_down[i] = down;
        switch (a.kind) {
            case Btn:
                if (down) bits |= a.bits;
                if (tap) tapped |= a.bits;
                break;
            case MoveX: if (down) mx += a.sign; break;
            case MoveY: if (down) my += a.sign; break;
            case CamX: if (down) cx += a.sign; break;
            case CamY: if (down) cy += a.sign; break;
            case Walk: walk |= down; break;
            default: break;
        }
    }
    pulse(tapped);
    if (!in_play && notches && focused && pce_map_screen()) {
        // map: zoom with the right stick held up (wheel up = in) or down, ~5 frames per notch, at most 4 notches;
        // a new notch replaces what is left of the last one (Zelda-on-PC PCMenus.cpp)
        g_pulses.erase(std::remove_if(g_pulses.begin(), g_pulses.end(), [](const Pulse& p) { return p.ry != 0; }),
                       g_pulses.end());
        const uint64_t t = SDL_GetTicks();
        const uint64_t ms = 167 * (uint64_t)std::min(4, std::abs(notches));
        g_pulses.push_back({0, t + ms, t, 0, notches > 0 ? 1.0f : -1.0f});
    } else if (!in_play && notches && focused) {
        // one right-stick flick per notch, 100 ms apart; wheel up = previous category, down = next
        uint64_t t = SDL_GetTicks();
        for (const Pulse& p : g_pulses) t = std::max(t, p.from + 100);
        for (int n = std::abs(notches); n > 0; n--, t += 100)
            g_pulses.push_back({0, t + kPulseMs, t, notches > 0 ? -1.0f : 1.0f});
    }
    {
        std::lock_guard<std::mutex> l(g_share_lock);
        for (int i = 0; i < kCount; i++) {
            if (sel_down[i] && !g_host_down[i]) g_sh.pressed[i] = true;
            g_sh.down[i] = sel_down[i];
            g_host_down[i] = sel_down[i];
        }
        g_sh.wheel_steps += wheel_steps;
        g_sh.enabled = focused && !menu::is_open() && !swkbd::keyboard().view().active;
    }

    const uint64_t now = SDL_GetTicks();
    for (auto it = g_pulses.begin(); it != g_pulses.end();) {
        if (it->until < now) {
            it = g_pulses.erase(it);
            continue;
        }
        if (it->from <= now) {
            bits |= it->bits;
            if (it->rx != 0) cx = it->rx / g_key_camera_speed;  // (scaled back below: a full flick)
            if (it->ry != 0) cy = it->ry / g_key_camera_speed;
        }
        ++it;
    }

    if (mx != 0 && my != 0) mx *= 0.70710678f, my *= 0.70710678f;
    if (walk) mx *= g_walk_speed, my *= g_walk_speed;
    s.buttons |= bits;
    s.lx += mx;
    s.ly += my;
    s.rx += cx * g_key_camera_speed;
    s.ry += cy * g_key_camera_speed;

    if (g_log && bits != g_last_bits) {
        const uint32_t pressed = bits & ~g_last_bits;
        if (pressed) {
            std::string names;
            for (int i = 0; i < kCount; i++)
                if (kActions[i].kind == Btn && (kActions[i].bits & pressed) == kActions[i].bits) {
                    if (!names.empty()) names += ", ";
                    names += kActions[i].id;
                }
            LOG("pc controls: pressed 0x%05x (%s)", pressed, names.c_str());
        }
    }
    g_last_bits = bits;
}

// ---- selectors (per game read) -------------------------------------------------------------------------
namespace {
struct Sel {
    bool active = false;
    bool key_driven = false;  // a selector key is held (else opened by the wheel)
    int src = -1;             // action that opened it
    uint32_t pre = 0;         // held first (bow selector: the aim button)
    uint32_t hold = 0;        // d-pad held while open
    int frame = 0;
    int pending = 0;          // steps still to flick
    int flick_dir = 0, flick_frame = 0;
    int close_frame = -1;     // bow selector: frames since the d-pad was released (aim still held)
    int cancel_frame = -1;    // ... frames the cancel button has been held
    double mouse_acc = 0;
    uint64_t last_activity_ms = 0;
};
Sel g_sel;
uint64_t g_sel_closed_ms = 0;
std::atomic<bool> g_owns_mouse{false};
// Output of the last advancing read, repeated on the sampling thread's reads.
struct Out {
    uint32_t buttons = 0;
    bool stick = false;
    float rx = 0;
} g_out;
std::mutex g_frame_lock;
bool g_was_gameplay = false;

int action_index(const char* id) {
    for (int i = 0; i < kCount; i++)
        if (!strcmp(kActions[i].id, id)) return i;
    return -1;
}

void apply_out(PadState& s, const Out& o) {
    s.buttons |= o.buttons;
    if (o.stick) {
        s.rx = o.rx;
        s.ry = 0;
    }
}
}  // namespace

bool owns_mouse() { return g_owns_mouse; }

void frame(PadState& s, bool advance) {
    if (!active()) return;
    std::lock_guard<std::mutex> fl(g_frame_lock);
    if (!advance) return apply_out(s, g_out);

    static const int kBow = action_index("Bow");
    Shared sh;
    {
        std::lock_guard<std::mutex> l(g_share_lock);
        sh = g_sh;
        for (bool& p : g_sh.pressed) p = false;
        g_sh.wheel_steps = 0;
    }
    const uint64_t now = SDL_GetTicks();
    const bool gameplay = pce_gameplay_camera();
    if (g_log && gameplay != g_was_gameplay) LOG("pc controls: gameplay camera %s", gameplay ? "active" : "inactive");
    g_was_gameplay = gameplay;
    static bool was_map = false;
    const bool map = pce_map_screen();
    if (g_log && map != was_map) LOG("pc controls: world map %s", map ? "shown" : "gone");
    was_map = map;
    const bool enabled = sh.enabled;
    const bool allowed = enabled && gameplay;
    // aiming: the aim button held (mouse, key or controller) and a bow drawn (its camera is on)
    const bool aiming = (s.buttons & BTN_ZR) != 0 && pce_bow_camera();

    Sel& sel = g_sel;
    auto open = [&](int src, bool key_driven) {
        sel = {};
        sel.active = true;
        sel.key_driven = key_driven;
        sel.src = src;
        sel.last_activity_ms = now;
        const ActionDef& a = kActions[src];
        if (a.kind == Selector) {
            sel.hold = a.bits;
            if (!strcmp(a.id, "BowSelect")) sel.pre = BTN_ZR;  // with the aim button held, d-pad right = bows
        } else {
            sel.hold = aiming ? BTN_LEFT : BTN_RIGHT;  // wheel: arrows while aiming, weapons otherwise
        }
        if (g_log) LOG("pc controls: selector opens (%s)", a.id);
    };

    if (!sel.active && allowed)
        for (int i = 0; i < kCount; i++)
            if (kActions[i].kind == Selector && sh.pressed[i]) {
                open(i, true);
                break;
            }
    // A selector of ours counts as the game's menu for a moment after closing: wheel steps right after go on.
    const bool own = sel.active ? sel.close_frame < 0 : (g_sel_closed_ms && now - g_sel_closed_ms < 250);
    int steps = sh.wheel_steps * (g_selector_invert_scroll ? -1 : 1);
    if (steps && (allowed || (own && enabled))) {
        if (!sel.active) open(action_index(steps > 0 ? "WeaponNext" : "WeaponPrev"), false);
        sel.pending += steps;
        sel.last_activity_ms = now;
    }

    Out o;
    if (sel.active && sel.close_frame >= 0) {
        // Bow selector closing: Link has an arrow nocked (the selector held the aim button), so letting go would
        // shoot. The d-pad is released first (equips), then the shot is cancelled with B for a few frames.
        o.buttons |= sel.pre;
        o.stick = true;
        if (sel.cancel_frame < 0 && sel.close_frame >= 30) sel.cancel_frame = 0;
        sel.close_frame++;
        if (sel.cancel_frame >= 0) {
            o.buttons |= BTN_B;
            if (++sel.cancel_frame >= 10) {
                sel.active = false;
                g_sel_closed_ms = now;
                if (g_log) LOG("pc controls: bow selector closed (shot cancelled)");
            }
        }
    } else if (sel.active) {
        constexpr int kPreFrames = 6;   // let ZR register (bow drawn) before the d-pad
        constexpr int kOpenFrames = 4;  // d-pad held before the first flick, so the carousel is shown
        constexpr int kFlickOn = 2, kFlickOff = 2;
        const int pre = sel.pre ? kPreFrames : 0;
        o.stick = true;
        o.buttons |= sel.pre;
        if (sel.frame >= pre) o.buttons |= sel.hold;
        if (sel.key_driven) {  // mouse X steps the selection while the key is held
            sel.mouse_acc += mouse::take().dx;
            const double step = g_selector_mouse_step;
            while (sel.mouse_acc >= step) sel.pending++, sel.mouse_acc -= step;
            while (sel.mouse_acc <= -step) sel.pending--, sel.mouse_acc += step;
        }
        if (sel.frame >= pre + kOpenFrames) {
            if (sel.flick_dir == 0 && sel.pending != 0) {
                sel.flick_dir = sel.pending > 0 ? 1 : -1;
                sel.pending -= sel.flick_dir;
                sel.flick_frame = 0;
            }
            if (sel.flick_dir != 0) {
                if (sel.flick_frame < kFlickOn) o.rx = (float)sel.flick_dir;
                if (++sel.flick_frame >= kFlickOn + kFlickOff) sel.flick_dir = 0;
            }
        }
        sel.frame++;
        bool want_close = sel.key_driven ? !sh.down[sel.src] || !enabled
                                         : now - sel.last_activity_ms > (uint64_t)g_wheel_release_ms || !enabled;
        if (sel.pre && sel.frame < 40 && enabled) want_close = false;  // the bow selector waits until it is shown
        if (want_close && sel.flick_dir == 0 && sel.pending == 0) {
            // releasing the d-pad equips the highlighted item
            if (sel.pre && !(kBow >= 0 && sh.down[kBow]) && enabled) {
                sel.close_frame = 0;  // (not when the player aims: their own button keeps the bow drawn)
            } else {
                sel.active = false;
                g_sel_closed_ms = now;
                if (g_log) LOG("pc controls: selector closed");
            }
        }
    }
    g_owns_mouse = sel.active && sel.close_frame < 0;  // (the wheel selector: the mouse is just not the camera's)
    g_out = o;
    apply_out(s, o);
}

BoundKey bound_key(const char* action_id, int slot) {
    ensure();
    const int i = action_index(action_id);
    if (i < 0 || slot < 0 || slot > 1) return {};
    const Key& k = g_keys[i][slot];
    switch (k.type) {
        case Key::Scan: return {1, k.code};
        case Key::Mouse: return {2, k.code};
        case Key::WheelUp: return {3, 0};
        case Key::WheelDown: return {4, 0};
        default: return {};
    }
}

void log_bindings() {
    if (!active()) return;
    std::string line;
    int shown = 0;
    for (int i = 0; i < kCount; i++) {
        std::string a = key_name(g_keys[i][0]), b = key_name(g_keys[i][1]);
        if (a.empty() && b.empty()) continue;
        line += std::string(kActions[i].id) + "=" + (a.empty() ? b : b.empty() ? a : a + "/" + b) +
                (kActions[i].kind == Later ? "(not yet)" : "") + "  ";
        if (++shown % 8 == 0) {
            LOG("pc controls: %s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) LOG("pc controls: %s", line.c_str());
}
}  // namespace pccontrols
