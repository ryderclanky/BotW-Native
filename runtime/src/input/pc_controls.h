// PC keyboard/mouse actions (port of Zelda-on-PC's action bindings, src/pcedition/PCSettings.cpp, MPL-2.0,
// https://github.com/mircea32000/Zelda-on-PC). Bindings live in pc_controls.ini in the user folder (written
// with the defaults on first run). While enabled, they replace the keyboard half of the built-in map and of
// controls.ini; controllers are not affected.
#pragma once
#include <SDL3/SDL.h>

#include <stdint.h>

#include <string>

namespace input {
struct PadState;
struct Motion;
}

namespace pccontrols {
// Reads (or creates) pc_controls.ini. Call once the user folder is known.
void load();
bool active();
// Level state of every bound key and mouse button, plus tap / click / wheel pulses, into s.
void apply(SDL_Window* win, input::PadState& s);
// Host event loop: a key went down (not a repeat). Keeps a short tap visible to VPADRead.
void key_down(SDL_Scancode sc);
// The key opens the overlay menu (Settings action).
bool is_settings_key(SDL_Scancode sc);
// Drop pending pulses (menu opened, keyboard shown).
void clear_pulses();
// VPADRead: weapon / bow / shield / rune selectors (hold 1-4 and move the mouse, or the wheel). They hold the
// d-pad and flick the right stick once per step, so they run per game read; `advance` only on the game's own
// reads (not the sampling thread), which see the latest output.
void frame(input::PadState& s, bool advance);
// A selector is open: mouse X steps it instead of turning the camera.
bool owns_mouse();
// Motion Control (TiltMode) is on: the mouse tilts the virtual GamePad, the cameras leave the mouse alone.
bool tilt_active();
// The virtual GamePad's motion while it is reported (Motion Control on, or levelling after); else false.
bool tilt_motion(input::Motion* out);
void log_bindings();

// A binding (for the key prompts): type 0 none, 1 SDL scancode, 2 SDL mouse button, 3 wheel up, 4 wheel down.
struct BoundKey {
    int type = 0;
    int code = 0;
};
BoundKey bound_key(const char* action_id, int slot);  // slot 0 or 1

// Keys tab of the overlay menu: the actions in pc_controls.ini order, their keys, and changes (saved at once).
int action_count();
const char* action_name(int i);
std::string binding_text(int i, int slot);  // SDL key name / MouseLeft / WheelUp ..., "" = unbound
void set_binding(int i, int slot, int type, int code);  // type as BoundKey (0 = clear)
void reset_bindings();
// Short label of an action's first bound key ("E", "Space", "LMB"), for the HUD keys; "" if unbound.
std::string key_label(const char* action_id);
}  // namespace pccontrols
