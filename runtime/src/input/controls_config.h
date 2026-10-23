// Applies userdata/controls.ini. window.cpp keeps the built-in keyboard map and the
// auto-layout controller path, and calls these when the file is present.
#pragma once

#include <SDL3/SDL.h>

#include <stdint.h>

namespace input {
struct PadState;
}

namespace controls {
// True once controls.ini has been read and has usable bindings.
bool loaded();
// Keyboard half of the file (level state). Tap pulses stay in window.cpp and use
// button_for_scancode so a short press is still visible to VPADRead.
bool apply_keyboard(SDL_Window* win, input::PadState& s);
// Controller half. face(pad, 0..3) is Wii U A/B/X/Y from the auto layout. A binding
// of "auto" uses that. east/south/north/west and every other token replace it.
bool apply_pad(SDL_Gamepad* pad, input::PadState& s, SDL_GamepadButton (*face)(SDL_Gamepad* pad, int which));
// Button bit for a remapped key, or 0 when the file is absent or the key is not a button.
uint32_t button_for_scancode(SDL_Scancode sc);
// SDL button for this action when the file names one. -1 for auto, unbound, or no file.
int mapped_button(const char* action_id);
// Logs the file once and returns true when it replaced the built-in control log.
bool log_if_loaded();
}  // namespace controls
