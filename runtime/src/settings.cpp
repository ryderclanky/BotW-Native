#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "audio/output.h"
#include "host/touch.h"
#include "host/window.h"
#include "latte/latte.h"
#include "log.h"
#include "os.h"

namespace settings {
namespace {
std::mutex g_lock;
Settings g_s;
std::string g_path;

void save_locked() {
    if (g_path.empty()) return;
    FILE* f = fopen(g_path.c_str(), "w");
    if (!f) return;
    fprintf(f, "# BotWRecomp settings (also editable from the in-game menu: touchpad, L3+R3 or F1)\n");
    fprintf(f, "gpu=%s\ngpu_name=%s\n", g_s.gpu.c_str(), g_s.gpu_name.c_str());
    fprintf(f, "fps=%d\nscale=%g\ndisplay=%d\nfilter=%d\ngyro=%d\nrumble=%d\nlayout=%d\nshow_fps=%d\nvolume=%d\ntouch=%d\ntouch_hidden=%d\ngamepad_view=%d\n", g_s.fps,
            g_s.scale, g_s.display, g_s.filter, g_s.gyro, g_s.rumble, g_s.layout, g_s.show_fps, g_s.volume, g_s.touch, g_s.touch_hidden,
            g_s.gamepad_view);
    fclose(f);
}
}  // namespace

Settings get() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_s;
}

void apply() {
    Settings s = get();
    g_target_fps = s.fps;
    latte::set_display_mode(s.display);
    latte::set_output_filter(s.filter);
    latte::set_gamepad_view(s.gamepad_view);
    window::set_gyro(s.gyro);
    window::set_rumble(s.rumble);
    window::set_layout(s.layout);
    audio::set_volume(s.volume / 100.0f);
    touch::set_mode(s.touch);
    touch::set_hidden(s.touch_hidden);
}

void set(const Settings& s, bool save) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_s = s;
        if (save) save_locked();
    }
    apply();
}

void load(const std::string& path) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_path = path;
        if (FILE* f = fopen(path.c_str(), "r")) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                char* eq = strchr(line, '=');
                if (line[0] == '#' || !eq) continue;
                *eq = 0;
                const char* v = eq + 1;
                if (!strcmp(line, "fps")) g_s.fps = atoi(v) >= 60 ? 60 : 30;
                else if (!strcmp(line, "scale")) g_s.scale = (float)atof(v);
                else if (!strcmp(line, "display")) g_s.display = atoi(v);
                else if (!strcmp(line, "filter")) g_s.filter = atoi(v);
                else if (!strcmp(line, "gyro")) g_s.gyro = atoi(v) != 0;
                else if (!strcmp(line, "rumble")) g_s.rumble = atoi(v) != 0;
                else if (!strcmp(line, "layout")) {
                    int n = atoi(v);
                    g_s.layout = n < -1 ? -1 : n > 1 ? 1 : n;
                }
                else if (!strcmp(line, "show_fps")) g_s.show_fps = atoi(v) != 0;
                else if (!strcmp(line, "volume")) g_s.volume = atoi(v);
                else if (!strcmp(line, "touch")) g_s.touch = atoi(v);
                else if (!strcmp(line, "touch_hidden")) g_s.touch_hidden = atoi(v) != 0;
                else if (!strcmp(line, "gamepad_view")) g_s.gamepad_view = atoi(v) != 0;
                else if (!strcmp(line, "gpu") || !strcmp(line, "gpu_name")) {
                    std::string s = v;
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
                    (line[3] ? g_s.gpu_name : g_s.gpu) = s;
                }
            }
            fclose(f);
            LOG("settings: %s", path.c_str());
        }
    }
    apply();
}
}  // namespace settings

// Read by the Vulkan renderer when it picks a GPU.
void botw_gpu_preference(std::string& uuid, std::string& name) {
    Settings s = settings::get();
    uuid = s.gpu;
    name = s.gpu_name;
}
