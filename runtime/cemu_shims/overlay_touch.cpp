// BotW-Native: draws the on-screen touch controls (runtime/src/host/touch.cpp) into the overlay's ImGui frame
// (GPU thread, called from LatteOverlay_render while the settings menu is closed). Background draw list, so
// ImGui windows (FPS counter, software keyboard) stay on top. Labels are stroked vector glyphs: the built-in
// ImGui font is a 13 px bitmap and would be blurry at thumb size.
#include <imgui.h>

#include <cmath>

#include "../src/host/touch.h"
#include "../src/host/touch_draw.h"
#include "../src/input/input.h"

namespace {
constexpr float kPi = 3.14159265f, kDeg = kPi / 180.0f;

struct Look {
    ImU32 fill, line, text;
};
Look look(bool pressed) {
    if (pressed) return {IM_COL32(255, 255, 255, 150), IM_COL32(255, 255, 255, 230), IM_COL32(18, 18, 22, 235)};
    return {IM_COL32(12, 12, 16, 70), IM_COL32(255, 255, 255, 95), IM_COL32(255, 255, 255, 175)};
}

float glyph_width(char c) {
    switch (c) {
        case 'A': return 0.64f;
        case 'B': return 0.53f;
        case 'X': return 0.58f;
        case 'Y': return 0.60f;
        case 'L': return 0.46f;
        case 'R': return 0.54f;
        case 'Z': return 0.56f;
        case '3': return 0.50f;
        default: return 0.60f;  // + -
    }
}

// One glyph, cap height s, top-left (x, y).
void glyph(ImDrawList* dl, char c, float x, float y, float s, ImU32 col, float th) {
    auto P = [&](float gx, float gy) { return ImVec2(x + gx * s, y + gy * s); };
    auto to = [&](float gx, float gy) { dl->PathLineTo(P(gx, gy)); };
    auto arc = [&](float cx, float cy, float r, float a0, float a1) {
        dl->PathArcTo(P(cx, cy), r * s, a0 * kDeg, a1 * kDeg, 16);
    };
    auto stroke = [&] { dl->PathStroke(col, 0, th); };
    // An arc's first point is where the straight part before it ends; never add that point twice (ImGui's thick
    // lines spike on zero-length segments).
    switch (c) {
        case 'A': to(0, 1), to(0.32f, 0), to(0.64f, 1), stroke(); to(0.13f, 0.64f), to(0.51f, 0.64f), stroke(); break;
        case 'B':
            to(0, 0.5f), to(0, 0), arc(0.25f, 0.25f, 0.25f, -90, 90), to(0, 0.5f), stroke();
            to(0, 0.5f), arc(0.28f, 0.75f, 0.25f, -90, 90), to(0, 1), to(0, 0.5f), stroke();
            break;
        case 'X': to(0, 0), to(0.58f, 1), stroke(); to(0.58f, 0), to(0, 1), stroke(); break;
        case 'Y': to(0, 0), to(0.3f, 0.5f), to(0.6f, 0), stroke(); to(0.3f, 0.5f), to(0.3f, 1), stroke(); break;
        case 'L': to(0, 0), to(0, 1), to(0.46f, 1), stroke(); break;
        case 'R':
            to(0, 1), to(0, 0), arc(0.25f, 0.25f, 0.25f, -90, 90), to(0, 0.5f), stroke();
            to(0.2f, 0.5f), to(0.54f, 1), stroke();
            break;
        case 'Z': to(0, 0), to(0.56f, 0), to(0, 1), to(0.56f, 1), stroke(); break;
        case '3': arc(0.25f, 0.25f, 0.25f, -150, 90), stroke(), arc(0.25f, 0.75f, 0.25f, -90, 150), stroke(); break;
        case '+': to(0, 0.5f), to(0.6f, 0.5f), stroke(); to(0.3f, 0.2f), to(0.3f, 0.8f), stroke(); break;
        case '-': to(0, 0.5f), to(0.6f, 0.5f), stroke(); break;
        default: break;
    }
}

void text(ImDrawList* dl, const char* s, ImVec2 c, float h, ImU32 col, float th) {
    const float gap = 0.2f * h;
    float w = -gap;
    for (const char* p = s; *p; p++) w += glyph_width(*p) * h + gap;
    float x = c.x - w * 0.5f;
    for (const char* p = s; *p; p++) {
        glyph(dl, *p, x, c.y - h * 0.5f, h, col, th);
        x += glyph_width(*p) * h + gap;
    }
}
}  // namespace

