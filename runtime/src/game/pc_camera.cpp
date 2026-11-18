// Mouse camera: port of Zelda-on-PC's camera patches (src/pcedition/PCGraphics.cpp WriteCameraPatch and the
// HLE_Cam* handlers in PCInput.cpp, MPL-2.0, https://github.com/mircea32000/Zelda-on-PC). This is a
// reimplementation of their approach as BOTW_HOOKs; docs/pc_hooks.md has the full address map.
//
// Main (chase) camera, U-King.rpx 1.5.0. The mouse rotation is injected in degrees into the camera
// controller's own per-frame deltas, so it is exact (1:1) and independent of the stick response curve:
// - 0x02B9B8A0 update entry (r3 = controller): take this frame's mouse motion, convert to degrees.
// - 0x02B9C950 manual yaw: f13 = yaw this frame; add the mouse yaw, scaled by [r31+0x124] like the stick.
// - 0x02B9C120 pitch: the controller works in a normalized pitch parameter (mapping 0x02B96758 from the pitch
//   in degrees at [r31+0xCC]); the mouse pitch is converted with that mapping, so the camera distance follows
//   the game's own orbit curve, and added to the stick delta at 0x02B9C188 / 0x02B9C1AC.
// While the mouse turns the camera, the right stick carries a deflection just past the deadzone so the game
// stays in manual camera mode (input::mouse::manual_stick).
//
// BOTW_PCE_SPIN=<degrees per frame> still adds a constant turn (phase 1 hook test).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "../input/mouse.h"
#include "../settings.h"
#include "hooks.h"

extern "C" void rt_call(PPCContext* c, uint32_t target);
namespace pccontrols {
bool owns_mouse();
}

namespace {
// Any gameplay camera updated recently: Link is in play (not a menu, map, cutscene or loading screen).
std::atomic<int64_t> g_cam_seen_ms{-1000000};
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void note_camera() { g_cam_seen_ms = now_ms(); }
}  // namespace

bool pce_gameplay_camera() { return now_ms() - g_cam_seen_ms.load() < 150; }

// The zoomed aim camera (bow drawn, scope) updated recently: the wheel steps arrows, not weapons.
std::atomic<int64_t> g_bow_cam_ms{-1000000};
bool pce_bow_camera() { return now_ms() - g_bow_cam_ms.load() < 150; }

// Entries (after stwu) of the other player cameras; phases 7-9 add their mouse rotation here.
BOTW_HOOK(0x02bee0f8) { note_camera(); }  // climbing
BOTW_HOOK(0x02bd2a44) { note_camera(); }  // horse
BOTW_HOOK(0x02bdfb20) { note_camera(); }  // Master Cycle Zero
BOTW_HOOK(0x02bf0234) { note_camera(); }  // lock-on / Magnesis
BOTW_HOOK(0x02bced3c) { note_camera(); }  // camera rune
BOTW_HOOK(0x02be8b30) { note_camera(); }  // shield surf
BOTW_HOOK(0x02bf5acc) { note_camera(); }  // sand seal
BOTW_HOOK(0x02bf8f88) { note_camera(); }  // Zora waterfall
BOTW_HOOK(0x02b9250c) {  // zoomed aim (scope, bow drawn)
    note_camera();
    g_bow_cam_ms = now_ms();
}
BOTW_HOOK(0x02b94e30) { note_camera(); }  // rune aim

namespace {
constexpr double kDegPerCount = 0.022;  // camera degrees per mouse count at sensitivity 1 (Source-style)
constexpr double kGameYawSign = -1.0;   // game delta sign relative to "camera turns right"
constexpr double kGamePitchSign = -1.0; // ... and to "camera looks up"
constexpr double kMaxPending = 90.0;    // degrees
constexpr auto kStaleGap = std::chrono::milliseconds(100);  // camera not updated this long: drop the motion

using Clock = std::chrono::steady_clock;

uint32_t g_cam = 0;        // last main camera controller seen
uint64_t g_cam_ticks = 0;  // main camera updates since boot
Clock::time_point g_last_tick{};
double g_pending_yaw = 0, g_pending_pitch = 0;  // degrees, right / up positive
int g_yaw_age = 0, g_pitch_age = 0;  // camera updates since the pending value was last consumed
float g_norm_delta = 0;  // mouse pitch in the controller's normalized pitch units, this update
float g_spin = [] {
    const char* s = std::getenv("BOTW_PCE_SPIN");
    return s ? (float)std::atof(s) : 0.0f;
}();

// Single-precision result as the recompiled code stores it (ps0 and ps1).
inline void set_fs(PPCContext* c, int n, double v) {
    v = (float)v;
    c->f[n] = v;
    c->ps1[n] = v;
}

double take(double& v, int& age) {
    double r = v;
    v = 0;
    age = 0;
    return r;
}

// Call a guest function with r3 and f1 set, return its f1; every register of the caller is preserved.
double guest_call_f1(PPCContext* c, uint32_t fn, uint32_t r3, double f1) {
    PPCContext saved = *c;
    c->r[3] = r3;
    c->f[1] = c->ps1[1] = f1;
    c->lr = 0;
    rt_call(c, fn);
    const double r = c->f[1];
    *c = saved;
    return r;
}
}  // namespace

