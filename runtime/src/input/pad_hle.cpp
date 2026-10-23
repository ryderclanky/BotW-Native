// vpad / vpadbase / padscore HLE. The GamePad (VPAD channel 0) is fed from input::gamepad();
// no Wii Remotes / Pro Controllers are reported (WPADProbe -> no controller).
//
// VPADSetSamplingCallback stores a function the console calls at the pad sample rate (200 Hz,
// channel in r3). BotW registers one. It is invoked on its own guest thread; VPADRead from the
// game's frame loop is unchanged.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <thread>

#include "../hle.h"
#include "../log.h"
#include "../thread.h"
#include "input.h"
#include "title_diag.h"

namespace {
// Face, shoulder, d-pad, stick-click and system buttons. Stick-emulation bits are
// excluded so a noisy axis cannot keep a real button's edge latched.
constexpr uint32_t kEdgeMask = 0x0007FFFFu;
constexpr uint32_t kRepeatBit = 0x80000000u;
// Absent finger. 0 would mean "valid sample"; a message window treats a valid
// touch as the finger that advances text and then waits for that finger to lift.
constexpr uint16_t kTouchInvalid = 3;  // VPAD_TP_VALIDITY_INVALID_X | INVALID_Y
constexpr int kEdgeLatchMs = 40;

std::mutex g_edge_lock;
uint32_t g_prev_buttons = 0;
uint32_t g_trig_latch = 0;
uint32_t g_rel_latch = 0;
uint32_t g_stick_trig = 0;
uint32_t g_stick_rel = 0;
uint64_t g_trig_until = 0;
uint64_t g_rel_until = 0;
uint64_t g_stick_until = 0;
float g_repeat_delay_ms = 0;
float g_repeat_pulse_ms = 0;  // 0: the game has not asked for repeat
uint64_t g_repeat_next = 0;
bool g_repeat_pending = false;
pthread_t g_sampling_tid{};
std::atomic<bool> g_sampling_ready{false};

std::atomic<uint32_t> g_sampling_cb{0};
std::once_flag g_sampling_once;

uint64_t edge_now_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// True on the 200 Hz VPAD sampling thread. That thread calls VPADRead from the
// guest callback and used to consume the one-sample trigger before the game
// frame's message window could see it. Keyboard and controller both arrive as
// the same hold bits, so a tap of E (or A on a pad) never reached the text box.
bool this_is_sampling_thread() {
    return g_sampling_ready.load(std::memory_order_acquire) && pthread_equal(pthread_self(), g_sampling_tid);
}

// hold/trigger/release for one VPADRead. A press stays latched until the game
// thread reads it, or for kEdgeLatchMs, whichever comes first. The sampling
// thread observes the latch and does not clear it. A release does not clear
// the trigger: a short tap is often already up when the message window reads.
void button_edges(uint32_t hold, uint32_t* out_hold, uint32_t* out_trig, uint32_t* out_rel, bool consume) {
    std::lock_guard<std::mutex> lock(g_edge_lock);
    uint64_t now = edge_now_ms();
    uint32_t rose = (hold & ~g_prev_buttons) & kEdgeMask;
    uint32_t fell = (g_prev_buttons & ~hold) & kEdgeMask;
    uint32_t other_rose = (hold & ~g_prev_buttons) & ~kEdgeMask;
    uint32_t other_fell = (g_prev_buttons & ~hold) & ~kEdgeMask;
    if (rose) {
        g_trig_latch |= rose;
        g_trig_until = now + kEdgeLatchMs;
    }
    if (fell) {
        g_rel_latch |= fell;
        g_rel_until = now + kEdgeLatchMs;
    }
    if (other_rose) {
        g_stick_trig |= other_rose;
        g_stick_until = now + kEdgeLatchMs;
    }
    if (other_fell) {
        g_stick_rel |= other_fell;
        g_stick_until = now + kEdgeLatchMs;
    }
    if (now >= g_trig_until) g_trig_latch = 0;
    if (now >= g_rel_until) g_rel_latch = 0;
    if (now >= g_stick_until) g_stick_trig = g_stick_rel = 0;
    bool buttons_changed = ((hold ^ g_prev_buttons) & kEdgeMask) != 0;
    g_prev_buttons = hold;

    uint32_t reported = hold;
    if (g_repeat_pulse_ms > 0 && (hold & kEdgeMask)) {
        if (buttons_changed || g_repeat_next == 0) g_repeat_next = now + (uint64_t)g_repeat_delay_ms;
        if (now >= g_repeat_next) {
            g_repeat_pending = true;
            g_repeat_next = now + (uint64_t)g_repeat_pulse_ms;
        }
    } else {
        g_repeat_next = 0;
        g_repeat_pending = false;
    }
    if (g_repeat_pending) reported |= kRepeatBit;
    *out_hold = reported;
    *out_trig = g_trig_latch | g_stick_trig;
    *out_rel = g_rel_latch | g_stick_rel;
    if (!consume) return;
    g_trig_latch = 0;
    g_rel_latch = 0;
    g_trig_until = 0;
    g_rel_until = 0;
    g_stick_trig = 0;
    g_stick_rel = 0;
    g_stick_until = 0;
    g_repeat_pending = false;
}

void set_button_repeat(float delay_sec, float pulse_sec) {
    std::lock_guard<std::mutex> lock(g_edge_lock);
    if (pulse_sec <= 0) {
        g_repeat_delay_ms = 0;
        g_repeat_pulse_ms = 0;
        g_repeat_next = 0;
        g_repeat_pending = false;
        return;
    }
    g_repeat_delay_ms = delay_sec * 1000.f;
    g_repeat_pulse_ms = pulse_sec * 1000.f;
    g_repeat_next = 0;
}

void sampling_main() {
    g_sampling_tid = pthread_self();
    g_sampling_ready.store(true, std::memory_order_release);
    threads::GuestThread* t = threads::adopt_host_thread("VPAD", 0x10000, 1);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        uint32_t cb = g_sampling_cb.load(std::memory_order_acquire);
        if (!cb) continue;
        guest_call(&t->ctx, cb, {0u});
        title_diag_vpad_invoked();
    }
}

