#include "mouse.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>

#include "../log.h"
#include "../settings.h"

namespace input::mouse {
namespace {
std::mutex g_lock;
Delta g_acc;                        // since the last take()
float g_wheel = 0;                  // since the last take_wheel()
uint32_t g_pressed = 0;             // since the last take_pressed()
std::atomic<bool> g_captured{false};
std::atomic<uint32_t> g_buttons{0};
std::atomic<bool> g_pointer_req{false};  // a menu wants the pointer
std::atomic<bool> g_pointer{false};      // pointer mode active (not captured, buttons / wheel / position tracked)
std::atomic<float> g_px{0}, g_py{0};
std::atomic<bool> g_pinside{false};

// window coordinates -> normalized picture coordinates (16:9 TV image placed by the display setting)
void update_pointer(SDL_Window* win) {
    static int display = 0;
    static uint64_t display_ms = 0;
    const uint64_t now = SDL_GetTicks();
    if (!display_ms || now - display_ms > 500) display = settings::get().display, display_ms = now;
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    if (w <= 0 || h <= 0) return g_pinside.store(false);
    const float aspect = 16.0f / 9.0f, wa = (float)w / (float)h;
    float iw = (float)w, ih = (float)h;
    if (display == 0) {  // fit (letterbox)
        if (wa > aspect) iw = h * aspect;
        else ih = w / aspect;
    } else if (display == 2) {  // fill (crop)
        if (wa > aspect) ih = w / aspect;
        else iw = h * aspect;
    }
    const float nx = (x - (w - iw) * 0.5f) / iw, ny = (y - (h - ih) * 0.5f) / ih;
    g_px = nx;
    g_py = ny;
    g_pinside = nx >= 0 && nx <= 1 && ny >= 0 && ny <= 1 && SDL_GetMouseFocus() == win;
}

// BOTW_PCE_MOUSE_LOG=1: once a second while the mouse is used, log what arrived (phase 2 test).
const bool g_log = [] {
    const char* s = std::getenv("BOTW_PCE_MOUSE_LOG");
    return s && *s && *s != '0';
}();
struct {
    double dx = 0, dy = 0;
    float wheel = 0;
    int events = 0;
    uint32_t pressed = 0;  // buttons pressed during the second
    uint64_t start_ms = 0;
} g_stat;

void release(SDL_Window* win, const char* why) {
    if (win) SDL_SetWindowRelativeMouseMode(win, false);
    g_captured = false;
    g_buttons = 0;
    std::lock_guard<std::mutex> l(g_lock);
    g_acc = {};
    g_wheel = 0;
    g_pressed = 0;
    LOG("mouse: released (%s)", why);
}
}  // namespace

void event(const SDL_Event& e) {
    if (!g_captured && !g_pointer) return;
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION: {
            if (e.motion.which == SDL_TOUCH_MOUSEID) return;  // touch emulation, not a mouse
            if (!g_captured) return;  // pointer mode: the position is read in update()
            std::lock_guard<std::mutex> l(g_lock);
            g_acc.dx += e.motion.xrel;
            g_acc.dy += e.motion.yrel;
            g_stat.dx += e.motion.xrel;
            g_stat.dy += e.motion.yrel;
            g_stat.events++;
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            if (e.wheel.which == SDL_TOUCH_MOUSEID) return;
            float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            std::lock_guard<std::mutex> l(g_lock);
            g_wheel += y;
            g_stat.wheel += y;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (e.button.which == SDL_TOUCH_MOUSEID) return;
            const uint32_t bit = SDL_BUTTON_MASK(e.button.button);
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                g_buttons |= bit;
                std::lock_guard<std::mutex> l(g_lock);
                g_pressed |= bit;
                g_stat.pressed |= bit;
            } else {
                g_buttons &= ~bit;
            }
            break;
        }
        default: break;
    }
}

