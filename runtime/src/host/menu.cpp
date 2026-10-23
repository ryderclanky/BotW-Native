#include "menu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "../settings.h"

extern float g_render_scale;  // main.cpp: the scale this run renders at

namespace menu {
namespace {
std::mutex g_lock;
std::atomic<bool> g_open{false};
int g_cursor = 0;  // selected row inside the current tab (the last row of every tab is Close)
int g_tab = 0;

enum RowId { kFps, kScale, kDisplay, kFilter, kGyro, kRumble, kLayout, kTouch, kShowFps, kVolume, kClose, kRows };
const float kScales[] = {0, 0.5f, 0.75f, 1, 1.5f, 2, 2.5f, 3};  // 0 = fit the display

// Tabs. Every setting of the previous flat list is in exactly one tab; the order inside a tab follows
// the order of the old list. Close is appended to every tab.
enum TabId { kTabGraphics, kTabControls, kTabAudio, kTabAbout, kTabs };
const char* kTabNames[kTabs] = {"Graphics", "Controls", "Audio", "About"};
const std::vector<int>& tab_rows(int t) {
    static const std::vector<int> graphics = {kFps, kScale, kDisplay, kFilter, kShowFps};
    static const std::vector<int> controls = {kGyro, kRumble, kLayout, kTouch};
    static const std::vector<int> audio = {kVolume};
    static const std::vector<int> about;
    switch (t) {
        case kTabGraphics: return graphics;
        case kTabControls: return controls;
        case kTabAudio: return audio;
        default: return about;
    }
}
// Row id for a selected position in a tab; the position after the last setting is Close.
int row_id(int t, int cursor) {
    const std::vector<int>& r = tab_rows(t);
    return cursor >= 0 && cursor < (int)r.size() ? r[cursor] : (int)kClose;
}
int row_count(int t) { return (int)tab_rows(t).size() + 1; }

const char* kDisplays[] = {"Fit 16:9", "Stretch", "Fill (crop)"};
const char* kFilters[] = {"Bilinear", "Bicubic", "Hermite", "Nearest"};

std::string scale_text(float s) {
    char b[64];
    if (s <= 0) return "Auto (fit the display)";
    snprintf(b, sizeof b, "%gx  (%dx%d)", s, (int)lroundf(1280 * s), (int)lroundf(720 * s));
    return b;
}

void step(Settings& s, int row, int dir) {
    auto wrap = [](int v, int n) { return (v % n + n) % n; };
    switch (row) {
        case kFps: s.fps = s.fps >= 60 ? 30 : 60; break;
        case kScale: {
            int i = 0;
            for (int k = 0; k < (int)std::size(kScales); k++)
                if (fabsf(kScales[k] - s.scale) < 0.01f) i = k;
            s.scale = kScales[wrap(i + dir, (int)std::size(kScales))];
            break;
        }
        case kDisplay: s.display = wrap(s.display + dir, 3); break;
        case kFilter: s.filter = wrap(s.filter + dir, 4); break;
        case kGyro: s.gyro = !s.gyro; break;
        case kRumble: s.rumble = !s.rumble; break;
        case kLayout: {
            // -1 auto, 0 Nintendo positions, 1 Xbox/PlayStation labels.
            if (s.layout < 0) s.layout = dir > 0 ? 0 : 1;
            else if (s.layout == 0) s.layout = dir > 0 ? 1 : -1;
            else s.layout = dir > 0 ? -1 : 0;
            break;
        }
        case kTouch: s.touch = wrap(s.touch + dir, 3), s.touch_hidden = false; break;
        case kShowFps: s.show_fps = !s.show_fps; break;
        case kVolume: s.volume = std::clamp(s.volume + dir * 10, 0, 100); break;
    }
}
}  // namespace

std::atomic<float> g_px{-1}, g_py{-1};
std::atomic<bool> g_pdown{false};
void pointer(float x, float y, bool down) {
    g_px = x;
    g_py = y;
    g_pdown = down;
}
void pointer_state(float* x, float* y, bool* down) {
    *x = g_px;
    *y = g_py;
    *down = g_pdown;
}

std::atomic<float> g_safe[4] = {0, 0, 0, 0};
void set_safe_area(float l, float t, float r, float b) {
    g_safe[0] = l, g_safe[1] = t, g_safe[2] = r, g_safe[3] = b;
}
void safe_area(float* l, float* t, float* r, float* b) {
    *l = g_safe[0], *t = g_safe[1], *r = g_safe[2], *b = g_safe[3];
}

std::atomic<float> g_panel[4] = {0, 0, 0, 0}, g_xbtn[4] = {0, 0, 0, 0};
void set_rects(const float p[4], const float x[4]) {
    for (int i = 0; i < 4; i++) g_panel[i] = p[i], g_xbtn[i] = x[i];
}
void rects(float p[4], float x[4]) {
    for (int i = 0; i < 4; i++) p[i] = g_panel[i], x[i] = g_xbtn[i];
}
bool touch_close(float x, float y) {
    const float slop = 0.04f;  // the X is small for a thumb
    bool outside = x < g_panel[0] || y < g_panel[1] || x > g_panel[2] || y > g_panel[3];
    bool on_x = x > g_xbtn[0] - slop && y > g_xbtn[1] - slop && x < g_xbtn[2] + slop && y < g_xbtn[3] + slop;
    if (!g_open || !(outside || on_x)) return false;
    close();
    return true;
}

bool is_open() { return g_open; }
void toggle() { g_open = !g_open; }
void close() { g_open = false; }

void move(int dy) {
    std::lock_guard<std::mutex> l(g_lock);
    const int n = row_count(g_tab);
    g_cursor = ((g_cursor + dy) % n + n) % n;
}

void select_tab(int i) {
    std::lock_guard<std::mutex> l(g_lock);
    if (i < 0 || i >= kTabs) return;
    if (i != g_tab) g_cursor = 0;
    g_tab = i;
}

void tab(int dir) {
    int t;
    {
        std::lock_guard<std::mutex> l(g_lock);
        t = g_tab;
    }
    select_tab(((t + dir) % kTabs + kTabs) % kTabs);
}

void change(int dir) {
    int row;
    {
        std::lock_guard<std::mutex> l(g_lock);
        row = row_id(g_tab, g_cursor);
    }
    if (row == kClose) return;
    Settings s = settings::get();
    step(s, row, dir);
    settings::set(s);
}

void activate() {
    int row;
    {
        std::lock_guard<std::mutex> l(g_lock);
        row = row_id(g_tab, g_cursor);
    }
    if (row == kClose) close();
    else change(+1);
}

void click(int row, int dir) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (row < 0 || row >= row_count(g_tab)) return;
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_cursor = row;
    }
    if (dir == 0) activate();
    else change(dir);
}