void start_sampling_thread() {
    std::call_once(g_sampling_once, [] {
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        int r = pthread_create(
            &th, &attr, [](void*) -> void* {
                sampling_main();
                return nullptr;
            },
            nullptr);
        pthread_attr_destroy(&attr);
        if (r != 0) {
            LOG("VPAD: sampling thread failed to start (%d)", r);
            std::thread(sampling_main).detach();
            return;
        }
        pthread_detach(th);
    });
}

void wf(uint32_t a, float v) { mem::wf32(a, v); }
}  // namespace

// int32 VPADRead(chan, VPADStatus* buf, uint32 count, int32* err)
HLE_EXPORT(vpad, VPADRead) {
    uint32_t chan = ARG(0), buf = ARG(1), count = ARG(2), err = ARG(3);
    if (chan != 0 || !count) {
        if (err) mem::w32(err, (uint32_t)-2);
        title_diag_vpad_read(0, 0);
        RET(0);
        return;
    }
    input::PadState s = input::gamepad();
    mem::commit(buf, 0xAC, true);
    memset(mem::host(buf), 0, 0xAC);
    uint32_t hold = s.buttons;
    // stick emulation bits (games use them for menu navigation)
    auto stick_bits = [](float x, float y, uint32_t base) {
        uint32_t b = 0;
        if (x < -0.5f) b |= base << 3;   // LEFT
        if (x > 0.5f) b |= base << 2;    // RIGHT
        if (y > 0.5f) b |= base << 1;    // UP
        if (y < -0.5f) b |= base;        // DOWN
        return b;
    };
    hold |= stick_bits(s.lx, s.ly, 0x08000000) | stick_bits(s.rx, s.ry, 0x00800000);
    uint32_t trig = 0, rel = 0;
    button_edges(hold, &hold, &trig, &rel, !this_is_sampling_thread());
    mem::w32(buf + 0x00, hold);
    mem::w32(buf + 0x04, trig);
    mem::w32(buf + 0x08, rel);
    wf(buf + 0x0C, s.lx);
    wf(buf + 0x10, s.ly);
    wf(buf + 0x14, s.rx);
    wf(buf + 0x18, s.ry);
    wf(buf + 0x30, 1.0f);  // accXY
    input::Motion m;
    if (input::motion(&m)) {  // controller gyro / accelerometer
        for (int k = 0; k < 3; k++) {
            wf(buf + 0x1C + 4 * k, m.acc[k]);
            wf(buf + 0x38 + 4 * k, m.gyro_change[k]);
            wf(buf + 0x44 + 4 * k, m.orientation[k]);
        }
        wf(buf + 0x28, m.acc_magnitude);
        wf(buf + 0x2C, m.acc_acceleration);
        for (int k = 0; k < 9; k++) wf(buf + 0x6C + 4 * k, m.dir[k]);
    } else {
        wf(buf + 0x20, -1.0f);  // acc: pad lying flat
        wf(buf + 0x28, 1.0f);   // acc magnitude
        // dir: identity orientation matrix (x, y, z axes)
        wf(buf + 0x6C, 1.0f);
        wf(buf + 0x7C, 1.0f);
        wf(buf + 0x8C, 1.0f);
    }
    // Touch blocks sit at 0x52, 0x5A and 0x62 (x, y, touched, validity). A zero
    // validity means the point is valid. The message window then treats (0, 0)
    // as a finger and ignores the A press it is waiting on.
    for (uint32_t off : {0x52u, 0x5Au, 0x62u}) mem::w16(buf + off + 6, kTouchInvalid);
    mem::w8(buf + 0xA0, 0x40);  // slide volume
    mem::w8(buf + 0xA1, 6);     // battery full
    if (err) mem::w32(err, 0);
    title_diag_vpad_read(1, hold);
    RET(1);
}
// int32 VPADControlMotor(chan, uint8* pattern, uint8 lengthInBits): rumble, see input::push_rumble
HLE_EXPORT(vpad, VPADControlMotor) {
    uint32_t bits = std::min<uint32_t>(ARG(2) & 0xFF, 120);
    if (ARG(0) != 0 || !ARG(1) || !bits) {
        input::clear_rumble();
        RET(0);
        return;
    }
    RET(input::push_rumble(mem::host(ARG(1)), (int)bits) ? 0 : (uint32_t)-1);
}
HLE_EXPORT(vpad, VPADStopMotor) { input::clear_rumble(); }
HLE_EXPORT(vpad, VPADDisableGyroDirRevise) {}
HLE_EXPORT(vpad, VPADEnableGyroAccRevise) {}
HLE_EXPORT(vpad, VPADSetGyroAccReviseParam) {}
HLE_EXPORT(vpad, VPADSetGyroDirection) {}
HLE_EXPORT(vpad, VPADSetGyroZeroDriftMode) {}
// void VPADSetBtnRepeat(chan, float delaySec, float pulseSec)
// Holding A in a text box pulses VPAD_BUTTON_REPEAT; the window advances on that too.
HLE_EXPORT(vpad, VPADSetBtnRepeat) {
    float delay = (float)ARGF(0);
    float pulse = (float)ARGF(1);
    static float seen_delay = -1.f, seen_pulse = -1.f;
    if (delay != seen_delay || pulse != seen_pulse) {
        seen_delay = delay;
        seen_pulse = pulse;
        LOG("VPAD button repeat: delay %.0f ms, pulse %.0f ms", delay * 1000.f, pulse > 0 ? pulse * 1000.f : 0.f);
    }
    set_button_repeat(delay, pulse);
}
HLE_EXPORT(vpad, VPADSetSamplingCallback) {
    uint32_t next = ARG(1);
    uint32_t prev = g_sampling_cb.exchange(next, std::memory_order_acq_rel);
    title_diag_vpad_callback(next);
    if (next) start_sampling_thread();
    RET(prev);
}
// void VPADGetTPCalibratedPoint(chan, VPADTPData* out, const VPADTPData* in)
HLE_EXPORT(vpad, VPADGetTPCalibratedPoint) {
    mem::commit(ARG(1), 8, true);
    mem::commit(ARG(2), 8, false);
    memmove(mem::host(ARG(1)), mem::host(ARG(2)), 8);
}
HLE_EXPORT(vpadbase, VPADBASEGetHeadphoneStatus) { RET(0); }

