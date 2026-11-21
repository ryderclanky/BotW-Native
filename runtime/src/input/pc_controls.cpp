// PC keyboard/mouse actions. Port of Zelda-on-PC's action list and default bindings
// (src/pcedition/PCSettings.cpp, MPL-2.0, https://github.com/mircea32000/Zelda-on-PC); the mapping of each
// action to GamePad input follows its PCInput.cpp. Selectors with the mouse, the wheel weapon switch and Motion
// Control are run per game read (frame()); so is the mouse in the game's menus and the Journal key (pc_menus.cpp).
#include "pc_controls.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <mutex>

#include "../host/menu.h"
#include "../host/swkbd_state.h"
#include "../log.h"
#include "../os.h"
#include "../utf8_path.h"
#include "input.h"
#include "mouse.h"
#include "pc_menus.h"
#include "../game/pc_ui.h"
#include "pc_tilt.h"

bool pce_gliding();  // game/pc_camera.cpp: the glide state ticked in the last 70 ms
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

// With Num Lock off the numpad is a second set of navigation keys (Windows: 9 = Page Up, 1 = End, 8 = Up, ...),
// but SDL keeps reporting the physical key (Keypad 9). A binding of PageUp etc. also takes its numpad key then.
const std::pair<SDL_Scancode, SDL_Scancode> kNumpadNav[] = {
    {SDL_SCANCODE_KP_9, SDL_SCANCODE_PAGEUP}, {SDL_SCANCODE_KP_3, SDL_SCANCODE_PAGEDOWN},
    {SDL_SCANCODE_KP_7, SDL_SCANCODE_HOME},   {SDL_SCANCODE_KP_1, SDL_SCANCODE_END},
    {SDL_SCANCODE_KP_8, SDL_SCANCODE_UP},     {SDL_SCANCODE_KP_2, SDL_SCANCODE_DOWN},
    {SDL_SCANCODE_KP_4, SDL_SCANCODE_LEFT},   {SDL_SCANCODE_KP_6, SDL_SCANCODE_RIGHT},
    {SDL_SCANCODE_KP_0, SDL_SCANCODE_INSERT}, {SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_DELETE},
};
bool num_lock_off() { return !(SDL_GetModState() & SDL_KMOD_NUM); }

// the navigation key a numpad key is right now (Num Lock off), else the key itself
SDL_Scancode numpad_nav(SDL_Scancode sc) {
    if (num_lock_off())
        for (auto [kp, nav] : kNumpadNav)
            if (kp == sc) return nav;
    return sc;
}

// a bound key held: the key, or with Num Lock off the numpad key that is this navigation key
bool scan_held(const bool* keys, int code) {
    if (keys[code]) return true;
    if (num_lock_off())
        for (auto [kp, nav] : kNumpadNav)
            if (nav == code && keys[kp]) return true;
    return false;
}

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
    Tilt,      // Motion Control: TiltMode turns it on / off, TiltLevel levels the slate (frame())
    Journal,   // pause menu on the adventure log (pc_menus)
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
    {"Walk", "Walk", Walk, 0, 0, "Left Ctrl", ""},
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
    {"Journal", "Adventure Log (Quests)", Journal, 0, 0, "J", ""},
    {"MenuUp", "Menu Up", Btn, BTN_UP, 0, "Up", ""},
    {"MenuDown", "Menu Down", Btn, BTN_DOWN, 0, "Down", ""},
    {"MenuLeft", "Menu Left", Btn, BTN_LEFT, 0, "Left", ""},
    {"MenuRight", "Menu Right", Btn, BTN_RIGHT, 0, "Right", ""},
    {"Confirm", "Confirm", Btn, BTN_A, 0, "Return", ""},
    {"Back", "Back", Btn, BTN_B, 0, "Backspace", ""},
    // Escape: in play it opens the pause menu on the System page; in any menu or dialog it backs out (B), see
    // pc_menus. (Zelda-on-PC opens its settings with it; here that is F1.)
    {"System", "System Menu / Back", Journal, 0, 0, "Escape", ""},
    {"SaveMenu", "Save (opens the game's Save)", Journal, 0, 0, "F5", ""},
    {"LoadMenu", "Load (opens the game's Load)", Journal, 0, 0, "F9", ""},
    {"CameraUp", "Camera Up", CamY, 0, +1, "", ""},
    {"CameraDown", "Camera Down", CamY, 0, -1, "", ""},
    {"CameraLeft", "Camera Left", CamX, 0, -1, "", ""},
    {"CameraRight", "Camera Right", CamX, 0, +1, "", ""},
    {"TiltMode", "Motion Control (Tilt)", Tilt, 0, 0, "T", ""},
    {"TiltLevel", "Motion Control: Level the Slate", Tilt, 0, 0, "MouseMiddle", ""},
    {"Settings", "Settings Menu", Settings, 0, 0, "F1", ""},
    {"MenuPrevPage", "Inventory: Previous Page", Btn, BTN_L, 0, "F", ""},  // menus only (not Use Rune)
    {"MenuNextPage", "Inventory: Next Page", Btn, BTN_R, 0, "R", ""},  // menus only (not Throw)
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
bool g_walk_toggle = false;  // Walk key toggles walking (else: walk while held)
bool g_walk_latched = false;
bool g_sprint_toggle = false;  // Sprint key toggles sprinting (B held) until pressed again
bool g_sprint_latched = false;
bool g_tilt_toggle = false;  // TiltMode toggles Motion Control (else: on while held)
pctilt::Config g_tilt;
std::atomic<bool> g_tilt_on{false};

