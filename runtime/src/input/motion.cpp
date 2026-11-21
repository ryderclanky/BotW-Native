// GamePad motion (gyro / accelerometer / attitude) from a host controller's sensors, e.g. the
// DualSense. Port of Cemu's input/motion (MahonySensorFusion, WiiUMotionHandler, MotionSample) and
// its SDL sensor handling (SDLControllerProvider), MPL-2.0, so values match what games expect there.
#include <cmath>
#include <mutex>

#include "input.h"

namespace input {
namespace {
constexpr float kPi = 3.14159265f;

struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
    Quat operator*(const Quat& r) const {
        return {w * r.w - x * r.x - y * r.y - z * r.z, w * r.x + x * r.w + y * r.z - z * r.y,
                w * r.y - x * r.z + y * r.w + z * r.x, w * r.z + x * r.y - y * r.x + z * r.w};
    }
    void normalize_xyzw() {
        float s = 1.0f / sqrtf(w * w + x * x + y * y + z * z);
        w *= s, x *= s, y *= s, z *= s;
    }
    static Quat angle_axis(float angle, float ax, float ay, float az) {  // keeps w, scales xyz
        Quat q{cosf(angle * 0.5f), ax, ay, az};
        float s = sqrtf(1.0f - q.w * q.w) / sqrtf(ax * ax + ay * ay + az * az);
        q.x *= s, q.y *= s, q.z *= s;
        return q;
    }
};

// Mahony IMU filter with gyro bias estimation
struct Mahony {
    Quat q{sqrtf(0.5f), sqrtf(0.5f), 0, 0};  // default pose: held in hands, tilted forward
    float roll = 0, pitch = 0, yaw = 0;
    int roll_w = 0, pitch_w = 0, yaw_w = 0;
    float bias[3]{};
    double bias_sum[3]{};
    uint64_t bias_n = 0;
    bool learn_bias = true;  // off for the virtual GamePad (its rates are exact)

    void update(float dt, float gx, float gy, float gz, float ax, float ay, float az) {
        if (dt > 0.2f) dt = 0.2f;
        if (learn_bias && fabsf(gx) < 0.35f && fabsf(gy) < 0.35f && fabsf(gz) < 0.35f) {  // at rest: learn the bias
            bias_sum[0] += gx, bias_sum[1] += gy, bias_sum[2] += gz;
            if (++bias_n >= 200)
                for (int k = 0; k < 3; k++) bias[k] = (float)(bias_sum[k] / (double)bias_n);
        }
        float g[3] = {gx - bias[0], gy - bias[1], gz - bias[2]};
        for (float& v : g)
            if (fabsf(v) < 0.015f) v = 0;
        if (fabsf(ax) > 1e-6f || fabsf(ay) > 1e-6f || fabsf(az) > 1e-6f) {
            float len = sqrtf(ax * ax + ay * ay + az * az);
            float a[3] = {ax / len, ay / len, az / len};
            float grav[3] = {(q.x * q.z - q.w * q.y), (q.y * q.z + q.w * q.x), (q.w * q.w + q.z * q.z) - 0.5f};  // Z * 0.5
            g[0] -= grav[1] * a[2] - grav[2] * a[1];
            g[1] -= grav[2] * a[0] - grav[0] * a[2];
            g[2] -= grav[0] * a[1] - grav[1] * a[0];
        }
        for (float& v : g) v *= 0.5f * dt;
        Quat d = q * Quat{0, g[0], g[1], g[2]};
        q.w += d.w, q.x += d.x, q.y += d.y, q.z += d.z;
        q.normalize_xyzw();
        float pr = roll, pp = pitch, py = yaw;
        roll = atan2f(2 * (q.z * q.w + q.x * q.y), 1 - 2 * (q.w * q.w + q.x * q.x));
        float sp = 2 * (q.z * q.x - q.y * q.w);
        pitch = fabsf(sp) >= 1 ? copysignf(kPi / 2, sp) : asinf(sp);
        yaw = atan2f(2 * (q.z * q.y + q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y));
        auto winding = [](float prev, float now) { return now - prev > kPi ? -1 : prev - now > kPi ? 1 : 0; };
        roll_w += winding(pr, roll), pitch_w += winding(pp, pitch), yaw_w += winding(py, yaw);
    }
};

float rad_to_rev(float r) { return r / (2.0f * kPi); }

// x axis of a rotation in VPAD's mixed-handedness convention (MotionSample::getXVector)
void x_vector(const Quat& q, float* v) {
    v[0] = 1 - 2 * (q.y * q.y + q.z * q.z);
    v[2] = 2 * (q.x * q.y + q.z * q.w);
    v[1] = 2 * (q.x * q.z - q.y * q.w);
}

std::mutex g_lock;
Mahony g_imu;
bool g_valid = false, g_has_acc = false, g_has_gyro = false;
uint64_t g_ts_acc = 0, g_ts_gyro = 0, g_ts_integrate = 0;
float g_acc[3]{}, g_prev_acc[3]{}, g_gyro[3]{};  // handler space
float g_sens_acc[3]{}, g_sens_gyro[3]{};       // latest tracked samples
Motion g_out;

// VPAD values from the fusion and the handler-space sample it was given (WiiUMotionHandler / MotionSample).
void fill(const Mahony& imu, const float acc[3], const float prev_acc[3], const float gyro[3], Motion& m) {
    m.acc[0] = -acc[0], m.acc[1] = -acc[1], m.acc[2] = acc[2];
    m.acc_magnitude = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
    float dx = acc[0] - prev_acc[0], dy = acc[1] - prev_acc[1], dz = acc[2] - prev_acc[2];
    m.acc_acceleration = sqrtf(dx * dx + dy * dy + dz * dz);
    m.gyro_change[0] = rad_to_rev(-(gyro[0] - imu.bias[0]));
    m.gyro_change[1] = rad_to_rev(-(gyro[1] - imu.bias[1]));
    m.gyro_change[2] = rad_to_rev(gyro[2] - imu.bias[2]);
    m.orientation[0] = rad_to_rev(-(imu.yaw + imu.yaw_w * 2 * kPi)) - 0.5f;
    m.orientation[1] = rad_to_rev(-(imu.pitch + imu.pitch_w * 2 * kPi)) - 0.5f;
    m.orientation[2] = rad_to_rev(imu.roll + imu.roll_w * 2 * kPi);
    x_vector(imu.q, m.dir + 0);
    x_vector(imu.q * Quat::angle_axis(kPi / 2, 0, 0, 1), m.dir + 3);
    x_vector(imu.q * Quat::angle_axis(kPi / 2, 0, 1, 0), m.dir + 6);
}

void integrate(float dt) {
    // WiiUMotionHandler::processMotionSample(dt, gyro, acc.x, -acc.y, -acc.z)
    float ax = g_sens_acc[0], ay = -g_sens_acc[1], az = -g_sens_acc[2];
    for (int k = 0; k < 3; k++) g_prev_acc[k] = g_acc[k], g_gyro[k] = g_sens_gyro[k];
    g_acc[0] = ax, g_acc[1] = ay, g_acc[2] = az;
    g_imu.update(dt, g_gyro[0], g_gyro[1], g_gyro[2], ax, ay, az);
    fill(g_imu, g_acc, g_prev_acc, g_gyro, g_out);
    g_valid = true;
}

// Virtual GamePad (PC Motion Control): its own fusion, fed a simulated IMU for a pose.
std::mutex g_vlock;
Mahony fresh_virtual() {
    Mahony m;
    m.learn_bias = false;
    return m;
}
Mahony g_vimu = fresh_virtual();
float g_vacc[3] = {0, 1, 0};  // handler space, lying flat
}  // namespace

