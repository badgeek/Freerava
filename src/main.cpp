// cyclomp — mock ride driver with session tracking.
// Feeds fake but plausible telemetry into the Slint UI on a repeating timer,
// and turns finished rides into SessionRow entries for the history list.

#include "cyclomp.h"

#ifdef CYCLOMP_HAVE_MAPLIBRE
#include "map_service.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>

namespace {

// Ride lifecycle, mirrored in the UI's `ride-state`.
enum RideState : int { Idle = 0, Running = 1, Paused = 2 };

// Tiny deterministic PRNG (no <random> pulled in for a mock).
struct Rng {
    uint32_t s = 0x9e3779b9u;
    float next() { // [0,1)
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (s >> 8) * (1.0f / 16777216.0f);
    }
    float centered() { return next() - 0.5f; } // [-0.5,0.5)
};

struct RideModel {
    Rng rng;
    float elapsed_s = 0.f; // moving seconds (pauses excluded)
    float dist_km   = 0.f;
    float speed     = 0.f; // km/h
    float max_speed = 0.f;
    float sum_speed = 0.f;
    long  sum_cad   = 0;
    long  sum_hr    = 0;
    int   heart_rate = 72;
    int   samples   = 0;
    int   nav_m     = 400; // metres to next turn

    // Mock position (Bandung); wanders while riding. Survives reset() so
    // consecutive rides continue from where the last one ended.
    double lat = -6.9147;
    double lon = 107.6098;
    float  heading_deg = 90.f;

    void reset() {
        elapsed_s = 0.f;
        dist_km = 0.f;
        speed = 0.f;
        max_speed = 0.f;
        sum_speed = 0.f;
        sum_cad = 0;
        sum_hr = 0;
        heart_rate = 72;
        samples = 0;
        nav_m = 400;
    }

    void tick(float dt_s) {
        elapsed_s += dt_s;

        // Speed random-walk, clamped to a sane cycling range.
        speed += rng.centered() * 6.f;
        if (speed < 8.f)  speed = 8.f;
        if (speed > 42.f) speed = 42.f;
        if (speed > max_speed) max_speed = speed;

        // Wander the mock position along a slowly-turning heading.
        heading_deg += rng.centered() * 24.f;
        double d_km = speed * (dt_s / 3600.f);
        double rad = heading_deg * M_PI / 180.0;
        lat += (d_km * std::cos(rad)) / 111.32;
        lon += (d_km * std::sin(rad)) / (111.32 * std::cos(lat * M_PI / 180.0));

        dist_km   += speed * (dt_s / 3600.f);
        sum_speed += speed;
        sum_cad   += cadence();
        samples   += 1;

        // Heart rate tracks effort (speed) with jitter.
        heart_rate = (int)std::lround(90 + speed * 1.9f + rng.centered() * 6.f);
        sum_hr += heart_rate;

        nav_m -= (int)std::lround(speed * (dt_s / 3600.f) * 1000.f);
        if (nav_m <= 0) nav_m = 400 + (int)(rng.next() * 800.f);
    }

    float avg() const { return samples ? sum_speed / samples : 0.f; }
    int   avg_cadence() const { return samples ? (int)(sum_cad / samples) : 0; }
    int   avg_hr() const { return samples ? (int)(sum_hr / samples) : 0; }
    int   cadence() const { return speed > 0 ? (int)std::lround(speed * 2.4f + 12) : 0; }
};

slint::SharedString fmt1(float v) { // one decimal
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return slint::SharedString(buf);
}

slint::SharedString fmt0(float v) { // rounded integer
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d", (int)std::lround(v));
    return slint::SharedString(buf);
}

slint::SharedString fmt_int(int v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d", v);
    return slint::SharedString(buf);
}

slint::SharedString mmss(int s) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", s / 60, s % 60);
    return slint::SharedString(buf);
}

slint::SharedString metres(int m) {
    char buf[24];
    if (m >= 1000) std::snprintf(buf, sizeof buf, "%.1f km", m / 1000.f);
    else           std::snprintf(buf, sizeof buf, "%d m", m);
    return slint::SharedString(buf);
}

// "07 SEP 14:32" — uppercased month to match the dashboard's type style.
slint::SharedString date_label() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%d %b %H:%M", std::localtime(&t));
    for (char *p = buf; *p; ++p) *p = (char)std::toupper((unsigned char)*p);
    return slint::SharedString(buf);
}

// Push the model's live values into the UI.
void publish_live(AppWindow &w, const RideModel &m) {
    w.set_speed(m.speed);
    w.set_distance(fmt1(m.dist_km));
    w.set_ride_time(mmss((int)m.elapsed_s));
    w.set_avg_speed(m.avg());
    w.set_cadence(m.cadence());
    w.set_heart_rate(m.heart_rate);
    int hr = m.heart_rate;
    w.set_hr_zone(hr < 105 ? 1 : hr < 125 ? 2 : hr < 145 ? 3 : hr < 165 ? 4 : 5);
    w.set_nav_distance(metres(m.nav_m));
}

// Reset the live view to a resting/idle state.
void publish_idle(AppWindow &w) {
    w.set_speed(0.f);
    w.set_distance(slint::SharedString("0.0"));
    w.set_ride_time(slint::SharedString("00:00"));
    w.set_avg_speed(0.f);
    w.set_cadence(0);
    w.set_heart_rate(72);
    w.set_hr_zone(1);
    w.set_nav_distance(slint::SharedString("400 m"));
}

} // namespace

