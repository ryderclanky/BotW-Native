// BotW-Native: replaces Cemu's LatteOverlay. Drawn with Cemu's ImGui pass right after the TV image
// (LatteRenderTarget_copyToBackbuffer, GPU thread): an optional FPS counter and the in-game settings
// menu (runtime/src/host/menu.cpp). ImGui's built-in font is used.
#include <imgui.h>

#include <algorithm>
#include <pthread.h>

#include <atomic>

#include "../src/host/menu.h"
#include "../src/host/touch.h"
#include "../src/host/touch_draw.h"
#include "../src/settings.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "title_diag.h"

void log_write(const char* fmt, ...);
namespace threads {
std::string main_wait_report();
}

namespace {
std::atomic<float> g_fps{0};
std::atomic<int> g_draws{0};
}  // namespace

// Cemu's GPU thread (LatteThread), for the host loop's slow-frame sampler (window.mm).
std::atomic<pthread_t> g_gpu_thread{};  // 0 / nullptr until the GPU thread publishes itself

void LatteOverlay_updateStats(double fps, sint32 drawcalls, sint32 fastDrawcalls) {
    g_gpu_thread = pthread_self();  // stats are updated on the GPU thread
    g_fps = (float)fps;
    g_draws = drawcalls;
    static int n = 0;
    if (fps > 0 && (n++ % 5) == 0)
        log_write("[gpu] %.1f fps, %d draws/frame (%d fast); main thread waits: %s", fps, drawcalls, fastDrawcalls,
                  threads::main_wait_report().c_str());
    title_diag_log();
}

void LatteOverlay_render(bool padView) {
    if (padView) return;
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    const float ui = std::max(1.0f, io.DisplaySize.y / 540.0f);  // the built-in font is 13 px

    float px, py;
    bool down;
    menu::pointer_state(&px, &py, &down);
    io.AddMousePosEvent(px, py);
    io.AddMouseButtonEvent(0, down);

    const ImGuiWindowFlags overlay = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (settings::get().show_fps) {
        float sl, st, sr, sb;  // top-right, inside the safe area with some padding
        menu::safe_area(&sl, &st, &sr, &sb);
        ImVec2 pos{io.DisplaySize.x - sr - 16 * ui, st + 12 * ui}, pivot{1.0f, 0.0f};
        const touch::DrawState ts = touch::draw_state();
        if (ts.visible && !ts.collapsed && ts.w > 0)  // on-screen ZR is up there: centered under the top button row
            for (const touch::DrawItem& it : ts.items)
                if (it.shape == touch::DrawItem::kMenu) {
                    const float k = io.DisplaySize.x / ts.w;
                    pos = {it.x * k, (it.y + it.r + 10 * ts.u) * k}, pivot = {0.5f, 0.0f};
                }
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
        ImGui::SetNextWindowBgAlpha(0.45f);
        if (ImGui::Begin("##fps", nullptr, overlay | ImGuiWindowFlags_NoInputs)) {
            ImGui::SetWindowFontScale(ui);
            ImGui::Text("%.0f fps  %d draws", (double)g_fps, (int)g_draws);
        }
        ImGui::End();
    }

    menu::View v = menu::view();
    if (!v.open) touch::draw();
    if (!v.open) return;
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.88f);
    if (ImGui::Begin("##menu", nullptr, overlay)) {
        ImGui::SetWindowFontScale(ui * 1.25f);
        const float label_w = 150 * ui, value_w = 230 * ui;
        ImGui::TextUnformatted("BotW-Native settings");
        const float xs = ImGui::GetFrameHeight();  // X in the top-right corner
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - xs);
        if (ImGui::Button("X", {xs, xs})) menu::close();
        const ImVec2 xa = ImGui::GetItemRectMin(), xb = ImGui::GetItemRectMax();
        ImGui::Separator();
        for (int i = 0; i < (int)v.rows.size(); i++) {
            const menu::Row& r = v.rows[i];
            ImGui::PushID(i);
            bool sel = i == v.cursor;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 215, 90, 255));
            if (ImGui::Selectable(r.label.c_str(), sel, 0, {r.action ? label_w + value_w : label_w, 0})) menu::click(i, 0);
            if (!r.action) {
                ImGui::SameLine();
                if (ImGui::ArrowButton("##l", ImGuiDir_Left)) menu::click(i, -1);
                ImGui::SameLine();
                ImGui::TextUnformatted(r.value.c_str());
                ImGui::SameLine(label_w + value_w + 24 * ui);
                if (ImGui::ArrowButton("##r", ImGuiDir_Right)) menu::click(i, +1);
            }
            if (sel) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::SetWindowFontScale(ui);
        ImGui::TextUnformatted(v.note.c_str());
        ImGui::TextUnformatted("D-pad: choose / change   A: select   B, touchpad, F1, X or a tap outside: close");
        // for touch (host side, see menu::touch_close): panel and X in fractions of the screen
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize(), d = io.DisplaySize;
        const float panel[4] = {wp.x / d.x, wp.y / d.y, (wp.x + ws.x) / d.x, (wp.y + ws.y) / d.y};
        const float xbtn[4] = {xa.x / d.x, xa.y / d.y, xb.x / d.x, xb.y / d.y};
        menu::set_rects(panel, xbtn);
    }
    ImGui::End();
}
