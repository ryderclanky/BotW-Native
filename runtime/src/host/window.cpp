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

#include "../input/input.h"
#include "../log.h"
#include "../os.h"
#include "../latte/latte.h"

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
SDL_Gamepad* g_pad = nullptr;
bool g_use_gyro = true;  // --no-gyro / settings
bool g_use_rumble = true;
bool g_background = false;  // iOS app lifecycle
int g_layout = 0;  // 0: A = right face button (Nintendo positions), 1: A = bottom face button (labels)
std::atomic<int> g_draw_w{1280}, g_draw_h{720};

// keyboard -> GamePad
struct KeyMap {
    SDL_Scancode sc;
    uint32_t button;
};
const KeyMap kKeys[] = {
    {SDL_SCANCODE_E, input::BTN_A},      {SDL_SCANCODE_LSHIFT, input::BTN_B}, {SDL_SCANCODE_SPACE, input::BTN_X},
    {SDL_SCANCODE_F, input::BTN_Y},      {SDL_SCANCODE_Q, input::BTN_ZL},     {SDL_SCANCODE_R, input::BTN_ZR},
    {SDL_SCANCODE_1, input::BTN_L},      {SDL_SCANCODE_3, input::BTN_R},      {SDL_SCANCODE_RETURN, input::BTN_PLUS},
    {SDL_SCANCODE_TAB, input::BTN_MINUS}, {SDL_SCANCODE_UP, input::BTN_UP},    {SDL_SCANCODE_DOWN, input::BTN_DOWN},
    {SDL_SCANCODE_LEFT, input::BTN_LEFT}, {SDL_SCANCODE_RIGHT, input::BTN_RIGHT}, {SDL_SCANCODE_C, input::BTN_STICK_L},
    {SDL_SCANCODE_V, input::BTN_STICK_R},
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
    };
    for (auto& [k, v] : names)
        if (n == k) return v;
    return 0;
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
    if (g_win && (SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS)) {
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
    if (g_pad) {
        auto ax = [](Sint16 v) { float f = v / 32767.0f; return fabsf(f) < 0.15f ? 0.0f : f; };
        s.lx += ax(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX));
        s.ly -= ax(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY));
        s.rx += ax(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX));
        s.ry -= ax(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY));
        // layout 0, Nintendo positions: SDL "east" = Wii U A, "south" = B, "north" = X, "west" = Y.
        // layout 1, by label (Xbox / PlayStation habit): south = A, east = B, west = X, north = Y.
        const bool labels = g_layout == 1;
        const std::pair<SDL_GamepadButton, uint32_t> btns[] = {
            {labels ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST, input::BTN_A},
            {labels ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH, input::BTN_B},
            {labels ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH, input::BTN_X},
            {labels ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST, input::BTN_Y},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, input::BTN_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, input::BTN_R},
            {SDL_GAMEPAD_BUTTON_START, input::BTN_PLUS},    {SDL_GAMEPAD_BUTTON_BACK, input::BTN_MINUS},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, input::BTN_UP},    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, input::BTN_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, input::BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, input::BTN_RIGHT},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, input::BTN_STICK_L}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, input::BTN_STICK_R},
        };
        for (auto& [b, v] : btns)
            if (SDL_GetGamepadButton(g_pad, b)) s.buttons |= v;
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) s.buttons |= input::BTN_ZL;
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) s.buttons |= input::BTN_ZR;
    }
    touch::apply(s);  // on-screen controls (s is cleared below while the menu is open)
    if (menu::is_open()) {  // the menu owns the controls; the game sees nothing pressed
        s = input::PadState{};
        if (g_pad) {  // left stick navigates the menu
            static int last_y = 0, last_x = 0;
            float y = -SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            float x = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
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
    if (g_pad) {  // live switch
        SDL_SetGamepadSensorEnabled(g_pad, SDL_SENSOR_GYRO, on);
        SDL_SetGamepadSensorEnabled(g_pad, SDL_SENSOR_ACCEL, on);
    }
    input::motion_reset();
}
void set_rumble(bool on) {
    g_use_rumble = on;
    if (!on && g_pad) SDL_RumbleGamepad(g_pad, 0, 0, 0);
}
void set_layout(int layout) { g_layout = layout; }

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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) LOG("SDL_Init failed: %s", SDL_GetError());
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    flags |= latte::using_vulkan() ? SDL_WINDOW_VULKAN : SDL_WINDOW_METAL;
    if (g_headless) flags |= SDL_WINDOW_HIDDEN;  // tests: render into a hidden window, never on screen