void virtual_motion(const float pose[4], float dt, Motion* out) {
    // Port of Zelda-on-PC's FillMotion (PCInput.cpp, MPL-2.0): the rate is the one that takes the fusion's own
    // orientation to the pose within this sample (closed loop: nothing drifts), the accelerometer is gravity for
    // the pose, so attitude, accumulated angles, rates and accelerometer agree as with a physical controller.
    std::lock_guard<std::mutex> l(g_vlock);
    Quat p{pose[0], pose[1], pose[2], pose[3]};
    p.normalize_xyzw();
    const Quat& qf = g_vimu.q;
    const Quat d = Quat{qf.w, -qf.x, -qf.y, -qf.z} * p;  // the fusion integrates q' = q * (1, rate * dt / 2)
    if (dt < 1.0f / 1000.0f) dt = 1.0f / 1000.0f;
    if (dt > 0.2f) dt = 0.2f;
    float rate[3] = {0, 0, 0};
    if (fabsf(d.w) > 0.01f) {
        const float k = 2.0f / (d.w * dt);
        rate[0] = d.x * k, rate[1] = d.y * k, rate[2] = d.z * k;
    }
    for (float& r : rate) {
        r = fmaxf(-60.0f, fminf(60.0f, r));
        if (fabsf(r) < 0.0005f) r = 0;  // settled: exactly static
    }
    const float grav[3] = {2.0f * (p.x * p.z - p.w * p.y), 2.0f * (p.y * p.z + p.w * p.x),
                           2.0f * (p.w * p.w + p.z * p.z) - 1.0f};  // Quaternion::GetVectorZ
    float prev[3] = {g_vacc[0], g_vacc[1], g_vacc[2]};
    for (int k = 0; k < 3; k++) g_vacc[k] = grav[k];
    g_vimu.update(dt, rate[0], rate[1], rate[2], grav[0], grav[1], grav[2]);
    fill(g_vimu, g_vacc, prev, rate, *out);
}

void virtual_motion_reset() {
    std::lock_guard<std::mutex> l(g_vlock);
    g_vimu = fresh_virtual();
    g_vacc[0] = 0, g_vacc[1] = 1, g_vacc[2] = 0;
}

void motion_sensor(bool gyro, const float data[3], uint64_t ts) {
    std::lock_guard<std::mutex> l(g_lock);
    uint64_t& last = gyro ? g_ts_gyro : g_ts_acc;
    if (ts <= last) return;
    if (ts - last >= 10000000000ull) {  // stale stream: restart pairing
        g_has_acc = g_has_gyro = false;
        last = ts;
        return;
    }
    last = ts;
    if (gyro) {
        g_sens_gyro[0] = data[0], g_sens_gyro[1] = -data[1], g_sens_gyro[2] = -data[2];  // rad/s
        g_has_gyro = true;
    } else {
        for (int k = 0; k < 3; k++) g_sens_acc[k] = -data[k] / 9.81f;  // m/s^2 -> g
        g_has_acc = true;
    }
    if (g_has_acc && g_has_gyro) {
        uint64_t t = g_ts_gyro > g_ts_acc ? g_ts_gyro : g_ts_acc;
        if (t > g_ts_integrate) {
            float dt = std::fmin((float)(t - g_ts_integrate) / 1e9f, 1.0f);
            g_ts_integrate = t;
            integrate(dt);
        }
        g_has_acc = g_has_gyro = false;
    }
}

void motion_reset() {
    std::lock_guard<std::mutex> l(g_lock);
    g_imu = Mahony{};
    g_valid = g_has_acc = g_has_gyro = false;
    g_ts_acc = g_ts_gyro = g_ts_integrate = 0;
}

bool motion(Motion* out) {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_valid) return false;
    *out = g_out;
    return true;
}
}  // namespace input
