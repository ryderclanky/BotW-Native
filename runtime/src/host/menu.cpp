#include "menu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "../coreinit/mods.h"
#include "../settings.h"

extern float g_render_scale;  // main.cpp: the scale this run renders at

namespace menu {
namespace {
std::mutex g_lock;
std::atomic<bool> g_open{false};
int g_cursor = 0;  // selected row inside the current tab (the last row of every tab is Close)
int g_tab = 0;

enum RowId {
    kFps,
    kScale,
    kDisplay,
    kFilter,
    kGamepadView,
    kGyro,
    kRumble,
    kLayout,
    kTouch,
    kShowFps,
    kWindowMode,
    kMods,
    kVolume,
    kModReload,
    kClose,
    kRows
};
const float kScales[] = {0, 0.5f, 0.75f, 1, 1.5f, 2, 2.5f, 3};  // 0 = fit the display

// Tabs. Every setting of the previous flat list is in exactly one tab; the order inside a tab follows
// the order of the old list. Close is appended to every tab.
enum TabId { kTabGraphics, kTabControls, kTabAudio, kTabMods, kTabAbout, kTabs };
const char* kTabNames[kTabs] = {"Graphics", "Controls", "Audio", "Mods", "About"};
constexpr int kModRowBase = 1000;
const std::vector<int>& tab_rows(int t) {
    static const std::vector<int> graphics = {kFps, kWindowMode, kScale, kDisplay, kFilter, kGamepadView, kShowFps, kMods};
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
    if (t == kTabMods) {
        auto list = mods::status();
        if (list.empty()) return cursor == 0 ? (int)kMods : (int)kClose;
        int n = (int)list.size();
        if (cursor >= 0 && cursor < n) return kModRowBase + cursor;
        if (cursor == n) return (int)kModReload;
        return (int)kClose;
    }
    const std::vector<int>& r = tab_rows(t);
    return cursor >= 0 && cursor < (int)r.size() ? r[cursor] : (int)kClose;
}
int row_count(int t) {
    if (t == kTabMods) {
        auto list = mods::status();
        return list.empty() ? 2 : ((int)list.size() + 2);
    }
    return (int)tab_rows(t).size() + 1;
}

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
        case kGamepadView: s.gamepad_view = !s.gamepad_view; break;
        case kMods: break;
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
        case kWindowMode: s.window_mode = wrap(s.window_mode + dir, 3); break;
        case kVolume: s.volume = std::clamp(s.volume + dir * 10, 0, 100); break;
    }
}

void cycle_mod_preset(const mods::DiscoveredMod& m, int dir) {
    for (const auto& pc : m.presets) {
        if (pc.options.size() <= 1) continue;
        int active_idx = 0;
        for (int i = 0; i < (int)pc.options.size(); ++i) {
            if (pc.options[i] == pc.active) {
                active_idx = i;
                break;
            }
        }
        int n = (int)pc.options.size();
        int next_idx = ((active_idx + dir) % n + n) % n;
        mods::set_preset(m.folder, pc.category, pc.options[next_idx]);
        return;
    }
    mods::set_enabled(m.folder, !m.enabled);
}

std::string format_active_presets(const mods::DiscoveredMod& m) {
    std::string out;
    for (const auto& pc : m.presets) {
        if (pc.active.empty()) continue;
        if (!out.empty()) out += ", ";
        if (!pc.category.empty() && m.presets.size() > 1) {
            out += pc.category + ": " + pc.active;
        } else {
            out += pc.active;
        }
    }
    return out;
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
    if (row == kClose || row == kMods || row == kModReload) return;
    if (row >= kModRowBase) {
        auto list = mods::status();
        int idx = row - kModRowBase;
        if (idx >= 0 && idx < (int)list.size()) {
            if (!list[idx].presets.empty()) {
                cycle_mod_preset(list[idx], dir);
            } else {
                mods::set_enabled(list[idx].folder, !list[idx].enabled);
            }
        }
        return;
    }
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
    else if (row == kMods) select_tab(kTabMods);
    else if (row == kModReload) mods::reload();
    else if (row >= kModRowBase) {
        auto list = mods::status();
        int idx = row - kModRowBase;
        if (idx >= 0 && idx < (int)list.size()) {
            mods::set_enabled(list[idx].folder, !list[idx].enabled);
        }
    } else change(+1);
}