// Host (apply) -> game reads (frame)
std::mutex g_share_lock;
struct Shared {
    bool down[kCount] = {};
    bool pressed[kCount] = {};  // went down since the last game read
    int wheel_steps = 0;        // + next, - previous
    int tilt_turn = 0;          // wheel notches while Motion Control is on (+ up)
    bool enabled = false;       // window focused, no overlay menu or keyboard
    uint32_t mouse_pressed = 0; // mouse buttons pressed since the last game read (menus)
    int menu_wheel = 0;         // wheel notches while a game menu has the pointer (+ up)
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
            "selector_mouse_step=%g\nwheel_release_ms=%g\nselector_invert_scroll=%d\n"
            "# Motion Control (TiltMode): the mouse tilts the GamePad, the wheel turns it flat. tilt_toggle=0: on\n"
            "# while the key is held. Sensitivity x 0.05 degrees per count; degrees, ms, degrees per second.\n"
            "# walk_toggle=1: the Walk key switches walking on/off instead of walking while held\n"
            "walk_toggle=%d\nsprint_toggle=%d\ntilt_toggle=%d\ntilt_sens=%g\ntilt_invert_x=%d\ntilt_invert_y=%d\ntilt_max_angle=%g\n"
            "tilt_return_speed=%g\ntilt_turn_step=%g\ntilt_invert_turn=%d\ntilt_smoothing=%g\ntilt_max_speed=%g\n\n",
            g_on ? 1 : 0, g_walk_speed, g_key_camera_speed, g_selector_mouse_step, g_wheel_release_ms,
            g_selector_invert_scroll ? 1 : 0, g_walk_toggle ? 1 : 0, g_sprint_toggle ? 1 : 0, g_tilt_toggle ? 1 : 0, g_tilt.sens, g_tilt.invert_x ? 1 : 0,
            g_tilt.invert_y ? 1 : 0, g_tilt.max_angle, g_tilt.return_speed, g_tilt.turn_step, g_tilt.invert_turn ? 1 : 0,
            g_tilt.smoothing_ms, g_tilt.max_speed);
    fprintf(f, "# the inventory's page keys (L / R) are their own; Use Rune and Throw are gameplay only\nmenu_page_keys=1\n");
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
    bool saw_system = false, saw_page_keys = false;
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
        else if (k == "sprint_toggle") g_sprint_toggle = atoi(v.c_str()) != 0;
        else if (k == "walk_toggle") g_walk_toggle = atoi(v.c_str()) != 0;
        else if (k == "menu_page_keys") saw_page_keys = true;
        else if (k == "tilt_toggle") g_tilt_toggle = atoi(v.c_str()) != 0;
        else if (k == "tilt_sens") g_tilt.sens = std::clamp((float)atof(v.c_str()), 0.01f, 50.0f);
        else if (k == "tilt_invert_x") g_tilt.invert_x = atoi(v.c_str()) != 0;
        else if (k == "tilt_invert_y") g_tilt.invert_y = atoi(v.c_str()) != 0;
        else if (k == "tilt_max_angle") g_tilt.max_angle = std::clamp((float)atof(v.c_str()), 1.0f, 180.0f);
        else if (k == "tilt_return_speed") g_tilt.return_speed = std::clamp((float)atof(v.c_str()), 0.0f, 1000.0f);
        else if (k == "tilt_turn_step") g_tilt.turn_step = std::clamp((float)atof(v.c_str()), 0.0f, 180.0f);
        else if (k == "tilt_invert_turn") g_tilt.invert_turn = atoi(v.c_str()) != 0;
        else if (k == "tilt_smoothing") g_tilt.smoothing_ms = std::clamp((float)atof(v.c_str()), 0.0f, 1000.0f);
        else if (k == "tilt_max_speed") g_tilt.max_speed = std::clamp((float)atof(v.c_str()), 10.0f, 3600.0f);
        else {
            int a = find_action(k);
            if (a < 0) {
                LOG("pc controls: line %d: unknown action \"%s\"", n, k.c_str());
                continue;
            }
            if (std::string(kActions[a].id) == "System") saw_system = true;
            g_keys[a][0] = g_keys[a][1] = {};
            size_t comma = v.find(',');
            std::string k1 = v.substr(0, comma), k2 = comma == std::string::npos ? "" : v.substr(comma + 1);
            if (!parse_key(k1, g_keys[a][0])) LOG("pc controls: line %d: unknown key \"%s\" for %s", n, k1.c_str(), kActions[a].id);
            if (!parse_key(k2, g_keys[a][1])) LOG("pc controls: line %d: unknown key \"%s\" for %s", n, k2.c_str(), kActions[a].id);
        }
    }
    fclose(f);
    if (!saw_system) {
        // file from before the System key: Escape was Back's second key; it moves to System (Esc still backs out)
        const int back = find_action("Back"), sys = find_action("System");
        Key esc;
        if (back >= 0 && sys >= 0 && parse_key("Escape", esc))
            for (Key& k : g_keys[back])
                if (k.type == esc.type && k.code == esc.code) k = {};
    }
    if (!saw_page_keys) {
        // file from before the page keys were split off: the inventory got L / R from Use Rune and Throw. Their
        // first keys join the page keys (in an empty slot), so those keys keep working in the inventory.
        const std::pair<const char*, const char*> kPairs[] = {{"UseRune", "MenuPrevPage"}, {"Throw", "MenuNextPage"}};
        for (auto [from, to] : kPairs) {
            const int a = find_action(from), b = find_action(to);
            if (a < 0 || b < 0 || g_keys[a][0].type == Key::None) continue;
            const Key src = g_keys[a][0];
            bool have = false;
            for (const Key& k : g_keys[b]) have |= k.type == src.type && k.code == src.code;
            if (have) continue;
            if (g_keys[b][0].type == Key::None) g_keys[b][0] = src;
            else if (g_keys[b][1].type == Key::None) g_keys[b][1] = src;
            else continue;
            LOG("pc controls: %s gets %s's key %s (inventory pages)", to, from, key_name(src).c_str());
        }
    }
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

