#ifdef __ANDROID__
#include "compass_android.h"

#include <android/looper.h>
#include <android/sensor.h>

#include <atomic>
#include <cmath>
#include <thread>

namespace compass {
namespace {

std::atomic<bool> g_started{false};
std::atomic<bool> g_have{false};
std::atomic<double> g_azimuth{0.0};

void run() {
    ALooper *looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    ASensorManager *mgr = ASensorManager_getInstanceForPackage("dev.bauhouse.cyclomp");
    if (!mgr) return;
    const ASensor *rot =
        ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_ROTATION_VECTOR);
    if (!rot) return; // no sensor (some emulators): compass just stays silent
    ASensorEventQueue *q =
        ASensorManager_createEventQueue(mgr, looper, 1, nullptr, nullptr);
    if (!q) return;
    ASensorEventQueue_enableSensor(q, rot);
    ASensorEventQueue_setEventRate(q, rot, 200000); // 5 Hz is plenty

    for (;;) {
        int events;
        void *data;
        if (ALooper_pollOnce(-1, nullptr, &events, &data) >= 0) {
            ASensorEvent ev;
            while (ASensorEventQueue_getEvents(q, &ev, 1) > 0) {
                if (ev.type != ASENSOR_TYPE_ROTATION_VECTOR) continue;
                // Quaternion (x, y, z, w), device -> world (X=E, Y=N, Z=up).
                double x = ev.data[0], y = ev.data[1], z = ev.data[2];
                double w = ev.data[3];
                // World direction of the device's screen-top (device +Y)...
                double uy_e = 2.0 * (x * y - w * z);
                double uy_n = 1.0 - 2.0 * (x * x + z * z);
                double uy_u = 2.0 * (y * z + w * x);
                // ...and of the rider's forward view (device -Z).
                double fz_e = -2.0 * (x * z + w * y);
                double fz_n = -2.0 * (y * z - w * x);
                // Phone flat on a mount: screen-top points forward. Phone
                // upright: screen-top points at the sky — use the view
                // direction instead.
                double he, hn;
                if (std::fabs(uy_u) < 0.8) {
                    he = uy_e;
                    hn = uy_n;
                } else {
                    he = fz_e;
                    hn = fz_n;
                }
                if (std::fabs(he) < 1e-6 && std::fabs(hn) < 1e-6) continue;
                double az = std::atan2(he, hn) * 180.0 / M_PI;
                if (az < 0) az += 360.0;
                g_azimuth.store(az);
                g_have.store(true);
            }
        }
    }
}

} // namespace

void start() {
    if (g_started.exchange(true)) return;
    std::thread(run).detach();
}

bool azimuth_deg(double &out) {
    if (!g_have.load()) return false;
    out = g_azimuth.load();
    return true;
}

} // namespace compass
#endif
