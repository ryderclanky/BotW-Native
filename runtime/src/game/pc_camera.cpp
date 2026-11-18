// Mouse camera: port of Zelda-on-PC's camera patches (src/pcedition/PCGraphics.cpp WriteCameraPatch and the
// HLE_Cam* handlers in PCInput.cpp, MPL-2.0, https://github.com/mircea32000/Zelda-on-PC). This is a
// reimplementation of their approach as BOTW_HOOKs; docs/pc_hooks.md has the full address map.
//
// The mouse motion is taken by whichever camera with mouse support updates (feed()), converted to degrees and
// kept as pending yaw / pitch; that camera's own hooks then add it to its rotation, so it is exact (1:1) and
// independent of the stick response curve. U-King.rpx 1.5.0.
//
// Main (chase) camera:
// - 0x02B9B8A0 update entry (r3 = controller): feed.
// - 0x02B9C950 manual yaw: f13 = yaw this frame; add the mouse yaw, scaled by [r31+0x124] like the stick.
// - 0x02B9C120 pitch: the controller works in a normalized pitch parameter (mapping 0x02B96758 from the pitch
//   in degrees at [r31+0xCC]); the mouse pitch is converted with that mapping, so the camera distance follows
//   the game's own orbit curve, and added to the stick delta at 0x02B9C188 / 0x02B9C1AC.
// Aim cameras (zoomed: scope and drawn bow, 0x02B9250C; rune aim: Magnesis/Stasis/Cryonis, 0x02B94E30): the
// pitch is added to the new pitch in degrees, the yaw is kept as an absolute target (aimed relative to the
// smoothed facing, per-frame deltas would lag behind under continuous movement).
// Camera rune (0x02BCED3C): pitch in degrees at +0x28 before the update; yaw like the aim cameras.
// Lock-on (0x02BF0234, also the Magnesis camera class): yaw +0xAC / pitch +0xA8 in degrees, and the eye of its
// pose turned around the look-at point so its easing has nothing to catch up.
// While the mouse turns a camera, the right stick carries a deflection just past the deadzone so the game
// stays in manual camera mode (input::mouse::manual_stick).
//
// BOTW_PCE_SPIN=<degrees per frame> still adds a constant turn (phase 1 hook test).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

// Entries (after stwu) of the other player cameras; phases 8-9 add their mouse rotation here.
BOTW_HOOK(0x02bee0f8) { note_camera(); }  // climbing
BOTW_HOOK(0x02bd2a44) { note_camera(); }  // horse
BOTW_HOOK(0x02bdfb20) { note_camera(); }  // Master Cycle Zero
BOTW_HOOK(0x02be8b30) { note_camera(); }  // shield surf
BOTW_HOOK(0x02bf5acc) { note_camera(); }  // sand seal
BOTW_HOOK(0x02bf8f88) { note_camera(); }  // Zora waterfall

namespace {
constexpr double kDegPerCount = 0.022;  // camera degrees per mouse count at sensitivity 1 (Source-style)
constexpr double kGameYawSign = -1.0;   // game delta sign relative to "camera turns right"
constexpr double kGamePitchSign = -1.0; // ... and to "camera looks up"
constexpr double kAimYawSign = -1.0;    // the same for the aim cameras
constexpr double kAimPitchSign = -1.0;
constexpr double kMaxPending = 90.0;    // degrees
constexpr auto kStaleGap = std::chrono::milliseconds(100);  // no camera took motion this long: drop it

using Clock = std::chrono::steady_clock;

uint32_t g_cam = 0;        // last main camera controller seen
uint64_t g_cam_ticks = 0;  // main camera updates since boot
Clock::time_point g_last_feed{};
double g_pending_yaw = 0, g_pending_pitch = 0;  // degrees, right / up positive
int g_yaw_age = 0, g_pitch_age = 0;  // camera updates since the pending value was last consumed
float g_norm_delta = 0;  // mouse pitch in the controller's normalized pitch units, this update
std::atomic<int64_t> g_magnet_ms{-1000000};  // Magnesis action updated (it drives the lock-on camera class)
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

double wrap180(double a) {
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0) a += 360.0;
    return a - 180.0;
}

