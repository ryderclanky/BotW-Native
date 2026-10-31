// BotW-Native: replaces Cemu's LatteOverlay. Drawn with Cemu's ImGui pass right after the TV image
// (LatteRenderTarget_copyToBackbuffer, GPU thread): an optional FPS counter and the in-game settings
// menu (runtime/src/host/menu.cpp). ImGui's built-in font is used.
#include <imgui.h>

#include <algorithm>
#include <pthread.h>

#include <atomic>

#include "../src/host/menu.h"
#include "../src/host/swkbd_state.h"
#include "../src/host/touch.h"
#include "../src/host/touch_draw.h"
#include "../src/bench/bench.h"
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
    bench::on_stats(drawcalls);
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

    swkbd::View keyboard = swkbd::keyboard().view();
    if (keyboard.active) {
        ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowBgAlpha(0.93f);
        if (ImGui::Begin("##software_keyboard", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
            ImGui::SetWindowFontScale(ui);
            ImGui::TextUnformatted("Enter a name");
            ImGui::TextUnformatted(keyboard.text.empty() ? " " : keyboard.text.c_str());
            ImGui::Separator();
            for (int i = 0; i < 44; ++i) {
                if (i % 10 != 0 && i < 40) ImGui::SameLine();
                if (i > 40) ImGui::SameLine();
                if (i == keyboard.selected)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.38f, 0.12f, 1.0f));
                char label[2] = {i < 40 ? swkbd::Keyboard::keys[i] : '\0', '\0'};
                const char* special[] = {"Space", "Delete", "OK", "Cancel"};
                ImGui::PushID(i);
                if (ImGui::Button(i < 40 ? label : special[i - 40], {i < 40 ? 30 * ui : 78 * ui, 27 * ui}))
                    swkbd::keyboard().select(i);
                ImGui::PopID();
                if (i == keyboard.selected) ImGui::PopStyleColor();
            }
            ImGui::TextUnformatted("Keyboard: type, Enter to accept, Esc to cancel");
            ImGui::TextUnformatted("Controller: D-pad to select, A to enter, X to delete, Start to accept");
        }
        ImGui::End();
        return;
    }

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
        const float xs = ImGui::GetFrameHeight();  // X in the top-right corner, same height as a row
        const float panel_w = label_w + value_w + 24 * ui + xs;  // fixed width: the panel does not jump between tabs
        const ImVec4 gold = ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 215, 90, 255));
        ImGui::Dummy({panel_w, 0});
        ImGui::TextUnformatted("BotW-Native settings");
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - xs);
        if (ImGui::Button("X", {xs, xs})) menu::close();
        const ImVec2 xa = ImGui::GetItemRectMin(), xb = ImGui::GetItemRectMax();

        // Tab bar: L1 / R1 on a controller, Tab or PageUp / PageDown on a keyboard, a tap on touch.
        ImGui::TextDisabled("L1");
        for (int t = 0; t < (int)v.tabs.size(); t++) {
            ImGui::SameLine();
            ImGui::PushID(1000 + t);
            const bool cur = t == v.tab;
            if (cur) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(gold.x * 0.45f, gold.y * 0.45f, gold.z * 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, gold);
            }
            if (ImGui::Button(v.tabs[t].c_str()) && !cur) menu::select_tab(t);
            if (cur) ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("R1");
        ImGui::Separator();

        // Body: the rows of the tab, or the read-only text of About. Padded to a fixed height.
        const float body_top = ImGui::GetCursorPosY();
        const float body_h = 9 * ImGui::GetFrameHeightWithSpacing();
        for (int i = 0; i < (int)v.rows.size(); i++) {
            const menu::Row& r = v.rows[i];
            ImGui::PushID(i);
            bool sel = i == v.cursor;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Text, gold);
            if (ImGui::Selectable(r.label.c_str(), sel, 0, {r.action ? label_w + value_w : label_w, 0})) menu::click(i, 0);
            if (!r.action) {
                ImGui::SameLine();
                if (!r.readonly && ImGui::ArrowButton("##l", ImGuiDir_Left)) menu::click(i, -1);
                if (!r.readonly) ImGui::SameLine();
                ImGui::TextUnformatted(r.value.c_str());
                if (!r.readonly) {
                    ImGui::SameLine(label_w + value_w + 24 * ui);
                    if (ImGui::ArrowButton("##r", ImGuiDir_Right)) menu::click(i, +1);
                }
            }
            if (sel) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        if (!v.text.empty()) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + panel_w);
            for (size_t i = 0; i < v.text.size(); i++) {
                if (i == 0) ImGui::TextColored(gold, "%s", v.text[i].c_str());
                else if (v.text[i].empty()) ImGui::Spacing();
                else ImGui::TextWrapped("%s", v.text[i].c_str());
            }
            ImGui::PopTextWrapPos();
        }
        const float used = ImGui::GetCursorPosY() - body_top;
        if (used < body_h) ImGui::Dummy({0, body_h - used});
        ImGui::Separator();

        ImGui::SetWindowFontScale(ui);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + panel_w);
        if (!v.note.empty()) ImGui::TextWrapped("%s", v.note.c_str());
        ImGui::TextWrapped("D-pad / left stick: move and change   L1 / R1: tab   Cross (A): select   Circle (B): close");
        ImGui::SetWindowFontScale(ui * 1.1f);
        ImGui::TextColored(gold, "Press touchpad to close");
        ImGui::SetWindowFontScale(ui);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Keyboard: F1 or Esc closes, Tab switches tab.  Touch: X or a tap outside closes.");
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        // for touch (host side, see menu::touch_close): panel and X in fractions of the screen
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize(), d = io.DisplaySize;
        const float panel[4] = {wp.x / d.x, wp.y / d.y, (wp.x + ws.x) / d.x, (wp.y + ws.y) / d.y};
        const float xbtn[4] = {xa.x / d.x, xa.y / d.y, xb.x / d.x, xb.y / d.y};
        menu::set_rects(panel, xbtn);
    }
    ImGui::End();
}