BOTW_HOOK(0x02b9b8a0) {  // main camera update entry, after stwu: r3 = camera controller
    if (c->r[3] != g_cam) {
        LOG("pce: main camera controller 0x%08x (tick %llu)", c->r[3], (unsigned long long)g_cam_ticks);
        g_cam = c->r[3];
    }
    g_cam_ticks++;
    g_norm_delta = 0;
    note_camera();
    if (pccontrols::owns_mouse()) {  // an open selector steps with the mouse; the camera stays
        input::mouse::take();
        g_pending_yaw = g_pending_pitch = 0;
        return;
    }

    // Rotation the camera did not ask for within two updates (pitch locked, scripted camera) is dropped.
    if (++g_yaw_age > 2) g_pending_yaw = 0;
    if (++g_pitch_age > 2) g_pending_pitch = 0;

    const auto now = Clock::now();
    const bool stale = now - g_last_tick > kStaleGap;  // other camera, menu, loading: motion since is not ours
    g_last_tick = now;
    const input::mouse::Delta d = input::mouse::take();
    const Settings s = settings::get();
    if (stale || !s.mouse_camera || (d.dx == 0 && d.dy == 0)) return;

    const double sens = kDegPerCount * s.mouse_sens;
    const double yaw = d.dx * sens * (s.mouse_invert_x ? -1.0 : 1.0);
    const double pitch = -d.dy * sens * s.mouse_sens_y * (s.mouse_invert_y ? -1.0 : 1.0);
    g_pending_yaw = std::clamp(g_pending_yaw + yaw, -kMaxPending, kMaxPending);
    g_pending_pitch = std::clamp(g_pending_pitch + pitch, -kMaxPending, kMaxPending);
    if (yaw != 0) g_yaw_age = 0;
    if (pitch != 0) g_pitch_age = 0;
    input::mouse::set_manual_dir((float)g_pending_yaw, (float)g_pending_pitch);
}

BOTW_HOOK(0x02b9c950) {  // fmuls f13,f10,f6: yaw rotation this frame; r31 = camera controller
    // An open selector steps with right-stick flicks; when the game shows no carousel (nothing to pick, e.g.
    // no bow while aiming) they would turn the camera, so the stick's yaw is dropped meanwhile.
    if (pccontrols::owns_mouse()) return set_fs(c, 13, 0.0);
    const double add = kGameYawSign * (take(g_pending_yaw, g_yaw_age) + g_spin);
    if (add == 0) return;
    set_fs(c, 13, (float)add * mem::rf32(c->r[31] + 0x124) + c->f[13]);
}

BOTW_HOOK(0x02b9c120) {  // fmr f23,f1: f1 = normalized pitch of the current pitch [r31+0xCC]
    const double pitch = kGamePitchSign * take(g_pending_pitch, g_pitch_age);
    if (pitch == 0) return;
    const float target = (float)(mem::rf32(c->r[31] + 0xcc) + pitch);
    const double mapped = guest_call_f1(c, 0x02b96758, c->r[31], target);
    g_norm_delta = (float)(mapped - c->f[23]);
}

BOTW_HOOK(0x02b9c184) {  // addi r24,0,0 after f1 = stick pitch delta; same as the yaw above
    if (pccontrols::owns_mouse()) c->f[1] = c->ps1[1] = 0.0;
}

BOTW_HOOK(0x02b9c188) {  // fadds f23,f23,f1: pitch += stick delta (stick path)
    if (g_norm_delta != 0) set_fs(c, 23, c->f[23] + g_norm_delta);
}

BOTW_HOOK(0x02b9c1ac) {  // fadds f23,f23,f1: pitch += delta (automatic path)
    if (g_norm_delta != 0) set_fs(c, 23, c->f[23] + g_norm_delta);
}
