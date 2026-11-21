// Controller state shared between the host event loop (window.cpp) and VPAD/KPAD HLE.
#pragma once
#include <stdint.h>

namespace input {
// VPAD button bits (Wii U GamePad)
enum : uint32_t {
    BTN_SYNC = 0x0001, BTN_HOME = 0x0002, BTN_MINUS = 0x0004, BTN_PLUS = 0x0008, BTN_R = 0x0010, BTN_L = 0x0020,
    BTN_ZR = 0x0040, BTN_ZL = 0x0080, BTN_DOWN = 0x0100, BTN_UP = 0x0200, BTN_RIGHT = 0x0400, BTN_LEFT = 0x0800,
    BTN_Y = 0x1000, BTN_X = 0x2000, BTN_B = 0x4000, BTN_A = 0x8000, BTN_TV = 0x10000, BTN_STICK_R = 0x20000,
    BTN_STICK_L = 0x40000,
};
struct PadState {
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, +y = up
};
PadState gamepad();               // current state (thread-safe snapshot)
void set_gamepad(const PadState&);  // from the host event loop / test script

// GamePad rumble (VPADControlMotor): pattern bits at 120 Hz, played back as 60 Hz on/off steps (as Cemu)
bool push_rumble(const uint8_t* pattern, int bits);  // false when too much is queued
void clear_rumble();
bool rumble_step();  // host loop, 60 times a second: whether the motor is on for the next step

// GamePad motion from host controller sensors (motion.cpp), in VPAD units
struct Motion {
    float acc[3], acc_magnitude, acc_acceleration;  // g
    float gyro_change[3];                           // revolutions per second
    float orientation[3];                           // revolutions
    float dir[9];                                   // attitude matrix (x, y, z axes)
};
void motion_sensor(bool gyro, const float data[3], uint64_t timestamp_ns);  // SDL units: rad/s, m/s^2
void motion_reset();
bool motion(Motion* out);  // false until a controller with motion sensors reports
// Virtual GamePad (PC Motion Control): pose = (w, x, y, z) in the fusion's frame (lying flat, screen up =
// (sqrt(1/2), sqrt(1/2), 0, 0)); advances its own fusion by dt seconds and returns the VPAD values.
void virtual_motion(const float pose[4], float dt, Motion* out);
void virtual_motion_reset();
}  // namespace input