// In the game's menus (a menu has the pointer) the Crouch key is the GamePad's Y: Sort in the inventory (its prompt
// shows the Crouch key there, see pc_ui.cpp). In play it stays the left stick click.
// L / R are split: in the menus only the page keys (MenuPrevPage / MenuNextPage, F / R by default) send them, in
// play only Use Rune and Throw do, so each pair can be bound on its own.
uint32_t action_bits(int i, bool pointer) {
    static const int kCrouch = find_action("Crouch"), kUseRune = find_action("UseRune"),
                     kThrow = find_action("Throw"), kPrevPage = find_action("MenuPrevPage"),
                     kNextPage = find_action("MenuNextPage");
    if (pointer && i == kCrouch) return BTN_Y;
    if (pointer && (i == kUseRune || i == kThrow)) return 0;
    if (!pointer && (i == kPrevPage || i == kNextPage)) return 0;
    return kActions[i].bits;
}

void key_down(SDL_Scancode sc) {
    if (!active()) return;
    const bool pointer = mouse::pointer_mode();
    const int nav = (int)numpad_nav(sc);  // Keypad 9 with Num Lock off is also Page Up
    uint32_t bits = 0;
    for (int i = 0; i < kCount; i++)
        if (kActions[i].kind == Btn)
            for (auto& k : g_keys[i])
                if (k.type == Key::Scan && (k.code == (int)sc || k.code == nav)) bits |= action_bits(i, pointer);
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
    int notches = (int)g_wheel_acc;  // whole notches (high-resolution wheels send fractions)
    g_wheel_acc -= notches;
    bool sel_down[kCount] = {};
    int wheel_steps = 0;
    // Outside gameplay (inventory, map, title, cutscenes) the mouse buttons have no gameplay meaning: right click
    // would be ZR (next pause-menu page). Until the menus take the mouse (phase 11) they do nothing there, and
    // the wheel steps the inventory categories (weapons, bows, ... food): a right-stick flick per notch.
    // Motion Control: the tilt puzzles' camera may not be one of ours, the mouse buttons still work; the wheel
    // turns the slate flat (and does nothing else), and a key of "Level the Slate" does only that (by default
    // the middle button, which is also the Scope's).
    const bool tilt = g_tilt_on.load();
    // a game menu has the pointer (pc_menus): mouse buttons and wheel are the menu's
    const bool pointer = mouse::pointer_mode();
    const bool in_play = (pce_gameplay_camera() || tilt) && !pointer;
    int tilt_turn = 0, menu_wheel = 0;
    if (pointer) menu_wheel = notches, notches = 0;
    else if (tilt) tilt_turn = notches, notches = 0;
    static const int kTiltLevel = [] {
        for (int i = 0; i < kCount; i++)
            if (!strcmp(kActions[i].id, "TiltLevel")) return i;
        return -1;
    }();
    auto level_key = [&](const Key& k) {
        if (!tilt || kTiltLevel < 0 || k.type == Key::None) return false;
        for (const Key& l : g_keys[kTiltLevel])
            if (l.type == k.type && l.code == k.code) return true;
        return false;
    };

    static const int kSprintIdx = find_action("Sprint");
    if (!g_sprint_toggle) g_sprint_latched = false;
    float mx = 0, my = 0, cx = 0, cy = 0;
    bool walk = false;
    uint32_t bits = 0, tapped = 0;
    for (int i = 0; i < kCount; i++) {
        const ActionDef& a = kActions[i];
        bool down = false, tap = false;
        int wheel_n = 0;
        for (auto& k : g_keys[i]) {
            if (a.kind != Tilt && level_key(k)) continue;
            switch (k.type) {
                case Key::Scan:
                    // Alt+Tab switches windows; it is not the inventory
                    if (focused && scan_held(keys, k.code) && !(alt && k.code == SDL_SCANCODE_TAB)) down = true;
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
                if (g_sprint_toggle && i == kSprintIdx) {
                    // toggle: a press latches B (sprint) on, the next press lets go; only in gameplay
                    if (!in_play) g_sprint_latched = false;
                    else if ((down && !g_host_down[i]) || tap) g_sprint_latched = !g_sprint_latched;
                    if (g_sprint_latched) bits |= action_bits(i, pointer);
                    break;
                }
                if (down) bits |= action_bits(i, pointer);
                if (tap) tapped |= action_bits(i, pointer);
                break;
            case MoveX: if (down) mx += a.sign; break;
            case MoveY: if (down) my += a.sign; break;
            case CamX: if (down) cx += a.sign; break;
            case CamY: if (down) cy += a.sign; break;
            case Walk:
                if (g_walk_toggle) {
                    if ((down && !g_host_down[i]) || tap) g_walk_latched = !g_walk_latched;
                } else
                    walk |= down;
                break;
            default: break;
        }
    }
    if (g_walk_toggle) walk |= g_walk_latched;
    else g_walk_latched = false;
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
        g_sh.tilt_turn += tilt_turn;
        g_sh.menu_wheel += menu_wheel;
        if (pointer) g_sh.mouse_pressed |= mpressed;
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
    uint32_t remove = 0;  // GamePad buttons taken away (context controls: the key means another button now)
    uint32_t buttons = 0;
    bool stick = false;
    float rx = 0;
} g_out;
// ... and the menus' (pointer clicks, map drag / zoom, category flicks)
struct MenuOut {
    uint32_t buttons = 0;
    bool l = false, r = false;
    float lx = 0, ly = 0, rx = 0, ry = 0;
} g_menu_out;
bool g_prev_lmb = false, g_prev_rmb = false;
std::mutex g_frame_lock;
bool g_was_gameplay = false;
// Motion Control: the virtual GamePad's motion of the last advancing read (under g_frame_lock)
input::Motion g_tilt_motion{};
bool g_tilt_report = false;
uint64_t g_tilt_last_ns = 0;

int action_index(const char* id) {
    for (int i = 0; i < kCount; i++)
        if (!strcmp(kActions[i].id, id)) return i;
    return -1;
}

// ---- context controls (Zelda-on-PC PCInput.cpp): the game's own action guides decide what a key does ----
struct Remap {
    uint32_t remove = 0, add = 0;
    int frames = 0;      // kept a few reads after the key is up (a quick click is a 50 ms pulse)
    uint32_t latch = 0;  // after the guide is gone, the original button stays off until the key is released
};
Remap g_remap[kCount];
bool g_climb_take = false;
bool g_glide_close = false;  // the jump key is closing the paraglider (until it is released)
uint64_t g_dodge_ms = 0;
uint32_t g_pp_button = 0;  // Magnesis push / pull and scope zoom: d-pad held for some reads per wheel notch
int g_pp_frames = 0;
std::string g_ctx_log, g_guides_log;

void context(const Shared& sh, int& wheel_steps, uint32_t& remove, uint32_t& add, uint64_t now) {
    static const int kAttack = action_index("Attack"), kBow = action_index("Bow"), kInteract = action_index("Interact"),
                     kJump = action_index("Jump"), kSprint = action_index("Sprint"), kTarget = action_index("Target");
    const pceui::State& ui = pceui::state();
    auto down = [&](int i) { return i >= 0 && (sh.down[i] || sh.pressed[i]); };
    bool remapped[kCount] = {};
    std::string names;
    auto remap = [&](int a, uint32_t rem, uint32_t to, const char* why, bool latch = true) {
        if (a < 0) return;
        g_remap[a] = {rem, to, 3, latch ? rem : 0u};
        remapped[a] = true;
        names += names.empty() ? why : std::string(", ") + why;
    };
    if (g_log) {
        std::string gl;
        for (auto& g : ui.guides) gl += (gl.empty() ? "" : ", ") + g.text;
        if (gl != g_guides_log) LOG("pc ui: guides [%s]", gl.c_str());
        g_guides_log = gl;
    }

    if (down(kTarget) && kJump >= 0 && sh.pressed[kJump]) g_dodge_ms = now;
    const bool after_dodge = g_dodge_ms && now - g_dodge_ms < 3000;
    if (ui.has("Skip") && down(kInteract)) remap(kInteract, BTN_A, ui.button_for("Skip", BTN_X), "skip");
    bool push_pull = false;
    if (ui.scope || ui.camera_rune) {
        // the keys its prompts show: click = Capture / Place pin, right click = Quit, wheel = zoom (d-pad up / down)
        if (down(kAttack)) remap(kAttack, BTN_Y, BTN_A, "scope click");
        if (down(kBow)) remap(kBow, BTN_ZR, BTN_B, "scope quit"), g_pp_frames = 0;
        if (wheel_steps) {
            const uint32_t b = wheel_steps < 0 ? BTN_UP : BTN_DOWN;  // wheel up = zoom in
            if (g_pp_button != b) g_pp_frames = 0;
            g_pp_button = b;
            g_pp_frames = std::min(60, g_pp_frames + 5 * std::abs(wheel_steps));
            wheel_steps = 0;
        }
        push_pull = true;
    } else if (!ui.any_menu) {
        if (ui.button_for("Dash", 0) == BTN_X && (down(kSprint) || down(kBow)))
            remap(down(kSprint) ? kSprint : kBow, BTN_B | BTN_ZR, BTN_X, "swim dash", false);
        if (ui.has("Let go")) {
            const uint32_t let_go = ui.button_for("Let go", BTN_B);
            // the interact key lets go of the wall, unless the game offers something on its own button (a fruit in
            // reach: "Take"), decided when the key goes down
            if (kInteract >= 0 && sh.pressed[kInteract]) g_climb_take = !ui.action_a.empty();
            if (down(kInteract) && !g_climb_take) remap(kInteract, BTN_A, let_go, "let go");
            if (!ui.has("Jump") && down(kJump)) remap(kJump, BTN_X, let_go, "let go (jump)");
        }
        if (!down(kInteract)) g_climb_take = false;
        // paraglider (Zelda-on-PC): a new jump press while gliding puts it away (the game's B); the key stays
        // remapped until it is released. Not with something in the hands ("Throw": a cucco glides on its own)
        if (kJump >= 0 && sh.pressed[kJump] && pce_gliding() && !ui.has("Throw")) g_glide_close = true;
        if (!down(kJump)) g_glide_close = false;
        if (g_glide_close) remap(kJump, BTN_X, BTN_B, "close paraglider");
        const bool cancel = ui.has("Cancel"), detonate = ui.has("Detonate"), throw_it = ui.has("Throw");
        const bool held = ui.held_items();
        if (throw_it && down(kAttack)) remap(kAttack, BTN_Y, ui.button_for("Throw", BTN_R), "throw");
        else if (held && down(kAttack)) remap(kAttack, BTN_Y, ui.button_for("Drop", BTN_A), "drop");
        else if (cancel && down(kAttack)) remap(kAttack, BTN_Y, BTN_A, "rune activate");
        if (detonate && down(kBow)) remap(kBow, BTN_ZR, ui.button_for("Detonate", BTN_L), "detonate");
        else if (cancel && down(kBow)) remap(kBow, BTN_ZR, ui.button_for("Cancel", BTN_B), "rune cancel");
        else if (held && down(kBow)) remap(kBow, BTN_ZR, ui.button_for("Put away", BTN_B), "put away");
        // Magnesis: the wheel pushes / pulls (never with something to throw: a bomb's Drop is not the d-pad)
        push_pull = (ui.has("Push") || ui.has("Pull") || ui.has("Far") || ui.has("Near")) && !throw_it;
        if (push_pull && wheel_steps) {
            g_pp_button = wheel_steps < 0 ? BTN_UP : BTN_DOWN;
            g_pp_frames = 6 * std::min(4, std::abs(wheel_steps));
            wheel_steps = 0;
        }
        // shield up + attack key = perfect guard (not when the game offers something on Y, nor 3 s after a dodge:
        // Flurry Rush needs the attack button)
        if (down(kTarget) && down(kAttack) && !cancel && !throw_it && !held && !after_dodge && !ui.has_button(BTN_Y))
            remap(kAttack, BTN_Y, BTN_A, "parry", false);
    }
    if (!push_pull) g_pp_frames = 0;
    if (g_pp_frames > 0) g_pp_frames--, add |= g_pp_button;

    for (int i = 0; i < kCount; i++) {
        Remap& m = g_remap[i];
        const bool d = sh.down[i] || sh.pressed[i];
        if (remapped[i]) {
            remove |= m.remove, add |= m.add;
        } else if (m.frames > 0 && !d) {
            m.frames--;  // the key is up already: the rest of a click's pulse is still the remapped button
            remove |= m.remove, add |= m.add;
        } else {
            m.frames = 0;
            if (!d) m.latch = 0;
            remove |= m.latch;
        }
    }
    if (g_log && names != g_ctx_log) LOG("pc controls: context %s", names.empty() ? "(none)" : names.c_str());
    g_ctx_log = names;
}

void apply_out(PadState& s, const Out& o) {
    s.buttons = (s.buttons & ~o.remove) | o.buttons;
    if (o.stick) {
        s.rx = o.rx;
        s.ry = 0;
    }
}

void apply_menu_out(PadState& s, const MenuOut& o) {
    s.buttons |= o.buttons;
    if (o.l) s.lx = o.lx, s.ly = o.ly;
    if (o.r) s.rx = o.rx, s.ry = o.ry;
}
}  // namespace

