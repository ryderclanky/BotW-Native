// Raw mouse for the PC controls (port of Zelda-on-PC's mouse input, see docs/pc_hooks.md).
// The host event loop (window.cpp) feeds SDL events and decides when the mouse may be captured; game-side
// hooks take the accumulated motion once per frame.
#pragma once
#include <stdint.h>

union SDL_Event;
struct SDL_Window;

namespace input::mouse {
// Host event loop: every mouse event (motion, buttons, wheel). Motion counts only while captured.
void event(const SDL_Event& e);
// Host event loop, once per iteration: capture (relative mode, cursor hidden) while `allowed` and the window
// has input focus; release otherwise.
void update(SDL_Window* win, bool allowed);
bool captured();

struct Delta {
    double dx = 0, dy = 0;  // raw counts, +x right, +y down (as the OS reports them)
};
// Motion since the last take (zero while not captured). Thread-safe; the camera hook is the consumer.
Delta take();
// Host side (key bindings): wheel notches since the last call (+ away from the user), and the buttons
// pressed since the last call (so a click shorter than a poll is not lost).
float take_wheel();
uint32_t take_pressed();
// SDL_BUTTON_*MASK bits currently held (0 while not captured). Thread-safe.
uint32_t buttons();

// Main camera: the rotation itself is injected into the camera code, but the game only stays in manual
// control while the right stick is deflected. The camera hook reports the mouse direction (game stick axes,
// +y up); for a short time after, the right stick carries a deflection just past the game's deadzone.
void set_manual_dir(float x, float y);
// Host apply_input: replace a resting right stick with that deflection while it is held.
void manual_stick(float& rx, float& ry);
}  // namespace input::mouse
