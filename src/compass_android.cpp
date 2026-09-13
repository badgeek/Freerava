#ifdef __ANDROID__
#include "compass_android.h"

#include <android/log.h>
#include <android/looper.h>
#include <android/sensor.h>

#include <atomic>
#include <cmath>
#include <thread>

#define CMP_LOG(...) __android_log_print(ANDROID_LOG_INFO, "cyclomp-compass", __VA_ARGS__)

namespace compass {
namespace {

std::atomic<bool> g_started{false};
std::atomic<bool> g_have{false};
std::atomic<double> g_azimuth{0.0};

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 cross(const V3 &a, const V3 &b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
double dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(const V3 &a) {
    double l = std::sqrt(dot(a, a));
    return l > 1e-9 ? V3{a.x / l, a.y / l, a.z / l} : V3{};
}

// Heading from the world-frame direction of the phone's "forward": the
// screen-top axis when the phone lies flat, the view direction when it is
// upright (bike mount).
void publish(double he, double hn) {
    if (std::fabs(he) < 1e-6 && std::fabs(hn) < 1e-6) return;
    double az = std::atan2(he, hn) * 180.0 / M_PI;
    if (az < 0) az += 360.0;
    static bool first = true;
    if (first) {
        CMP_LOG("first heading %.0f deg", az);
        first = false;
    }
    g_azimuth.store(az);
    g_have.store(true);
}

void from_quaternion(double x, double y, double z, double w) {
    // Device axes in world coordinates (X=E, Y=N, Z=up).
    double uy_e = 2.0 * (x * y - w * z);
    double uy_n = 1.0 - 2.0 * (x * x + z * z);
    double uy_u = 2.0 * (y * z + w * x);
    double fz_e = -2.0 * (x * z + w * y);
    double fz_n = -2.0 * (y * z - w * x);
    if (std::fabs(uy_u) < 0.8) publish(uy_e, uy_n);
    else publish(fz_e, fz_n);
}

// Tilt-compensated fallback from raw accel + magnetometer.
V3 g_acc, g_mag;
bool g_haveAcc = false, g_haveMag = false;

void from_acc_mag() {
    if (!g_haveAcc || !g_haveMag) return;
    V3 up = norm(g_acc);            // gravity, device frame
    V3 east = norm(cross(g_mag, up));
    V3 north = norm(cross(up, east));
    // Same forward selection as above, in the device frame.
    V3 fwd = std::fabs(up.y) < 0.8 ? V3{0, 1, 0} : V3{0, 0, -1};
    publish(dot(fwd, east), dot(fwd, north));
}

void run() {
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorManager *mgr =
        ASensorManager_getInstanceForPackage("dev.bauhouse.cyclomp");
    if (!mgr) {
        CMP_LOG("no sensor manager");
        return;
    }
    const ASensor *rot =
        ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_ROTATION_VECTOR);
    const ASensor *acc = nullptr, *mag = nullptr;
    if (!rot) {
        acc = ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_ACCELEROMETER);
        mag = ASensorManager_getDefaultSensor(mgr,
                                              ASENSOR_TYPE_MAGNETIC_FIELD);
        if (!acc || !mag) {
            CMP_LOG("no usable sensors (rot=%p acc=%p mag=%p)", (void *)rot,
                    (void *)acc, (void *)mag);
            return;
        }
    }
    ASensorEventQueue *q =
        ASensorManager_createEventQueue(mgr, looper, 1, nullptr, nullptr);
    if (!q) {
        CMP_LOG("no event queue");
        return;
    }
    auto enable = [&](const ASensor *s) {
        int rc = ASensorEventQueue_enableSensor(q, s);
        ASensorEventQueue_setEventRate(q, s, 200000); // 5 Hz
        return rc;
    };
    if (rot) {
        CMP_LOG("using rotation-vector (enable=%d)", enable(rot));
    } else {
        CMP_LOG("using accel+mag fallback (enable=%d/%d)", enable(acc),
                enable(mag));
    }

    for (;;) {
        int events;
        void *data;
        if (ALooper_pollOnce(-1, nullptr, &events, &data) >= 0) {
            ASensorEvent ev;
            while (ASensorEventQueue_getEvents(q, &ev, 1) > 0) {
                switch (ev.type) {
                case ASENSOR_TYPE_ROTATION_VECTOR:
                    from_quaternion(ev.data[0], ev.data[1], ev.data[2],
                                    ev.data[3]);
                    break;
                case ASENSOR_TYPE_ACCELEROMETER:
                    // light low-pass to steady the gravity estimate
                    g_acc = {g_acc.x * 0.7 + ev.acceleration.x * 0.3,
                             g_acc.y * 0.7 + ev.acceleration.y * 0.3,
                             g_acc.z * 0.7 + ev.acceleration.z * 0.3};
                    g_haveAcc = true;
                    from_acc_mag();
                    break;
                case ASENSOR_TYPE_MAGNETIC_FIELD:
                    g_mag = {g_mag.x * 0.7 + ev.magnetic.x * 0.3,
                             g_mag.y * 0.7 + ev.magnetic.y * 0.3,
                             g_mag.z * 0.7 + ev.magnetic.z * 0.3};
                    g_haveMag = true;
                    from_acc_mag();
                    break;
                default:
                    break;
                }
            }
        }
    }
}

} // namespace

void start() {
    if (g_started.exchange(true)) return;
    CMP_LOG("starting");
    std::thread(run).detach();
}

bool azimuth_deg(double &out) {
    if (!g_have.load()) return false;
    out = g_azimuth.load();
    return true;
}

} // namespace compass
#endif