void reorder_mod(int delta) {
    if (delta == 0) return;
    int tab_now, cursor;
    {
        std::lock_guard<std::mutex> l(g_lock);
        tab_now = g_tab;
        cursor = g_cursor;
    }
    if (tab_now != kTabMods) return;
    auto list = mods::status();
    if (cursor < 0 || cursor >= (int)list.size()) return;
    std::string target_folder = list[cursor].folder;
    if (mods::move_order(target_folder, delta)) {
        auto updated = mods::status();
        std::lock_guard<std::mutex> l(g_lock);
        for (int i = 0; i < (int)updated.size(); ++i) {
            if (updated[i].folder == target_folder) {
                g_cursor = i;
                break;
            }
        }
    }
}

void reload_mods() {
    int tab_now;
    {
        std::lock_guard<std::mutex> l(g_lock);
        tab_now = g_tab;
    }
    if (tab_now != kTabMods) return;
    mods::reload();
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
    all[kGamepadView] = {"GamePad screen", s.gamepad_view ? "On (F2 / Guide)" : "Off (F2 / Guide)"};
    all[kShowFps] = {"FPS counter", s.show_fps ? "On" : "Off"};
    all[kWindowMode] = {"Window mode", s.window_mode == 2 ? "Exclusive fullscreen" : s.window_mode == 1 ? "Borderless fullscreen" : "Windowed"};
    all[kMods] = {"Mods", mods::summary(), false, true};
    snprintf(b, sizeof b, "%d%%", s.volume);
    all[kVolume] = {"Volume", b};
    all[kModReload] = {"Apply / Reload Overlay (R)", "", true};
    all[kClose] = {"Close", "", true};
    if (tab_now == kTabMods) {
        auto list = mods::status();
        if (list.empty()) {
            v.rows.push_back({"Mods folder", mods::disabled_by_flag() ? "Off (--no-mods)" : "No mods found in mods/", false, true});
        } else {
            int priority = 1;
            for (const auto& m : list) {
                std::string label;
                if (m.enabled) {
                    label = std::to_string(priority++) + ". " + m.name;
                } else {
                    label = "-. " + m.name;
                }
                if (!m.version.empty()) label += " (v" + m.version + ")";

                std::string val;
                if (m.enabled && m.loaded_now) val = "[ON]";
                else if (m.enabled && !m.loaded_now) val = "[ON*]";
                else if (!m.enabled && m.loaded_now) val = "[OFF*]";
                else val = "[OFF]";

                std::string active_preset = format_active_presets(m);
                if (!active_preset.empty()) {
                    val += " <" + active_preset + ">";
                }

                uint64_t total_files = static_cast<uint64_t>(m.content_files) + static_cast<uint64_t>(m.aoc_files);
                char counts[64];
                if (total_files > 0 && m.patch_files > 0) {
                    snprintf(counts, sizeof counts, " (%llu files, %u patch)",
                             static_cast<unsigned long long>(total_files), m.patch_files);
                    val += counts;
                } else if (total_files > 0) {
                    snprintf(counts, sizeof counts, " (%llu %s)",
                             static_cast<unsigned long long>(total_files),
                             total_files == 1 ? "file" : "files");
                    val += counts;
                } else if (m.patch_files > 0) {
                    snprintf(counts, sizeof counts, " (%u %s)",
                             m.patch_files, m.patch_files == 1 ? "patch" : "patches");
                    val += counts;
                } else if (!m.texture_rules.empty() || m.shader_files > 0) {
                    val += " (gfx pack)";
                }

                if (m.overridden_files > 0) {
                    snprintf(counts, sizeof counts, " [%llu overridden]",
                             static_cast<unsigned long long>(m.overridden_files));
                    val += counts;
                }
                v.rows.push_back({label, val, false, false});
            }
            v.rows.push_back(all[kModReload]);
        }
        v.rows.push_back(all[kClose]);
    } else {
        for (int id : tab_rows(tab_now)) v.rows.push_back(all[id]);
        v.rows.push_back(all[kClose]);
    }
    if (tab_now == kTabGraphics) {
        snprintf(b, sizeof b, "Rendering at %gx now; resolution changes apply after a restart.", g_render_scale);
        v.note = b;
    } else if (tab_now == kTabControls) {
        v.note = "Controls changes apply at once and are saved to settings.ini.";
    } else if (tab_now == kTabMods) {
        v.note = mods::pending_restart()
                     ? "Enter/A: Toggle ON/OFF | Left/Right: Preset | U/D (X/Y): Order | R: Reload (boot packs need restart)"
                     : "Enter/A: Toggle ON/OFF | Left/Right: Preset | U/D (X/Y): Order | R: Apply / Reload overlay now";
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