bool owns_mouse() { return g_owns_mouse; }
bool tilt_active() { return g_tilt_on; }

bool tilt_motion(input::Motion* out) {
    std::lock_guard<std::mutex> fl(g_frame_lock);
    if (!g_tilt_report) return false;
    *out = g_tilt_motion;
    return true;
}

void frame(PadState& s, bool advance) {
    if (!active()) return;
    std::lock_guard<std::mutex> fl(g_frame_lock);
    if (!advance) return apply_menu_out(s, g_menu_out), apply_out(s, g_out);

    static const int kBow = action_index("Bow");
    Shared sh;
    {
        std::lock_guard<std::mutex> l(g_share_lock);
        sh = g_sh;
        for (bool& p : g_sh.pressed) p = false;
        g_sh.wheel_steps = 0;
        g_sh.tilt_turn = 0;
        g_sh.mouse_pressed = 0;
        g_sh.menu_wheel = 0;
    }
    const uint64_t now = SDL_GetTicks();

    // ---- the game's menus (pointer, map, tabs, pause menu page keys) ----
    {
        static const int kInventory = action_index("Inventory"), kJournal = action_index("Journal"),
                         kSystem = action_index("System"),
                         kSaveMenu = action_index("SaveMenu"), kLoadMenu = action_index("LoadMenu");
        pcmenus::FrameIO io;
        const mouse::Pointer p = mouse::pointer();
        io.mouse_inside = sh.enabled && p.inside;
        io.mouse_x = p.x;
        io.mouse_y = p.y;
        const uint32_t held = sh.enabled ? mouse::buttons() | sh.mouse_pressed : 0;
        const bool lmb = (held & SDL_BUTTON_LMASK) != 0, rmb = (held & SDL_BUTTON_RMASK) != 0;
        io.lmb_down = lmb;
        io.lmb_pressed = (sh.enabled && (sh.mouse_pressed & SDL_BUTTON_LMASK)) || (lmb && !g_prev_lmb);
        io.lmb_released = !lmb && g_prev_lmb;
        io.rmb_pressed = (sh.enabled && (sh.mouse_pressed & SDL_BUTTON_RMASK)) || (rmb && !g_prev_rmb);
        io.mmb_down = (held & SDL_BUTTON_MMASK) != 0;
        g_prev_lmb = lmb;
        g_prev_rmb = rmb;
        io.wheel = sh.enabled ? sh.menu_wheel : 0;
        io.inventory_key = sh.enabled && kInventory >= 0 && sh.pressed[kInventory];
        io.journal_key = sh.enabled && kJournal >= 0 && sh.pressed[kJournal];
        io.system_key = sh.enabled && kSystem >= 0 && sh.pressed[kSystem];
        io.save_key = sh.enabled && kSaveMenu >= 0 && sh.pressed[kSaveMenu];
        io.load_key = sh.enabled && kLoadMenu >= 0 && sh.pressed[kLoadMenu];
        io.in_play = pce_gameplay_camera();
        pcmenus::update(io);
        mouse::set_pointer_mode(io.pointer_mode);
        g_menu_out = {io.hold_add, io.override_l, io.override_r, io.lx, io.ly, io.rx, io.ry};
        apply_menu_out(s, g_menu_out);
    }

    // ---- Motion Control ----
    static const int kTiltMode = action_index("TiltMode"), kTiltLevel = action_index("TiltLevel");
    {
        const bool was = g_tilt_on;
        bool on = was;
        if (g_tilt_toggle) {
            if (kTiltMode >= 0 && sh.pressed[kTiltMode] && sh.enabled) on = !on;
        } else {
            on = kTiltMode >= 0 && sh.down[kTiltMode] && sh.enabled;
        }
        if (on != was) {
            g_tilt_on = on;
            if (on) mouse::take();  // motion from before belongs to nothing
            LOG("pc controls: motion control %s", on ? "on (mouse tilts the GamePad)" : "off");
        }
        const uint64_t ns = SDL_GetTicksNS();
        const float dt = g_tilt_last_ns ? (float)((double)(ns - g_tilt_last_ns) / 1e9) : 1.0f / 30.0f;
        g_tilt_last_ns = ns;
        double mdx = 0, mdy = 0;
        if (on) {
            const mouse::Delta d = mouse::take();
            if (sh.enabled) mdx = d.dx, mdy = d.dy;
        }
        const bool level = on && kTiltLevel >= 0 && sh.pressed[kTiltLevel];
        g_tilt_report = pctilt::step(g_tilt, on, mdx, mdy, on ? sh.tilt_turn : 0, level, dt, &g_tilt_motion);
    }
    const bool tilt = g_tilt_on;
    const bool gameplay = pce_gameplay_camera();
    if (g_log && gameplay != g_was_gameplay) LOG("pc controls: gameplay camera %s", gameplay ? "active" : "inactive");
    g_was_gameplay = gameplay;
    static bool was_map = false;
    const bool map = pce_map_screen();
    if (g_log && map != was_map) LOG("pc controls: world map %s", map ? "shown" : "gone");
    was_map = map;
    const bool enabled = sh.enabled;
    const bool allowed = enabled && gameplay && !tilt;  // no selectors while the mouse tilts the GamePad
    // aiming: the aim button held (mouse, key or controller) and a bow drawn (its camera is on)
    const bool aiming = (s.buttons & BTN_ZR) != 0 && pce_bow_camera();

    // ---- context controls (needs the keys, not a menu with the pointer, not Motion Control) ----
    uint32_t ctx_remove = 0, ctx_add = 0;
    int wheel_steps = sh.wheel_steps;
    if (enabled && !tilt && !mouse::pointer_mode()) context(sh, wheel_steps, ctx_remove, ctx_add, now);
    s.buttons = (s.buttons & ~ctx_remove) | ctx_add;

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
    int steps = wheel_steps * (g_selector_invert_scroll ? -1 : 1);
    if (steps && (allowed || (own && enabled))) {
        if (!sel.active) open(action_index(steps > 0 ? "WeaponNext" : "WeaponPrev"), false);
        sel.pending += steps;
        sel.last_activity_ms = now;
    }

    Out o;
    o.remove = ctx_remove;
    o.buttons = ctx_add;
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

// ---- bindings for the overlay menu's Keys tab (host main thread) ----
int action_count() { return kCount; }
const char* action_name(int i) { return i >= 0 && i < kCount ? kActions[i].name : ""; }

std::string binding_text(int i, int slot) {
    ensure();
    if (i < 0 || i >= kCount || slot < 0 || slot > 1) return "";
    return key_name(g_keys[i][slot]);
}

namespace {
void save() {
    if (g_path.empty()) return;
    if (write_file(botw::path_from_utf8(g_path))) LOG("pc controls: bindings saved to %s", g_path.c_str());
    else LOG("pc controls: could not write %s", g_path.c_str());
}
}  // namespace

void set_binding(int i, int slot, int type, int code) {
    ensure();
    if (i < 0 || i >= kCount || slot < 0 || slot > 1) return;
    Key k;
    switch (type) {
        case 1: k = {Key::Scan, code}; break;
        case 2: k = {Key::Mouse, code}; break;
        case 3: k = {Key::WheelUp, 0}; break;
        case 4: k = {Key::WheelDown, 0}; break;
        default: break;
    }
    g_keys[i][slot] = k;
    LOG("pc controls: %s %s = %s", kActions[i].id, slot ? "(second key)" : "", key_name(k).empty() ? "(none)" : key_name(k).c_str());
    save();
}

bool hold_toggle(int which) {
    ensure();
    return which == 0 ? g_tilt_toggle : which == 1 ? g_walk_toggle : g_sprint_toggle;
}

void set_hold_toggle(int which, bool toggle) {
    ensure();
    (which == 0 ? g_tilt_toggle : which == 1 ? g_walk_toggle : g_sprint_toggle) = toggle;
    g_walk_latched = g_sprint_latched = false;
    LOG("pc controls: %s key = %s", which == 0 ? "Motion Control" : which == 1 ? "Walk" : "Sprint", toggle ? "toggle" : "hold");
    save();
}

void reset_bindings() {
    ensure();
    set_defaults();
    LOG("pc controls: bindings reset to the defaults");
    save();
}

std::string key_label(const char* action_id) {
    ensure();
    const int i = action_index(action_id);
    if (i < 0) return "";
    for (const Key& k : g_keys[i]) {
        switch (k.type) {
            case Key::Mouse:
                switch (k.code) {
                    case SDL_BUTTON_LEFT: return "LMB";
                    case SDL_BUTTON_RIGHT: return "RMB";
                    case SDL_BUTTON_MIDDLE: return "MMB";
                    case SDL_BUTTON_X1: return "M4";
                    default: return "M5";
                }
            case Key::WheelUp: return "Wheel Up";
            case Key::WheelDown: return "Wheel Dn";
            case Key::Scan: {
                std::string n = key_name(k);
                for (const char* side : {"Left ", "Right "})
                    if (n.rfind(side, 0) == 0) n.erase(0, strlen(side));
                if (n == "Return") n = "Enter";
                if (n == "Escape") n = "Esc";
                if (n == "Backspace") n = "Bksp";
                return n;
            }
            default: break;
        }
    }
    return "";
}

void log_bindings() {
    if (!active()) return;
    std::string line;
    int shown = 0;
    for (int i = 0; i < kCount; i++) {
        std::string a = key_name(g_keys[i][0]), b = key_name(g_keys[i][1]);
        if (a.empty() && b.empty()) continue;
        line += std::string(kActions[i].id) + "=" + (a.empty() ? b : b.empty() ? a : a + "/" + b) +
"  ";
        if (++shown % 8 == 0) {
            LOG("pc controls: %s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) LOG("pc controls: %s", line.c_str());
}
}  // namespace pccontrols