#if defined(_WIN32)
    const char* title = "BotW-Native (Windows)";
#else
    const char* title = "BotW-Native";
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
    LOG("window \"%s\" %dx%d (%dx%d pixels)%s, renderer %s", title, pw, ph, w, h, g_headless ? ", hidden" : "",
        latte::using_vulkan() ? "vulkan" : "metal");
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
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED && !g_pad) {
                g_pad = SDL_OpenGamepad(e.gdevice.which);
                if (g_pad) {
                    bool motion = g_use_gyro && SDL_GamepadHasSensor(g_pad, SDL_SENSOR_GYRO) &&
                                  SDL_GamepadHasSensor(g_pad, SDL_SENSOR_ACCEL) &&
                                  SDL_SetGamepadSensorEnabled(g_pad, SDL_SENSOR_GYRO, true) &&
                                  SDL_SetGamepadSensorEnabled(g_pad, SDL_SENSOR_ACCEL, true);
                    input::motion_reset();
                    touch::set_controller_connected(true);
                    LOG("gamepad: %s%s", SDL_GetGamepadName(g_pad), motion ? " (gyro on)" : "");
                }
            }
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && g_pad && e.gdevice.which == SDL_GetGamepadID(g_pad)) {
                SDL_CloseGamepad(g_pad);
                g_pad = nullptr;
                input::motion_reset();
                touch::set_controller_connected(false);
            } else if (e.type == SDL_EVENT_GAMEPAD_SENSOR_UPDATE && g_pad && e.gsensor.which == SDL_GetGamepadID(g_pad)) {
                if (e.gsensor.sensor == SDL_SENSOR_GYRO || e.gsensor.sensor == SDL_SENSOR_ACCEL)
                    input::motion_sensor(e.gsensor.sensor == SDL_SENSOR_GYRO, e.gsensor.data, e.gsensor.timestamp);
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && g_pad && e.gbutton.which == SDL_GetGamepadID(g_pad)) {
                auto b = (SDL_GamepadButton)e.gbutton.button;
                bool both_sticks = (b == SDL_GAMEPAD_BUTTON_LEFT_STICK && SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) ||
                                   (b == SDL_GAMEPAD_BUTTON_RIGHT_STICK && SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_LEFT_STICK));
                SDL_GamepadButton confirm = g_layout == 1 ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
                SDL_GamepadButton back = g_layout == 1 ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
                if (b == SDL_GAMEPAD_BUTTON_TOUCHPAD || both_sticks) menu::toggle();
                else if (menu::is_open()) {
                    if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) menu::move(-1);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) menu::move(+1);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) menu::change(-1);
                    else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) menu::change(+1);
                    else if (b == confirm) menu::activate();
                    else if (b == back || b == SDL_GAMEPAD_BUTTON_START) menu::close();
                }
            } else if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_F1 || (menu::is_open() && !e.key.repeat))) {
                switch (e.key.key) {
                    case SDLK_F1: menu::toggle(); break;
                    case SDLK_ESCAPE: menu::close(); break;
                    case SDLK_UP: menu::move(-1); break;
                    case SDLK_DOWN: menu::move(+1); break;
                    case SDLK_LEFT: menu::change(-1); break;
                    case SDLK_RIGHT: menu::change(+1); break;
                    case SDLK_RETURN: menu::activate(); break;
                    default: break;
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
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11 && g_win) {
                bool fs = SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN;
                SDL_SetWindowFullscreen(g_win, !fs);
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
                if (g_pad) SDL_RumbleGamepad(g_pad, on && g_use_rumble ? 0x6000 : 0, on && g_use_rumble ? 0x6000 : 0,
                                             on && g_use_rumble ? 1000 : 0);
            }
        }
        if (g_run_seconds && SDL_GetTicks() - t0 > (Uint64)g_run_seconds * 1000) {
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
