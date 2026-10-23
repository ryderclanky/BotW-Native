// 60 fps (--fps 60). Finding and approach credit: Cemu's FPS++ pack (Xalphenos, rajkosto, epigramx,
// Crementif, leoetlino, Pig, Exzap); this is an independent host-side implementation of the same idea.
//
// - Pacing: every frame the GPU waits (GX2SetGPUFence) until the vblank counter, bumped by the game's
//   vsync callback, reaches last frame + interval. The interval is 2 vblanks (30 fps); use 1.
// - Speed: the engine's frame delta (VFR) and UI step assume the interval. Replace them with the
//   measured frame time in 1/30 s units (g_delta: 1.0 at 30 fps, 0.5 at 60), averaged over a few frames.
// - A few systems use hardcoded 30 fps constants; scale those by g_delta too.
// - Some cutscenes are known to crash above 30 fps; hold those at 30.
#include <algorithm>
#include <cstring>

#include "hooks.h"

namespace {
constexpr int kAvgFrames = 8;
constexpr double kTicksPerSecond = 62156250.0;  // OSGetSystemTime
// 1/30 s units. Read only while fps60() is on, so the first UI step is one 60 fps frame
// before the measured average exists.
float g_delta = 0.5f;
double g_dt[kAvgFrames];
int g_dt_n = 0;
int g_span_logs = 0;
int g_clamp_logs = 0;
bool g_cap30 = false;      // a cutscene that must run at 30 fps is playing
uint32_t g_cap_event = 0;  // its event id
bool fps60() { return g_target_fps >= 60; }

void data_patches() {
    mem::wf32(0x100775ac, -999.0f);  // Thunderblight Ganon gets thrown off the arena at high fps
    mem::wf32(0x1031e2c0, 2.0f);     // audio constant (1.0), as FPS++ sets it
}
}  // namespace

// ---- pacing --------------------------------------------------------------------------------------

// lwz r0, 904(r31): frame interval in vblanks, used for the GX2SetGPUFence value
BOTW_HOOK(0x031faae8) {
    if (fps60()) c->r[0] = g_cap30 ? 2 : 1;
}

// The framework's waitForVBlank (0x031facc4) loops { GX2WaitForVsync; GX2GetSwapStatus } until every
// swap has flipped, so a frame that misses a vblank waits for the next one (30 fps). At 60 fps pace
// with the GPU fence only, so slow frames show at e.g. 45 fps instead of dropping to 30.
BOTW_HOOK(0x031faccc) {  // stwu r1, -32(r1) on entry
    if (fps60() && !g_cap30) g_skip_vsync_wait = true;
}
BOTW_HOOK(0x031facec) {  // lwz r0, 8(r1): swap count, compared against the flip count in r12 next
    if (fps60() && !g_cap30) c->r[12] = c->r[0];
}

// stw r0, 120(r30) in the framework's frame function: r0:r12 = OSGetSystemTime ticks since last frame.
// The high word is already stored. The next instruction stores r12 at +124, so the low word is
// updated through the register. A boot frame of several seconds (last timestamp still 0) steps the
// title timeline once, which skips the logo fade-in, the TitleBG scene and the BGM cue and leaves
// the idle "(A) Start" panes. The same cap is the 20 fps floor g_delta already applies (1.5).
BOTW_HOOK(0x031fa97c) {
    uint64_t ticks = ((uint64_t)c->r[0] << 32) | c->r[12];
    const uint64_t kMaxStep = (uint64_t)(kTicksPerSecond / 20.0);
    const bool clamp = ticks > kMaxStep;
    if (g_span_logs < 4) {
        g_span_logs++;
        LOG("frame span %.3fs%s", (double)ticks / kTicksPerSecond, clamp ? " (clamped to 50 ms)" : "");
    } else if (clamp && g_clamp_logs < 8) {
        g_clamp_logs++;
        LOG("frame span %.3fs clamped to 50 ms", (double)ticks / kTicksPerSecond);
    }
    if (clamp) {
        ticks = kMaxStep;
        c->r[0] = (uint32_t)(ticks >> 32);
        c->r[12] = (uint32_t)ticks;
        mem::w32(c->r[30] + 120, c->r[0]);
    }
    if (!fps60()) return;
    if (g_dt_n == 0) data_patches();
    g_dt[g_dt_n++ % kAvgFrames] = ticks / kTicksPerSecond;
    int n = std::min(g_dt_n, kAvgFrames);
    double sum = 0;
    for (int k = 0; k < n; k++) sum += g_dt[k];
    // never faster than 60 fps or slower than 20 fps (below that the game slows down, as on hardware)
    g_delta = (float)std::clamp(sum / n * 30.0, 0.5, 1.5);
}

