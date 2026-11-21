// PC Motion Control: the mouse tilts a virtual GamePad. Port of Zelda-on-PC's tilt emulation
// (src/pcedition/PCInput.cpp: WorldRotation, LevelBy, FollowTilt, ...; MPL-2.0,
// https://github.com/mircea32000/Zelda-on-PC). This Source Code Form is subject to the terms of the Mozilla
// Public License, v. 2.0. If a copy of the MPL was not distributed with this file, You can obtain one at
// https://mozilla.org/MPL/2.0/.
//
// The virtual GamePad has a pose in the motion fusion's frame. The game is not handed made-up VPAD values: the
// pose goes through the same sensor fusion as a real gyro controller (input::virtual_motion), so everything the
// game reads agrees, as with a physical GamePad. The pose is turned around fixed axes of the player's view of the
// GamePad lying flat (X left-right, -Y towards the player, Z up), so the mouse turns it the same way on screen
// however it lies already, like a trackball. The reported pose follows the set one like a GamePad in a hand: a
// critically damped spring with a speed limit.
#include "pc_tilt.h"

#include <algorithm>
#include <cmath>

#include "input.h"

namespace pctilt {
namespace {
struct Q {
    float w = 1, x = 0, y = 0, z = 0;
    Q operator*(const Q& r) const {
        return {w * r.w - x * r.x - y * r.y - z * r.z, w * r.x + x * r.w + y * r.z - z * r.y,
                w * r.y - x * r.z + y * r.w + z * r.x, w * r.z + x * r.y - y * r.x + z * r.w};
    }
    void normalize() {
        const float s = 1.0f / std::sqrt(w * w + x * x + y * y + z * z);
        w *= s, x *= s, y *= s, z *= s;
    }
};

constexpr float kDegToRad = 3.14159265f / 180.0f;
const Q kFlat{std::sqrt(0.5f), std::sqrt(0.5f), 0, 0};  // lying flat, screen up

// rotation by an angle (radians, either sign) around a unit axis
Q axis_rotation(float angle, float x, float y, float z) {
    const float s = std::sin(angle * 0.5f);
    return {std::cos(angle * 0.5f), x * s, y * s, z * s};
}

Q world_rotation(float angle, float x, float y, float z, const Q& pose) {
    Q r = axis_rotation(angle, x, y, z) * pose;
    r.normalize();
    return r;
}

// the screen's normal in the fusion's frame (q * (0,1,0) * q^-1)
void screen_normal(const Q& q, float& nx, float& ny, float& nz) {
    nx = 2.0f * (q.x * q.y - q.w * q.z);
    ny = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    nz = 2.0f * (q.y * q.z + q.w * q.x);
}

float tilt_of(const Q& q) {
    float nx, ny, nz;
    screen_normal(q, nx, ny, nz);
    return std::acos(std::clamp(nz, -1.0f, 1.0f));
}

// turns the screen's normal towards up by (at most) angle radians: the tilt gets smaller, the turn stays
Q level_by(const Q& q, float angle) {
    float nx, ny, nz;
    screen_normal(q, nx, ny, nz);
    const float tilt = std::acos(std::clamp(nz, -1.0f, 1.0f));
    if (tilt < 1e-5f || angle <= 0.0f) return q;
    float ax = ny, ay = -nx;  // axis n x up; straight upside down any horizontal axis does
    const float len = std::sqrt(ax * ax + ay * ay);
    if (len < 1e-5f) ax = 1.0f, ay = 0.0f;
    else ax /= len, ay /= len;
    return world_rotation(std::min(angle, tilt), ax, ay, 0.0f, q);
}

Q slerp(const Q& a, Q b, float t) {
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot < 0.0f) b = {-b.w, -b.x, -b.y, -b.z}, dot = -dot;
    float wa = 1.0f - t, wb = t;
    if (dot < 0.9995f) {
        const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f)), sn = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / sn;
        wb = std::sin(t * theta) / sn;
    }
    Q r{a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb};
    r.normalize();
    return r;
}

// from a towards b by at most angle radians
Q turn_toward(const Q& a, const Q& b, float angle) {
    const float dot = std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    const float between = 2.0f * std::acos(std::min(1.0f, dot));
    return between <= angle ? b : slerp(a, b, angle / between);
}