// ---- padscore: no remotes connected ---------------------------------------------------------------
HLE_EXPORT(padscore, KPADInitEx) {}
HLE_EXPORT(padscore, KPADGetMplsWorkSize) { RET(0x5FE0); }
HLE_EXPORT(padscore, KPADSetMplsWorkarea) {}
// int32 KPADReadEx(chan, KPADStatus* buf, uint32 count, int32* err)
HLE_EXPORT(padscore, KPADReadEx) {
    if (ARG(3)) mem::w32(ARG(3), (uint32_t)-1);  // KPAD_READ_ERR_NO_CONTROLLER
    RET(0);
}
// int32 WPADProbe(chan, uint32* type)
HLE_EXPORT(padscore, WPADProbe) {
    if (ARG(1)) mem::w32(ARG(1), 253);  // WPAD_DEV_NOT_FOUND
    RET((uint32_t)-1);                  // WPAD_ERR_NO_CONTROLLER
}
HLE_EXPORT(padscore, WPADControlMotor) {}
HLE_EXPORT(padscore, WPADControlSpeaker) { RET((uint32_t)-1); }
HLE_EXPORT(padscore, WPADCanSendStreamData) { RET(0); }
HLE_EXPORT(padscore, WPADSendStreamData) { RET((uint32_t)-1); }
HLE_EXPORT(padscore, WPADStartSyncDevice) { RET(0); }
HLE_EXPORT(padscore, WPADEnableURCC) {}
HLE_EXPORT(padscore, WENCGetEncodeData) { RET(0); }