// A camera with mouse support updates: take the mouse motion since the last one into the pending rotation.
// zoomed: the aiming sensitivity applies (bow drawn, scope, camera rune).
void feed(bool zoomed) {
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
    const bool stale = now - g_last_feed > kStaleGap;  // camera without mouse support, menu, loading: not ours
    g_last_feed = now;
    const input::mouse::Delta d = input::mouse::take();
    const Settings s = settings::get();
    if (stale || !s.mouse_camera || (d.dx == 0 && d.dy == 0)) return;

    const double sens = kDegPerCount * s.mouse_sens * (zoomed ? s.mouse_sens_aim : 1.0f);
    const double yaw = d.dx * sens * (s.mouse_invert_x ? -1.0 : 1.0);
    const double pitch = -d.dy * sens * s.mouse_sens_y * (s.mouse_invert_y ? -1.0 : 1.0);
    g_pending_yaw = std::clamp(g_pending_yaw + yaw, -kMaxPending, kMaxPending);
    g_pending_pitch = std::clamp(g_pending_pitch + pitch, -kMaxPending, kMaxPending);
    if (yaw != 0) g_yaw_age = 0;
    if (pitch != 0) g_pitch_age = 0;
    input::mouse::set_manual_dir((float)g_pending_yaw, (float)g_pending_pitch);
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

// Aim yaw: the cameras turn relative to the current (smoothed) facing `base`, so an absolute target is kept
// and target - base returned every frame. stick = the stick's turn this frame (degrees).
double g_aim_target = 0;
int64_t g_aim_yaw_ms = -1000000;
double aim_yaw_abs(double stick, double base) {
    const int64_t t = now_ms();
    if (t - g_aim_yaw_ms > 100) g_aim_target = base;  // aim camera (re)started
    g_aim_yaw_ms = t;
    g_aim_target += stick + kAimYawSign * take(g_pending_yaw, g_yaw_age);
    const double out = wrap180(g_aim_target - base);
    g_aim_target = base + out;
    return (float)out;
}

double aim_pitch() { return kAimPitchSign * take(g_pending_pitch, g_pitch_age); }

// Turns the eye of a camera's pose ([[camera+0x10]]) around its look-at point by the rotation (degrees, right /
// up positive), the pitch of the eye kept within the limits.
void turn_eye(uint32_t cam, double yaw, double pitch, double min_pitch, double max_pitch) {
    if ((yaw == 0 && pitch == 0) || !mem::readable(cam + 0x10, 4)) return;
    const uint32_t holder = mem::r32(cam + 0x10);
    if (!holder || !mem::readable(holder, 4)) return;
    const uint32_t pose = mem::r32(holder);
    if (!pose || !mem::readable(pose + 0x550, 0x18)) return;
    const double ex = mem::rf32(pose + 0x550), ey = mem::rf32(pose + 0x554), ez = mem::rf32(pose + 0x558);
    const double ax = mem::rf32(pose + 0x55c), ay = mem::rf32(pose + 0x560), az = mem::rf32(pose + 0x564);
    const double dx = ex - ax, dy = ey - ay, dz = ez - az;
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(dist) || dist <= 0.1 || dist >= 100.0) return;
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const double y = std::atan2(dx, dz) + kGameYawSign * yaw * kDeg;
    const double p =
        std::clamp(std::asin(std::clamp(dy / dist, -1.0, 1.0)) / kDeg + kGamePitchSign * pitch, min_pitch, max_pitch) * kDeg;
    mem::wf32(pose + 0x550, (float)(ax + dist * std::cos(p) * std::sin(y)));
    mem::wf32(pose + 0x554, (float)(ay + dist * std::sin(p)));
    mem::wf32(pose + 0x558, (float)(az + dist * std::cos(p) * std::cos(y)));
}
}  // namespace