View view() {
    View v;
    v.open = g_open;
    int tab_now, cursor;
    {
        std::lock_guard<std::mutex> l(g_lock);
        tab_now = g_tab;
        cursor = g_cursor;
    }
    v.tab = tab_now;
    v.cursor = cursor;
    for (const char* n : kTabNames) v.tabs.push_back(n);
    Settings s = settings::get();
    char b[96];
    Row all[kRows];
    all[kFps] = {"Frame rate", s.fps >= 60 ? "60 fps" : "30 fps (original)"};
    all[kScale] = {"Resolution", scale_text(s.scale)};
    all[kDisplay] = {"Aspect", kDisplays[std::clamp(s.display, 0, 2)]};
    all[kFilter] = {"Upscale filter", kFilters[std::clamp(s.filter, 0, 3)]};
    all[kGyro] = {"Gyro aiming", s.gyro ? "On" : "Off"};
    all[kRumble] = {"Rumble", s.rumble ? "On" : "Off"};
    all[kLayout] = {"Buttons", s.layout < 0 ? "Auto (per controller)"
                                     : s.layout > 0 ? "A = bottom (Cross / Xbox A)"
                                                    : "A = right (Circle / Xbox B)"};
    all[kTouch] = {"Touch controls", s.touch == 1 ? "On" : s.touch == 2 ? "Off" : "Auto (no controller)"};
    all[kShowFps] = {"FPS counter", s.show_fps ? "On" : "Off"};
    snprintf(b, sizeof b, "%d%%", s.volume);
    all[kVolume] = {"Volume", b};
    all[kClose] = {"Close", "", true};
    for (int id : tab_rows(tab_now)) v.rows.push_back(all[id]);
    v.rows.push_back(all[kClose]);
    if (tab_now == kTabGraphics) {
        snprintf(b, sizeof b, "Rendering at %gx now; resolution changes apply after a restart.", g_render_scale);
        v.note = b;
    } else if (tab_now == kTabControls) {
        v.note = "Controls changes apply at once and are saved to settings.ini.";
    } else if (tab_now == kTabAbout) {
        v.text = {
            "BotW-Native",
            "Breath of the Wild (Wii U, v1.5.0) statically recompiled to run natively, with a Vulkan backend on Windows.",
            "No game assets are included. You need your own copy of the game.",
            "",
            "Based on BotW Recompiled by Ishaan Kalra: github.com/IshaanKalra2103/BotWRecomp",
            "GPU layer: Cemu (MPL-2.0), with its graphic pack rules and the FPS++ approach for 60 fps.",
            "Libraries: SDL3, fmt, zstd, glslang, Dear ImGui and metal-cpp.",
            "Own code is MIT licensed. Full credits are in the README.",
        };
    }
    return v;
}
}  // namespace menu