// The shown pose follows the target: critically damped spring, speed limited. tau in seconds (0: at once).
// Returns true once it has arrived (exactly the target, at rest).
bool follow(Q& shown, float vel[3], const Q& target, float tau, float max_speed, float dt) {
    if (tau <= 0.0f) {
        shown = target;
        vel[0] = vel[1] = vel[2] = 0.0f;
        return true;
    }
    const float k = 1.0f / tau;
    const int steps = std::clamp((int)std::ceil(dt / 0.004f), 1, 64);  // small steps: stable for any frame time
    const float h = dt / (float)steps;
    for (int i = 0; i < steps; i++) {
        // the rotation from the shown pose to the target as axis * angle, the short way round
        Q e = target * Q{shown.w, -shown.x, -shown.y, -shown.z};
        if (e.w < 0.0f) e = {-e.w, -e.x, -e.y, -e.z};
        const float sn = std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z);
        float r[3] = {0, 0, 0};
        if (sn > 1e-7f) {
            const float angle = 2.0f * std::atan2(sn, e.w);
            r[0] = e.x / sn * angle, r[1] = e.y / sn * angle, r[2] = e.z / sn * angle;
        }
        float speed = 0.0f;
        for (int a = 0; a < 3; a++) {
            vel[a] += (k * k * r[a] - 2.0f * k * vel[a]) * h;
            speed += vel[a] * vel[a];
        }
        speed = std::sqrt(speed);
        if (speed > max_speed) {
            for (int a = 0; a < 3; a++) vel[a] *= max_speed / speed;
            speed = max_speed;
        }
        if (sn < 1e-5f && speed < 1e-3f) {  // arrived: exactly the target (the motion data is static again)
            shown = target;
            vel[0] = vel[1] = vel[2] = 0.0f;
            return true;
        }
        if (speed > 1e-7f) shown = world_rotation(speed * h, vel[0] / speed, vel[1] / speed, vel[2] / speed, shown);
    }
    return false;
}

Q g_target = kFlat, g_shown = kFlat;
float g_vel[3] = {0, 0, 0};
bool g_reporting = false;
}  // namespace

bool step(const Config& cfg, bool active, double mdx, double mdy, int turn_notches, bool level, float dt,
          input::Motion* out) {
    dt = std::clamp(dt, 1.0f / 1000.0f, 0.2f);
    if (active && !g_reporting) {  // turned on: start flat, with a fresh fusion
        g_reporting = true;
        input::virtual_motion_reset();
        g_target = g_shown = kFlat;
        g_vel[0] = g_vel[1] = g_vel[2] = 0.0f;
    }
    if (active) {
        float tsens = 0.05f * cfg.sens * kDegToRad;
        // a hand turns the GamePad only so fast: quicker mouse movement is not used (a flick does not throw the
        // slate over, and it stops where the mouse stops)
        const float wanted = (float)std::hypot(mdx, mdy) * tsens;
        const float allowed = cfg.max_speed * kDegToRad * dt;
        if (wanted > allowed && wanted > 0.0f) tsens *= allowed / wanted;
        Q& q = g_target;
        if (level) q = kFlat;
        // mouse up / down: forward / back, left / right: sideways, the wheel: flat (yaw)
        if (mdy) q = world_rotation((float)mdy * tsens * (cfg.invert_y ? -1.0f : 1.0f), 1.0f, 0.0f, 0.0f, q);
        if (mdx) q = world_rotation((float)mdx * tsens * (cfg.invert_x ? -1.0f : 1.0f), 0.0f, -1.0f, 0.0f, q);
        if (turn_notches)
            q = world_rotation((float)turn_notches * cfg.turn_step * kDegToRad * (cfg.invert_turn ? 1.0f : -1.0f), 0.0f,
                               0.0f, 1.0f, q);
        if (cfg.return_speed > 0.0f) q = level_by(q, cfg.return_speed * kDegToRad * dt);
        const float max_tilt = cfg.max_angle * kDegToRad;  // at 180 degrees it goes all the way round
        if (cfg.max_angle < 179.5f && tilt_of(q) > max_tilt) q = level_by(q, tilt_of(q) - max_tilt);
    } else {
        g_target = turn_toward(g_target, kFlat, 400.0f * kDegToRad * dt);  // level smoothly
    }
    const bool arrived = follow(g_shown, g_vel, g_target, cfg.smoothing_ms / 1000.0f, cfg.max_speed * kDegToRad, dt);
    if (!active && arrived && g_reporting && g_target.w == kFlat.w && g_target.x == kFlat.x && g_target.y == kFlat.y &&
        g_target.z == kFlat.z) {
        // flat and at rest again: hand the GamePad's motion back to the controller / the fixed flat values
        g_reporting = false;
        input::virtual_motion_reset();
    }
    if (!g_reporting) return false;
    const float pose[4] = {g_shown.w, g_shown.x, g_shown.y, g_shown.z};
    input::virtual_motion(pose, dt, out);
    return true;
}
}  // namespace pctilt