// ---- main camera ----
BOTW_HOOK(0x02b9b8a0) {  // main camera update entry, after stwu: r3 = camera controller
    if (c->r[3] != g_cam) {
        LOG("pce: main camera controller 0x%08x (tick %llu)", c->r[3], (unsigned long long)g_cam_ticks);
        g_cam = c->r[3];
    }
    g_cam_ticks++;
    g_norm_delta = 0;
    feed(false);
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

// ---- zoomed aim camera (scope, bow drawn) ----
BOTW_HOOK(0x02b9250c) {  // update entry, after stwu
    g_bow_cam_ms = now_ms();
    feed(true);
}

BOTW_HOOK(0x02b92848) {  // fmadds f1,f26,f10,f11: f1 = new pitch (degrees) = old + stick delta
    const double p = aim_pitch();
    if (p != 0) set_fs(c, 1, c->f[1] + p);
}

BOTW_HOOK(0x02b92a48) {  // fmuls f1,f27,f8: f1 = yaw delta (degrees, after the frame time factor), base at r1+0x10
    set_fs(c, 1, aim_yaw_abs(c->f[1], mem::rf32(c->r[1] + 0x10)));
}

// ---- rune aim camera (Magnesis, Stasis, Cryonis: not zoomed, normal sensitivity) ----
BOTW_HOOK(0x02b94e30) { feed(false); }  // update entry, after stwu

BOTW_HOOK(0x02b95278) {  // lfs f1,0x134(r31): pitch (degrees) before clamping / normalization
    const double p = aim_pitch();
    if (p != 0) set_fs(c, 1, c->f[1] + p);
}

BOTW_HOOK(0x02b956b0) {  // fmuls f1,f26,f9: yaw delta, base at r1+0x10 (same layout as the scope)
    set_fs(c, 1, aim_yaw_abs(c->f[1], mem::rf32(c->r[1] + 0x10)));
}

// ---- camera rune (the Sheikah Slate's camera; zoomed, aiming sensitivity) ----
BOTW_HOOK(0x02bced3c) {  // update entry, after stwu: r3 = camera; pitch in degrees at +0x28 (game limits -70..75)
    feed(true);
    const double p = kGamePitchSign * take(g_pending_pitch, g_pitch_age);
    const uint32_t cam = c->r[3];
    if (p != 0 && mem::readable(cam + 0x28, 4)) mem::wf32(cam + 0x28, (float)(mem::rf32(cam + 0x28) + p));
}

BOTW_HOOK(0x02bcf698) {  // fmadds f1,f31,f27,f12: f1 = stick x * rate + current yaw (f12), degrees
    // The yaw is taken only while the stick is deflected: the manual-mode stick nudge keeps it so.
    const double stick = (float)(c->f[31] * c->f[27]);
    set_fs(c, 1, (float)c->f[12] + aim_yaw_abs(stick, c->f[12]));
}

// ---- lock-on camera (Z-targeting; its class is also the Magnesis camera) ----
BOTW_HOOK(0x020d72d0) {  // Magnesis action update entry: the rune turns that camera itself (phase 9)
    note_camera();
    g_magnet_ms = now_ms();
}

BOTW_HOOK(0x02bf0234) {  // update entry, after stwu: r3 = camera; yaw +0xAC, pitch +0xA8 (degrees)
    if (now_ms() - g_magnet_ms.load() < 50) return note_camera();
    feed(false);
    const double yaw = take(g_pending_yaw, g_yaw_age);
    const double pitch = take(g_pending_pitch, g_pitch_age);
    const uint32_t cam = c->r[3];
    if ((yaw == 0 && pitch == 0) || !mem::readable(cam, 0xb0)) return;
    mem::wf32(cam + 0xac, (float)wrap180(mem::rf32(cam + 0xac) + kGameYawSign * yaw));
    mem::wf32(cam + 0xa8, std::clamp((float)(mem::rf32(cam + 0xa8) + kGamePitchSign * pitch), -80.0f, 80.0f));
    turn_eye(cam, yaw, pitch, -80.0, 80.0);
}
