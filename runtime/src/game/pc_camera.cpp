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
// Climbing (0x02BEE0F8) / conversation (0x02BCB3A0): yaw / pitch in degrees added before the update. Horse
// (0x02BD2A44) / Master Cycle Zero (0x02BDFB20): the eye of the pose turned around the look-at point.
// Shield surf / sand seal / waterfall: added where the update adds the stick's turn. Magnesis with an object held:
// see its section at the end (the view turns 1:1, the object follows at a limited speed).
// While the mouse turns a camera, the right stick carries a deflection just past the deadzone so the game
// stays in manual camera mode (input::mouse::manual_stick).
//
// BOTW_PCE_SPIN=<degrees per frame> still adds a constant turn (phase 1 hook test).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

#include "../input/mouse.h"
#include "../settings.h"
#include "hooks.h"

extern "C" void rt_call(PPCContext* c, uint32_t target);
namespace pccontrols {
bool owns_mouse();
bool tilt_active();
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

// paraglider: the per-frame update of the player's glide state (Zelda-on-PC PCE_GlideTick, 0x02CB7004 "mr r30,r3")
std::atomic<int64_t> g_glide_ms{-1000000};
bool pce_gliding() { return now_ms() - g_glide_ms.load() < 70; }

// The zoomed aim camera (bow drawn, scope) updated recently: the wheel steps arrows, not weapons.
std::atomic<int64_t> g_bow_cam_ms{-1000000};
bool pce_bow_camera() { return now_ms() - g_bow_cam_ms.load() < 150; }

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
bool magnet_active() { return now_ms() - g_magnet_ms.load() < 100; }
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

// BOTW_PCE_INPUT_LOG: log the set of cameras with mouse support that updated in the last 150 ms whenever it
// changes ("pce camera: horse"), so a play-tester can see which one is turning the view. A set, not the last
// one, since two can update in the same frame.
const bool g_cam_log = [] {
    const char* s = std::getenv("BOTW_PCE_INPUT_LOG");
    return s && *s && *s != '0';
}();
void log_camera(const char* name) {
    constexpr int kMax = 16;
    static const char* names[kMax];
    static int64_t seen[kMax];
    static int count = 0;
    static uint32_t logged = 0;
    const int64_t t = now_ms();
    int i = 0;
    while (i < count && names[i] != name) i++;
    if (i == count) {
        if (count == kMax) return;
        names[count++] = name;
    }
    seen[i] = t;
    uint32_t set = 0;
    for (int k = 0; k < count; k++)
        if (t - seen[k] < 150) set |= 1u << k;
    if (set == logged) return;
    logged = set;
    std::string s;
    for (int k = 0; k < count; k++)
        if (set & (1u << k)) s += (s.empty() ? "" : " + ") + std::string(names[k]);
    LOG("pce camera: %s", s.c_str());
}

// A camera with mouse support updates: take the mouse motion since the last one into the pending rotation.
// name: for the camera log. zoomed: the aiming sensitivity applies (bow drawn, scope, camera rune). gameplay:
// the camera counts as Link being in play (pce_gameplay_camera); not the conversation camera, where mouse
// buttons stay inert.
void feed(const char* name, bool zoomed, bool gameplay = true) {
    if (g_cam_log) log_camera(name);
    if (gameplay) note_camera();
    if (pccontrols::tilt_active()) {  // Motion Control: the mouse tilts the GamePad (pc_controls takes it)
        g_pending_yaw = g_pending_pitch = 0;
        return;
    }
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
BOTW_HOOK(0x02cb7004) {  // mr r30,r3: Link is gliding this frame
    (void)c;
    g_glide_ms.store(now_ms());
}

BOTW_HOOK(0x02b9b8a0) {  // main camera update entry, after stwu: r3 = camera controller
    if (c->r[3] != g_cam) {
        LOG("pce: main camera controller 0x%08x (tick %llu)", c->r[3], (unsigned long long)g_cam_ticks);
        g_cam = c->r[3];
    }
    g_cam_ticks++;
    g_norm_delta = 0;
    feed("main", false);
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
    feed("scope/bow", true);
}

BOTW_HOOK(0x02b92848) {  // fmadds f1,f26,f10,f11: f1 = new pitch (degrees) = old + stick delta
    const double p = aim_pitch();
    if (p != 0) set_fs(c, 1, c->f[1] + p);
}

BOTW_HOOK(0x02b92a48) {  // fmuls f1,f27,f8: f1 = yaw delta (degrees, after the frame time factor), base at r1+0x10
    set_fs(c, 1, aim_yaw_abs(c->f[1], mem::rf32(c->r[1] + 0x10)));
}

// ---- rune aim camera (Magnesis, Stasis, Cryonis: not zoomed, normal sensitivity) ----
BOTW_HOOK(0x02b94e30) { feed("rune aim", false); }  // update entry, after stwu

BOTW_HOOK(0x02b95278) {  // lfs f1,0x134(r31): pitch (degrees) before clamping / normalization
    const double p = aim_pitch();
    if (p != 0) set_fs(c, 1, c->f[1] + p);
}

BOTW_HOOK(0x02b956b0) {  // fmuls f1,f26,f9: yaw delta, base at r1+0x10 (same layout as the scope)
    set_fs(c, 1, aim_yaw_abs(c->f[1], mem::rf32(c->r[1] + 0x10)));
}

// ---- camera rune (the Sheikah Slate's camera; zoomed, aiming sensitivity) ----
BOTW_HOOK(0x02bced3c) {  // update entry, after stwu: r3 = camera; pitch in degrees at +0x28 (game limits -70..75)
    feed("camera rune", true);
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
BOTW_HOOK(0x02bf0234) {  // update entry, after stwu: r3 = camera; yaw +0xAC, pitch +0xA8 (degrees)
    if (magnet_active()) return note_camera();  // Magnesis turns this camera itself (0x02BF02A8)
    feed("lock-on", false);
    const double yaw = take(g_pending_yaw, g_yaw_age);
    const double pitch = take(g_pending_pitch, g_pitch_age);
    const uint32_t cam = c->r[3];
    if ((yaw == 0 && pitch == 0) || !mem::readable(cam, 0xb0)) return;
    mem::wf32(cam + 0xac, (float)wrap180(mem::rf32(cam + 0xac) + kGameYawSign * yaw));
    mem::wf32(cam + 0xa8, std::clamp((float)(mem::rf32(cam + 0xa8) + kGamePitchSign * pitch), -80.0f, 80.0f));
    turn_eye(cam, yaw, pitch, -80.0, 80.0);
}

// ---- orbit cameras with their own classes (called by the camera manager instead of the chase camera) ----
namespace {
// Climbing and conversation keep yaw / pitch in degrees (stick right / up lower them) and add the stick to them:
// the mouse rotation is added right before the update, which normalises and clamps them itself.
void orbit_tick(PPCContext* c, const char* name, uint32_t yaw_off, uint32_t pitch_off, bool gameplay) {
    feed(name, false, gameplay);
    const double yaw = take(g_pending_yaw, g_yaw_age);
    const double pitch = take(g_pending_pitch, g_pitch_age);
    const uint32_t cam = c->r[3];
    if ((yaw == 0 && pitch == 0) || !mem::readable(cam, 0x100)) return;
    mem::wf32(cam + yaw_off, (float)(mem::rf32(cam + yaw_off) + kGameYawSign * yaw));
    mem::wf32(cam + pitch_off, std::clamp((float)(mem::rf32(cam + pitch_off) + kGamePitchSign * pitch), -89.0f, 89.0f));
}

// Horse and Master Cycle take yaw / pitch from their pose every frame: the eye is turned around the look-at
// point before the update (game pitch limits -40..60). The manual-mode stick nudge keeps the camera from
// swinging back behind the mount, as a touch of the stick does.
void mount_tick(PPCContext* c, const char* name) {
    feed(name, false);
    const double yaw = take(g_pending_yaw, g_yaw_age);
    const double pitch = take(g_pending_pitch, g_pitch_age);
    turn_eye(c->r[3], yaw, pitch, -40.0, 60.0);
}
}  // namespace

BOTW_HOOK(0x02bee0f8) { orbit_tick(c, "climbing", 0x98, 0x8c, true); }  // climbing (walls, ladders): game clamps pitch +-70
BOTW_HOOK(0x02bcb3a0) { orbit_tick(c, "conversation", 0x48, 0x40, false); }  // conversation: game clamps pitch -40..60
BOTW_HOOK(0x02bd2a44) { mount_tick(c, "horse"); }  // horse
BOTW_HOOK(0x02bdfb20) { mount_tick(c, "Master Cycle"); }  // Master Cycle Zero

// ---- shield surf, sand seal, Zora waterfall ----
// Their cameras turn by the stick only while it is deflected (the manual-mode stick nudge keeps them so while the
// mouse moves); the mouse rotation is added where the update adds the stick's turn. When the stick is let go they
// ease back behind Link, as with the GamePad.
// Shield surf 0x02BE8B30: state byte +0x1C2 is 5 while the stick is deflected; pitch +0xA4 read at 0x02BE8EF8 (r29 =
// camera, used in that state only), yaw at 0x02BE9698 (f1 = current yaw f30 + stick turn f0, degrees).
// Sand seal 0x02BF5ACC: the same kind of camera, state +0x1EA == 4, pitch +0xA0 at 0x02BF5E80 (r30), yaw 0x02BF65E8.
// Waterfall 0x02BF8F88: yaw only, at 0x02BF914C (f1 = current yaw f31 + stick turn f30).
namespace {
void surf_pitch(PPCContext* c, uint32_t cam, uint32_t state_off, uint8_t manual_state) {
    if (!mem::readable(cam + state_off, 1) || mem::r8(cam + state_off) != manual_state) return;
    const double p = take(g_pending_pitch, g_pitch_age);
    if (p != 0) set_fs(c, 1, std::clamp(c->f[1] + kGamePitchSign * p, -80.0, 80.0));
}
void surf_yaw(PPCContext* c) {
    const double y = take(g_pending_yaw, g_yaw_age);
    if (y != 0) set_fs(c, 1, c->f[1] + kGameYawSign * y);
}
}  // namespace

BOTW_HOOK(0x02be8b30) { feed("shield surf", false); }                          // shield surf update entry
BOTW_HOOK(0x02be8ef8) { surf_pitch(c, c->r[29], 0x1c2, 5); }    // lfs f1,0xa4(r29)
BOTW_HOOK(0x02be9698) { surf_yaw(c); }                          // fadds f1,f30,f0
BOTW_HOOK(0x02bf5acc) { feed("sand seal", false); }                          // sand seal update entry
BOTW_HOOK(0x02bf5e80) { surf_pitch(c, c->r[30], 0x1ea, 4); }    // lfs f1,0xa0(r30)
BOTW_HOOK(0x02bf65e8) { surf_yaw(c); }                          // fadds f1,f27,f8
BOTW_HOOK(0x02bf8f88) {  // waterfall update entry: no pitch control on this camera
    feed("waterfall", false);
    g_pending_pitch = 0;
}
BOTW_HOOK(0x02bf914c) { surf_yaw(c); }                          // fadds f1,f31,f30

// ---- Magnesis with an object held ----
// No camera reads the stick here: the rune's action (update 0x020D72D0) keeps where the object is held around Link:
// direction +0xA8 (radians), height +0xA4 and distance +0xA0 (metres); the stick turns the direction by 60 degrees
// per second at most. The camera (the lock-on class, update 0x02BF0234) frames Link and the object: it asks where the
// object is (0x02BD8A30: three copies of the position at camera +0x40 / +0x4C / +0x58), follows its direction with a
// dead zone of 5 degrees (smoothed direction at camera +0x90, degrees) and eases its eye / look-at point after that.
// With the mouse the view turns at once and the object swings after it:
//  - the camera rig (eye, look-at point, smoothed direction) is turned by the mouse around Link (pose +0x7D4) before
//    the camera's own update (0x02BF02A8), so the view follows the mouse 1:1;
//  - the action's direction follows the view at a limited speed (kMagnetFollow*): an object far behind its direction
//    is taken for a stuck one, pulled closer and dropped;
//  - the object position given to the camera is turned by what the object still has to do to reach the view (lead,
//    0x02BD8AB8), so the camera's smoothing sees nothing to correct and does not pull the view back;
//  - the game shortens the distance when the object is more than 1 m nearer than it is held at, measured to a
//    smoothed position of the object that cuts the corner on quick turns; while the mouse turns the object that
//    measurement is not allowed below the distance to where the object really is (0x020D6DB0).
// Up / down moves the object by the same angle as seen from Link (the game limits the height).
namespace {
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr double kMagnetFollowDeg = 360.0;    // degrees per second the object may follow the view at most
constexpr double kMagnetFollowMetres = 40.0;  // ... and metres per second (less angle for a far object)
constexpr uint32_t kMagnetStatePtr = 0x10467be0;  // the game's magnet state (object positions +0xB0, index +0x11C)

struct Magnet {
    double rig_yaw = 0;        // degrees the view was turned by the mouse and the camera rig not yet
    double backlog_yaw = 0;    // degrees the view is ahead of the action's direction
    double backlog_pitch = 0;  // the same for up / down
    double direction = 0;      // the action's direction (degrees) as the last update left it
    double pending = 0;        // change of the view's direction the camera has not been told about yet
    double lead = 0;           // degrees the view is ahead of the object, as the camera sees it
    double object_yaw = 0;     // direction of the object from Link when the camera last asked
    bool has_direction = false, has_object = false;
    double last_t = 0;
    double last_turn_t = -1e9;  // when the mouse last turned the action's direction
    uint32_t pose = 0;          // pose of the camera in the update that is running
} g_mag;

double secs() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// Turns a point (x, y, z floats) around a vertical axis; angles grow from +z towards +x like atan2(x, z).
void turn_around(uint32_t point, double cx, double cz, double degrees) {
    const double a = degrees * kDegToRad, co = std::cos(a), si = std::sin(a);
    const double x = mem::rf32(point) - cx, z = mem::rf32(point + 8) - cz;
    mem::wf32(point, (float)(cx + x * co + z * si));
    mem::wf32(point + 8, (float)(cz + z * co - x * si));
}
}  // namespace

BOTW_HOOK(0x020d72d0) {  // Magnesis action update entry, after stwu: r3 = action
    const int64_t t_ms = now_ms();
    const bool resumed = t_ms - g_magnet_ms.exchange(t_ms) > 100;
    feed("Magnesis", false);
    const double yaw = kGameYawSign * take(g_pending_yaw, g_yaw_age);
    const double pitch = take(g_pending_pitch, g_pitch_age);
    const uint32_t action = c->r[3];
    if (!mem::readable(action, 0xb0)) return;
    const double now = secs();
    if (resumed) {
        g_mag = {};
        g_mag.last_t = now;
    }
    const double dt = std::clamp(now - g_mag.last_t, 1.0 / 240.0, 1.0 / 20.0);
    g_mag.last_t = now;
    const double distance = std::clamp((double)mem::rf32(action + 0xa0), 1.0, 100.0);
    const double max_step = std::min(kMagnetFollowDeg, kMagnetFollowMetres / distance / kDegToRad) * dt;

    // what the stick turned in the last update belongs to the view as well
    const double before = mem::rf32(action + 0xa8) / kDegToRad;
    const double stick = g_mag.has_direction ? before - g_mag.direction : 0.0;
    g_mag.backlog_yaw = wrap180(g_mag.backlog_yaw + yaw);
    const double step_yaw = std::clamp(g_mag.backlog_yaw, -max_step, max_step);
    g_mag.backlog_yaw -= step_yaw;
    if (step_yaw != 0) {
        mem::wf32(action + 0xa8, (float)((before + step_yaw) * kDegToRad));
        g_mag.last_turn_t = now;
    }
    g_mag.direction = mem::rf32(action + 0xa8) / kDegToRad;
    g_mag.has_direction = true;
    g_mag.pending += yaw + stick;
    g_mag.rig_yaw += yaw;

    g_mag.backlog_pitch = std::clamp(g_mag.backlog_pitch + pitch, -60.0, 60.0);
    const double step_pitch = std::clamp(g_mag.backlog_pitch, -max_step, max_step);
    g_mag.backlog_pitch -= step_pitch;
    if (step_pitch != 0)
        mem::wf32(action + 0xa4, mem::rf32(action + 0xa4) + (float)(distance * std::tan(step_pitch * kDegToRad)));
}

BOTW_HOOK(0x02bf02a8) {  // or. r27,r3,r3 in the camera update: r3 = camera pose (0 = none), r29 = camera
    const uint32_t pose = c->r[3], cam = c->r[29];
    const bool active = magnet_active() && pose != 0 && mem::readable(pose + 0x550, 0x290) && mem::readable(cam, 0x94);
    const double turn = g_mag.rig_yaw;
    g_mag.rig_yaw = 0;
    g_mag.pose = active ? pose : 0;
    if (!active || turn == 0) return;
    const double cx = mem::rf32(pose + 0x7d4), cz = mem::rf32(pose + 0x7dc);
    turn_around(pose + 0x550, cx, cz, turn);
    turn_around(pose + 0x55c, cx, cz, turn);
    mem::wf32(cam + 0x90, (float)wrap180(mem::rf32(cam + 0x90) + turn));
}

BOTW_HOOK(0x02bd8ab8) {  // lwz r0,0x14(r1): the camera (r31) has just been given the object's position
    const uint32_t cam = c->r[31], pose = g_mag.pose;
    if (pose == 0 || !mem::readable(cam, 0x64)) return;
    const double cx = mem::rf32(pose + 0x7d4), cz = mem::rf32(pose + 0x7dc);
    const double object_yaw = std::atan2(mem::rf32(cam + 0x58) - cx, mem::rf32(cam + 0x60) - cz) / kDegToRad;
    // what the view has turned since the last time, less what the object has done of it
    g_mag.lead = wrap180(g_mag.lead + g_mag.pending - (g_mag.has_object ? wrap180(object_yaw - g_mag.object_yaw) : 0.0));
    g_mag.pending = 0;
    g_mag.object_yaw = object_yaw;
    g_mag.has_object = true;
    if (std::fabs(g_mag.lead) < 0.001) g_mag.lead = 0;
    if (g_mag.lead == 0) return;
    for (const uint32_t copy : {cam + 0x40, cam + 0x4c, cam + 0x58}) turn_around(copy, cx, cz, g_mag.lead);
}

BOTW_HOOK(0x020d6db0) {  // fsubs f11,f1,f29 after bl 0x03C6FD44: f1 = distance to the object's smoothed position
    // Link's position at r1+0x38. While the mouse turns the object, measure no shorter than to where it really is.
    if (!magnet_active() || secs() - g_mag.last_turn_t >= 1.0 || !mem::readable(kMagnetStatePtr, 4)) return;
    const uint32_t state = mem::r32(kMagnetStatePtr), link = c->r[1] + 0x38;
    if (state == 0 || !mem::readable(state, 0x120) || !mem::readable(link, 12)) return;
    const uint32_t index = mem::r32(state + 0x11c);
    const uint32_t object = state + 0xb0 + (index < 2 ? index * 0xc : 0);
    const double dx = (double)mem::rf32(object) - mem::rf32(link), dz = (double)mem::rf32(object + 8) - mem::rf32(link + 8);
    const double real = std::sqrt(dx * dx + dz * dz);
    if (real > c->f[1] && real < 200.0) {
        set_fs(c, 1, real);
        set_fs(c, 11, (float)c->f[1] - (float)c->f[29]);
    }
}