// ---- game speed ----------------------------------------------------------------------------------

// VFR delta: fdivs f0, f12, f13 (interval / base) before it is stored to +28 and +40
BOTW_HOOK(0x03793378) {
    if (fps60()) c->f[0] = g_delta;
}
// VFR delta when the interval did not change: lfs f0, 28(r30) (cached value)
BOTW_HOOK(0x0379339c) {
    if (!fps60()) return;
    c->f[0] = g_delta;
    mem::wf32(c->r[30] + 28, g_delta);
}
// UI (layout animation) step: fmuls f0, f13, f0 = interval * 0.5
BOTW_HOOK(0x03415c2c) {
    if (fps60()) c->f[0] = g_delta;
}

// ---- hardcoded 30 fps constants (each hook follows the lfs that loads it) --------------------------

BOTW_HOOK(0x02d90d30) {  // stamina regen: 30.0
    if (fps60()) c->f[11] = 30.0 / g_delta;
}
BOTW_HOOK(0x02d90d8c) {  // stamina regen: 30.0
    if (fps60()) c->f[11] = 30.0 / g_delta;
}
BOTW_HOOK(0x037dc360) {  // audio looping: 1.0
    if (fps60()) c->f[13] = g_delta;
}
BOTW_HOOK(0x02f62b40) {  // map scrolling: 1.0
    if (fps60()) c->f[0] = g_delta;
}
BOTW_HOOK(0x020e551c) {  // arrow speed: 30.0 (inverse)
    if (fps60()) c->f[11] = 30.0 * g_delta;
}
BOTW_HOOK(0x020e731c) {  // arrow speed: 30.0 (inverse)
    if (fps60()) c->f[13] = 30.0 * g_delta;
}
BOTW_HOOK(0x020e3eb8) {  // arrow bounce / distance: 0.5
    if (fps60()) c->f[13] = 0.5 / g_delta;
}
BOTW_HOOK(0x020e5398) {  // distance arrows fly straight: 0.5
    if (fps60()) c->f[13] = 0.5 / g_delta;
}
BOTW_HOOK(0x033609e0) {  // shield surfing: 1.0
    if (fps60()) c->f[0] = g_delta;
}
BOTW_HOOK(0x02b951f0) {  // vertical sensitivity while using runes: 1.5
    if (fps60()) c->f[11] = 1.5 * g_delta;
}
BOTW_HOOK(0x02b95240) {  // vertical gyro sensitivity while using runes: 30.0
    if (fps60()) c->f[9] = 30.0 / g_delta;
}
BOTW_HOOK(0x02c6b8f4) {  // flurry rush dash distance (compared against 1.0 next)
    if (fps60()) c->f[13] = (float)(c->f[13] / g_delta);
}
BOTW_HOOK(0x02d5f760) {  // stamina drain during arrow time
    if (fps60()) c->f[1] = (float)(c->f[1] * g_delta);
}
BOTW_HOOK(0x0209ffc0) {  // Master Cycle Zero boost speed: 0.9
    if (fps60()) c->f[0] = (float)(c->f[0] * g_delta);
}

// ---- cutscenes that must run at 30 fps -------------------------------------------------------------

// evt::EventFlowBase ctor, lwz r12, 8(r29): name of the event being created; r9 = its id
BOTW_HOOK(0x031d2d2c) {
    if (!fps60() || !c->r[12]) return;
    const char* n = mem::str(c->r[12]);
    if (strncmp(n + 1, "emo", 3) != 0) return;  // "Demo123_4"
    int id = (n[4] - '0') * 1000 + (n[5] - '0') * 100 + (n[6] - '0') * 10 + (n[8] - '0');
    bool cap = id == 3480 || id == 3481     // Rito Village bridge crash
               || id == 1620 || id == 1630 || id == 3340  // end of Vah Ruta crash
               || id == 1690                // intro: Zelda's voice on the black screen
               || (id >= 1100 && id <= 1250 && id % 10 == 0);  // divine beast enter / exit
    if (cap) {
        g_cap30 = true;
        g_cap_event = c->r[9];
    }
}
// evt::EventFlowBase dtor, lwz r11, 636(r30)
BOTW_HOOK(0x031d3798) {
    if (g_cap30 && mem::r32(c->r[30] + 0x274) == g_cap_event) g_cap30 = false;
}