namespace touch {
void draw() {
    const DrawState st = draw_state();
    if (!st.visible || st.w <= 0) return;
    ImGuiIO& io = ImGui::GetIO();
    const float k = io.DisplaySize.x / st.w;  // layout space -> ImGui pixels (normally 1)
    const float u = st.u * k;                 // one design point
    const float lw = 1.6f * u, tw = 2.1f * u;  // outline, label stroke
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    auto P = [&](float x, float y) { return ImVec2(x * k, y * k); };

    for (const DrawItem& it : st.items) {
        const ImVec2 c = P(it.x, it.y);
        const float r = it.r * k;
        const Look lk = look(it.pressed);
        switch (it.shape) {
            case DrawItem::kCircle:
                dl->AddCircleFilled(c, r, lk.fill);
                dl->AddCircle(c, r, lk.line, 0, lw);
                text(dl, it.label, c, r * (it.label[1] ? 0.62f : 0.78f), lk.text, tw);
                break;
            case DrawItem::kPill: {
                const float hh = it.hh * k;
                const ImVec2 a(c.x - r, c.y - hh), b(c.x + r, c.y + hh);
                dl->AddRectFilled(a, b, lk.fill, hh);
                dl->AddRect(a, b, lk.line, hh, 0, lw);
                text(dl, it.label, c, hh * 0.95f, lk.text, tw);
                break;
            }
            case DrawItem::kMenu: {
                dl->AddCircleFilled(c, r, lk.fill);
                dl->AddCircle(c, r, lk.line, 0, lw);
                const float hw = r * 0.46f, dy = r * 0.3f;
                for (int i = -1; i <= 1; i++)
                    dl->AddLine({c.x - hw, c.y + i * dy}, {c.x + hw, c.y + i * dy}, lk.text, tw * 0.9f);
                break;
            }
            case DrawItem::kToggle: {  // an eye; crossed out while the controls are shown (tap to hide)
                dl->AddCircleFilled(c, r, lk.fill);
                dl->AddCircle(c, r, lk.line, 0, lw);
                const float a = r * 0.58f, s = r * 0.32f, R = (a * a + s * s) / (2 * s), off = R - s;
                const float t0 = std::atan2(off, a);  // where the lids meet the corners
                dl->PathArcTo({c.x, c.y + off}, R, -kPi + t0, -t0, 16);
                dl->PathStroke(lk.text, 0, tw * 0.8f);
                dl->PathArcTo({c.x, c.y - off}, R, t0, kPi - t0, 16);
                dl->PathStroke(lk.text, 0, tw * 0.8f);
                dl->AddCircleFilled(c, r * 0.17f, lk.text);
                if (!st.collapsed) dl->AddLine({c.x - a * 0.85f, c.y - a * 0.85f}, {c.x + a * 0.85f, c.y + a * 0.85f}, lk.text, tw * 0.8f);
                break;
            }
            case DrawItem::kDPad: {
                const Look base = look(false);
                dl->AddCircleFilled(c, r, base.fill);
                dl->AddCircle(c, r, base.line, 0, lw);
                const struct {
                    uint32_t bit;
                    float dx, dy;
                } dirs[] = {{input::BTN_UP, 0, -1}, {input::BTN_DOWN, 0, 1}, {input::BTN_LEFT, -1, 0}, {input::BTN_RIGHT, 1, 0}};
                for (auto& d : dirs) {
                    const bool on = (it.dpad & d.bit) != 0;
                    const Look lk2 = look(on);
                    const ImVec2 at(c.x + d.dx * r * 0.6f, c.y + d.dy * r * 0.6f);
                    if (on) dl->AddCircleFilled(at, r * 0.36f, lk2.fill);
                    const float t = r * 0.17f;  // arrow pointing outwards
                    const ImVec2 tip(at.x + d.dx * t, at.y + d.dy * t);
                    const ImVec2 b1(at.x - d.dx * t * 0.6f - d.dy * t, at.y - d.dy * t * 0.6f + d.dx * t);
                    const ImVec2 b2(at.x - d.dx * t * 0.6f + d.dy * t, at.y - d.dy * t * 0.6f - d.dx * t);
                    dl->AddTriangleFilled(tip, b1, b2, lk2.text);
                }
                break;
            }
        }
    }

    if (st.collapsed) return;
    // floating stick
    const float sr = st.stick_r * k, kr = sr * 0.42f;
    if (st.stick) {
        const ImVec2 b = P(st.base_x, st.base_y);
        ImVec2 kn = P(st.knob_x, st.knob_y);
        const float dx = kn.x - b.x, dy = kn.y - b.y, m = std::sqrt(dx * dx + dy * dy);
        if (m > sr) kn = ImVec2(b.x + dx / m * sr, b.y + dy / m * sr);
        dl->AddCircleFilled(b, sr, IM_COL32(12, 12, 16, 60));
        dl->AddCircle(b, sr, IM_COL32(255, 255, 255, 120), 0, lw);
        dl->AddCircleFilled(kn, kr, IM_COL32(255, 255, 255, 130));
        dl->AddCircle(kn, kr, IM_COL32(255, 255, 255, 230), 0, lw);
    } else {
        const ImVec2 h = P(st.home_x, st.home_y);
        dl->AddCircle(h, sr, IM_COL32(255, 255, 255, 60), 0, lw);
        dl->AddCircleFilled(h, kr, IM_COL32(12, 12, 16, 50));
        dl->AddCircle(h, kr, IM_COL32(255, 255, 255, 80), 0, lw);
    }
    // camera finger
    if (st.look) {
        const ImVec2 p = P(st.look_x, st.look_y);
        dl->AddCircle(p, 26 * u, IM_COL32(255, 255, 255, 80), 0, lw);
        dl->AddCircleFilled(p, 4 * u, IM_COL32(255, 255, 255, 110));
    }
}
}  // namespace touch