void update(SDL_Window* win, bool allowed) {
#if defined(SDL_PLATFORM_IOS)
    (void)win, (void)allowed;
    return;  // touch device: no mouse capture
#else
    if (!win) return;
    const bool focused = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool want_pointer = allowed && focused && g_pointer_req.load();
    const bool want = allowed && focused && !want_pointer;
    if (want_pointer && g_captured) {
        // a menu opened: show the cursor in the middle of the picture
        SDL_SetWindowRelativeMouseMode(win, false);
        g_captured = false;
        int w = 0, h = 0;
        SDL_GetWindowSize(win, &w, &h);
        SDL_WarpMouseInWindow(win, w * 0.5f, h * 0.5f);
        std::lock_guard<std::mutex> l(g_lock);
        g_acc = {};
        LOG("mouse: pointer (menu)");
    }
    if (want_pointer != g_pointer.load()) {
        g_pointer = want_pointer;
        g_buttons = 0;  // nothing held from before counts as a click
        if (!want_pointer) {
            std::lock_guard<std::mutex> l(g_lock);
            g_wheel = 0;
            g_pressed = 0;
        }
    }
    if (g_pointer) update_pointer(win);
    else g_pinside = false;
    if (want && !g_captured) {
        if (!SDL_SetWindowRelativeMouseMode(win, true)) {
            static bool warned = false;
            if (!warned) warned = true, LOG("mouse: relative mode failed: %s", SDL_GetError());
            return;
        }
        {
            std::lock_guard<std::mutex> l(g_lock);
            g_acc = {};  // nothing from before the capture
        }
        g_buttons = 0;  // a click that focused the window does not count until pressed again
        g_captured = true;
        LOG("mouse: captured (raw relative motion)");
    } else if (!want && g_captured) {
        release(win, !focused ? "window lost focus" : "menu or keyboard open");
    }
    // (when a menu takes the pointer while not captured, e.g. after a focus change, nothing else is needed)

    if (!g_log) return;
    const uint64_t now = SDL_GetTicks();
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_stat.start_ms) g_stat.start_ms = now;
    if (now - g_stat.start_ms < 1000) return;
    if (g_stat.events || g_stat.wheel != 0 || g_stat.pressed)
        LOG("mouse: 1s  dx %+.0f dy %+.0f counts, %d motion events, wheel %+.1f, buttons pressed 0x%x, held 0x%x",
            g_stat.dx, g_stat.dy, g_stat.events, g_stat.wheel, g_stat.pressed, g_buttons.load());
    g_stat = {};
    g_stat.start_ms = now;
#endif
}

bool captured() { return g_captured; }

void set_pointer_mode(bool on) { g_pointer_req = on; }
bool pointer_mode() { return g_pointer; }
Pointer pointer() { return {g_px.load(), g_py.load(), g_pointer.load() && g_pinside.load()}; }

Delta take() {
    std::lock_guard<std::mutex> l(g_lock);
    Delta d = g_acc;
    g_acc = {};
    return d;
}

uint32_t buttons() { return g_buttons; }

float take_wheel() {
    std::lock_guard<std::mutex> l(g_lock);
    float w = g_wheel;
    g_wheel = 0;
    return w;
}

uint32_t take_pressed() {
    std::lock_guard<std::mutex> l(g_lock);
    uint32_t p = g_pressed;
    g_pressed = 0;
    return p;
}

namespace {
std::mutex g_manual_lock;
float g_manual_x = 0, g_manual_y = 0;
uint64_t g_manual_until_ms = 0;
constexpr uint64_t kManualHoldMs = 120;
constexpr float kManualMagnitude = 0.0062f;  // just above the game's 0.005 stick deadzone (no rotation of its own)
}  // namespace

void set_manual_dir(float x, float y) {
    const float len = std::sqrt(x * x + y * y);
    if (len <= 0) return;
    std::lock_guard<std::mutex> l(g_manual_lock);
    g_manual_x = x / len;
    g_manual_y = y / len;
    g_manual_until_ms = SDL_GetTicks() + kManualHoldMs;
}

void manual_stick(float& rx, float& ry) {
    if (std::fabs(rx) > 0.001f || std::fabs(ry) > 0.001f) return;  // the stick or keys are turning the camera
    std::lock_guard<std::mutex> l(g_manual_lock);
    if (!g_captured || SDL_GetTicks() >= g_manual_until_ms) return;
    rx = g_manual_x * kManualMagnitude;
    ry = g_manual_y * kManualMagnitude;
}
}  // namespace input::mouse