#ifdef __ANDROID__
// Capture the JavaVM for the JNI HTTP transport (map tiles). NativeActivity
// dlsym's ANativeActivity_onCreate from OUR lib before libslint_cpp.so, so we
// grab activity->vm here and forward to Slint's real entry point.
#include "android_env.h"
#include <android/native_activity.h>
#include <dlfcn.h>

namespace {
JavaVM *g_java_vm = nullptr;
}
JavaVM *cyclomp_java_vm() { return g_java_vm; }

#ifdef CYCLOMP_HAVE_MAPLIBRE
// mbgl-core's android platform threads attach to the JVM through this global
// (normally assigned by the Java SDK's JNI_OnLoad, which we don't have).
namespace mln { namespace android { extern JavaVM *theJVM; } }
#endif

extern "C" JNIEXPORT void ANativeActivity_onCreate(
    ANativeActivity *activity, void *savedState, size_t savedStateSize) {
    g_java_vm = activity->vm;
#ifdef CYCLOMP_HAVE_MAPLIBRE
    mln::android::theJVM = activity->vm;
#endif
    using Fn = void (*)(ANativeActivity *, void *, size_t);
    void *h = dlopen("libslint_cpp.so", RTLD_NOW);
    Fn real = h ? (Fn)dlsym(h, "ANativeActivity_onCreate") : nullptr;
    if (real && real != &ANativeActivity_onCreate)
        real(activity, savedState, savedStateSize);
}
#endif

#ifdef __ANDROID__
extern "C" void slint_main()
#else
int main(int, char **)
#endif
{
    auto ui = AppWindow::create();
    auto model = std::make_shared<RideModel>();

    // History of finished rides, newest first. In-memory for the mock.
    auto sessions = std::make_shared<slint::VectorModel<SessionRow>>();
    ui->set_sessions(sessions);
    publish_idle(*ui);

    // Debug: start on a given screen (0 ride / 1 map / 2 history).
    if (const char *s = std::getenv("CYCLOMP_SCREEN"))
        ui->set_screen(std::atoi(s));

    // ---- Map hook: camera follows the (mock) rider ----
    struct MapHook {
#ifdef CYCLOMP_HAVE_MAPLIBRE
        std::unique_ptr<MapService> service;
#endif
        double zoom = 15.0;
        void push(const RideModel &m) {
#ifdef CYCLOMP_HAVE_MAPLIBRE
            if (service) service->set_camera(m.lat, m.lon, zoom);
#else
            (void)m;
#endif
        }
    };
    auto map_hook = std::make_shared<MapHook>();
#ifdef CYCLOMP_HAVE_MAPLIBRE
    map_hook->service = std::make_unique<MapService>(
        480, 720,
        [ui = slint::ComponentWeakHandle(ui)](uint32_t w, uint32_t h,
                                              std::vector<uint8_t> rgba) {
            // Frame arrives on the map thread; hop to the UI thread.
            slint::invoke_from_event_loop(
                [ui, w, h, rgba = std::move(rgba)] {
                    auto u = ui.lock();
                    if (!u) return;
                    slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(
                        w, h,
                        reinterpret_cast<const slint::Rgba8Pixel *>(rgba.data()));
                    (*u)->set_map_frame(slint::Image(buf));
                });
        });
    ui->set_map_available(true);
    map_hook->push(*model); // initial frame
#endif
    ui->on_map_zoom([model, map_hook](int delta) {
        map_hook->zoom = std::clamp(map_hook->zoom + delta, 3.0, 19.0);
#ifdef CYCLOMP_HAVE_MAPLIBRE
        if (map_hook->service)
            map_hook->service->set_camera(model->lat, model->lon, map_hook->zoom);
#endif
    });

    // idle -> start (fresh session) | running -> pause | paused -> resume
    ui->on_toggle_ride([ui = slint::ComponentWeakHandle(ui), model] {
        auto u = ui.lock();
        if (!u) return;
        auto &w = **u;
        switch (w.get_ride_state()) {
        case Idle:
            model->reset();
            publish_idle(w);
            w.set_ride_state(Running);
            break;
        case Running:
            w.set_ride_state(Paused);
            break;
        default:
            w.set_ride_state(Running);
            break;
        }
    });

    // Finalize the ride: save a summary row, then return to idle.
    ui->on_stop_ride([ui = slint::ComponentWeakHandle(ui), model, sessions] {
        auto u = ui.lock();
        if (!u) return;
        auto &w = **u;
        if (w.get_ride_state() == Idle) return;
        if (model->samples > 0) {
            SessionRow row {
                date_label(),
                fmt1(model->dist_km),
                mmss((int)model->elapsed_s),
                fmt0(model->avg()),
                fmt0(model->max_speed),
                fmt_int(model->avg_cadence()),
                fmt_int(model->avg_hr()),
            };
            sessions->insert(0, row); // newest first
        }
        model->reset();
        publish_idle(w);
        w.set_selected_session(-1);
        w.set_ride_state(Idle);
    });

    // ~500 ms telemetry tick; only advances while RUNNING (pause freezes all).
    static slint::Timer timer;
    timer.start(slint::TimerMode::Repeated, std::chrono::milliseconds(500),
        [ui = slint::ComponentWeakHandle(ui), model, map_hook] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if (w.get_ride_state() != Running) return;
            model->tick(0.5f);
            publish_live(w, *model);
            map_hook->push(*model);
        });

    ui->run();
#ifndef __ANDROID__
    return 0;
#endif
}
