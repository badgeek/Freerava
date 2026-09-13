// cyclomp — mock ride driver with session tracking.
// Feeds fake but plausible telemetry into the Slint UI on a repeating timer,
// and turns finished rides into SessionRow entries for the history list.

#include "cyclomp.h"

#include "core/follow_camera.h"
#include "core/format.h"
#include "core/mock_telemetry.h"
#include "core/ride_engine.h"

#include <functional>

#ifdef CYCLOMP_HAVE_MAPLIBRE
#include "map_service.h"
#endif
#ifdef CYCLOMP_MAP_GL
#include "map_gl.h"
#include <android/log.h>
#endif
#ifdef __ANDROID__
#include "gps_telemetry_android.h"
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

// Thin SharedString wrappers over the unit-tested core formatters.
slint::SharedString fmt1(float v) { return slint::SharedString(core::fmt::fmt1(v)); }
slint::SharedString fmt0(float v) { return slint::SharedString(core::fmt::fmt0(v)); }
slint::SharedString fmt_int(int v) { return slint::SharedString(core::fmt::fmt_int(v)); }
slint::SharedString mmss(int s) { return slint::SharedString(core::fmt::mmss(s)); }
slint::SharedString metres(int m) { return slint::SharedString(core::fmt::metres(m)); }
slint::SharedString date_label() {
    return slint::SharedString(core::fmt::date_label(std::time(nullptr)));
}

// Push the engine's live values into the UI. `sensors` is false when the
// source has no cadence/heart-rate channel (real GPS) — the tiles then read
// "--" instead of a fabricated number.
void publish_live(AppWindow &w, const core::LiveStats &m, bool sensors) {
    w.set_speed((float)m.speed_kmh);
    w.set_distance(fmt1((float)m.dist_km));
    w.set_ride_time(mmss((int)m.elapsed_s));
    w.set_avg_speed((float)m.avg_kmh);
    w.set_cadence(sensors ? m.cadence : -1);
    w.set_heart_rate(sensors ? m.heart_rate : -1);
    w.set_hr_zone(sensors ? core::hr_zone(m.heart_rate) : 0);
}

// The only place session numbers become strings.
SessionRow to_row(const core::SessionSummary &s) {
    return SessionRow{
        slint::SharedString(core::fmt::date_label(s.ended_at)),
        fmt1((float)s.dist_km),
        mmss((int)s.moving_s),
        fmt0((float)s.avg_kmh),
        fmt0((float)s.max_kmh),
        s.has_cadence ? fmt_int(s.avg_cadence) : slint::SharedString("--"),
        s.has_hr ? fmt_int(s.avg_hr) : slint::SharedString("--"),
    };
}

// Reset the live view to a resting/idle state.
void publish_idle(AppWindow &w, bool sensors) {
    w.set_speed(0.f);
    w.set_distance(slint::SharedString("0.0"));
    w.set_ride_time(slint::SharedString("00:00"));
    w.set_avg_speed(0.f);
    w.set_cadence(sensors ? 0 : -1);
    w.set_heart_rate(sensors ? 72 : -1);
    w.set_hr_zone(sensors ? 1 : 0);
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
jobject g_activity = nullptr;
}
JavaVM *cyclomp_java_vm() { return g_java_vm; }
jobject cyclomp_activity() { return g_activity; }

#ifdef CYCLOMP_HAVE_MAPLIBRE
// mbgl-core's android platform threads attach to the JVM through this global
// (normally assigned by the Java SDK's JNI_OnLoad, which we don't have).
namespace mln { namespace android { extern JavaVM *theJVM; } }
#endif

extern "C" JNIEXPORT void ANativeActivity_onCreate(
    ANativeActivity *activity, void *savedState, size_t savedStateSize) {
    g_java_vm = activity->vm;
    // activity->clazz is only valid for this frame — pin it for the GPS source.
    if (activity->env && activity->clazz)
        g_activity = activity->env->NewGlobalRef(activity->clazz);
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

    // Telemetry: real GPS on Android, the RNG mock everywhere else.
    // CYCLOMP_MOCK_RIDE=1 forces the mock on Android too (demo mode).
    std::shared_ptr<core::MockTelemetrySource> mock;
    std::shared_ptr<core::TelemetrySource> source;
#ifdef __ANDROID__
    std::shared_ptr<gps::AndroidTelemetrySource> gps_source;
    const char *mock_env = std::getenv("CYCLOMP_MOCK_RIDE");
    if (!(mock_env && mock_env[0] == '1')) {
        gps_source = std::make_shared<gps::AndroidTelemetrySource>();
        source = gps_source;
    }
#endif
    if (!source) {
        mock = std::make_shared<core::MockTelemetrySource>();
        source = mock;
    }
    // Cadence and heart rate exist only in the mock; GPS leaves them nullopt.
    const bool sensors = mock != nullptr;

    // The nav strip carries the mock's turn countdown, or the GPS fix status.
    std::function<void(AppWindow &)> publish_nav = [](AppWindow &) {};
    if (mock) {
        publish_nav = [mock](AppWindow &w) { w.set_nav_distance(metres(mock->nav_m())); };
    }
#ifdef __ANDROID__
    if (gps_source) {
        publish_nav = [gps_source](AppWindow &w) {
            gps_source->poll(); // rate limited internally
            auto st = gps_source->status();
            w.set_nav_distance(slint::SharedString(st.distance));
            w.set_nav_instruction(slint::SharedString(st.instruction));
        };
    }
#endif

    auto engine = std::make_shared<core::RideEngine>();
    auto log = std::make_shared<core::SessionLog>();

    // History of finished rides, newest first. In-memory for the mock.
    auto sessions = std::make_shared<slint::VectorModel<SessionRow>>();
    ui->set_sessions(sessions);
    publish_idle(*ui, sensors);
    publish_nav(*ui);

    // Debug: start on a given screen (0 ride / 1 map / 2 history).
    if (const char *s = std::getenv("CYCLOMP_SCREEN"))
        ui->set_screen(std::atoi(s));

    // ---- Camera: core follow-mode decisions + a sink to the platform map ----
    auto camera = std::make_shared<core::FollowCamera>(15.0);
    std::function<void(const core::CameraUpdate &)> cam_sink =
        [](const core::CameraUpdate &) {};
#if defined(CYCLOMP_HAVE_MAPLIBRE) && !defined(CYCLOMP_MAP_GL)
    std::shared_ptr<MapService> map_service;
#endif
#if defined(CYCLOMP_MAP_GL)
    // B-lite: mbgl renders headless (own context) and GPU-blits into shared
    // AHardwareBuffers; the UI shows them as borrowed-GL-texture Images.
    // Frames arrive on the map thread — hop to the UI thread first.
    mapgl::set_frame_sink([ui = slint::ComponentWeakHandle(ui)](
                              uint32_t idx, uint32_t w, uint32_t h) {
        slint::invoke_from_event_loop([ui, idx, w, h] {
            auto u = ui.lock();
            if (!u) return;
            uint32_t tex = mapgl::ui_texture(idx);
            if (tex == 0) {
                // Buffers not imported into Slint's context yet: a redraw
                // runs the notifier (which imports), and a poke re-renders
                // the dropped frame afterwards.
                (*u)->window().request_redraw();
                mapgl::poke();
                return;
            }
            (*u)->set_map_frame(slint::Image::create_from_borrowed_gl_2d_rgba_texture(
                tex, slint::Size<uint32_t>{w, h},
                slint::Image::BorrowedOpenGLTextureOrigin::BottomLeft));
        });
    });
    {
        // The notifier's only job: import the shared buffers into Slint's GL
        // context once they exist.
        auto err = ui->window().set_rendering_notifier(
            [](slint::RenderingState st, slint::GraphicsAPI) {
                if (st != slint::RenderingState::BeforeRendering) return;
                mapgl::ui_import_all();
            });
        if (!err) {
            ui->set_map_available(true);
        } else {
            __android_log_print(ANDROID_LOG_ERROR, "cyclomp-mapgl",
                                "set_rendering_notifier unsupported (err=%d)",
                                (int)*err);
        }
    }
    cam_sink = [](const core::CameraUpdate &up) {
        mapgl::set_camera(up.lat, up.lon, up.zoom);
    };
#elif defined(CYCLOMP_HAVE_MAPLIBRE)
    map_service = std::make_shared<MapService>(
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
    // Desktop CPU path: substitute the tracked zoom when the update says
    // "keep current" (the service has no zoom<0 contract).
    cam_sink = [map_service, camera](const core::CameraUpdate &up) {
        map_service->set_camera(up.lat, up.lon,
                                up.zoom < 0 ? camera->zoom() : up.zoom);
    };
#endif
    // Initial camera placement (consumes FollowCamera's first-fire). With real
    // GPS there is nowhere to point yet — the first valid sample fires it.
    if (mock) {
        if (auto up = camera->on_position(mock->lat(), mock->lon())) cam_sink(*up);
    }

    ui->on_map_zoom([=](int delta) {
#if defined(CYCLOMP_MAP_GL)
        mapgl::zoom_step(delta);
#elif defined(CYCLOMP_HAVE_MAPLIBRE)
        if (map_service && mock)
            map_service->set_camera(mock->lat(), mock->lon(),
                                    camera->adjust_zoom(delta));
#else
        (void)delta;
#endif
    });
#if defined(CYCLOMP_MAP_GL)
    ui->on_map_pan([camera](float dx, float dy) {
        camera->release();
        mapgl::drag_by(dx, dy);
    });
    {
        // Slint reports a cumulative pinch scale; mbgl wants increments.
        auto last = std::make_shared<double>(1.0);
        ui->on_map_pinch_begin([last] { *last = 1.0; });
        ui->on_map_pinch([camera, last](float s, float cx, float cy) {
            if (s <= 0 || *last <= 0) return;
            camera->release();
            mapgl::scale_by(s / *last, cx, cy);
            *last = s;
        });
    }
#endif

    // idle -> start (fresh session) | running -> pause | paused -> resume.
    // The engine owns the state machine; the UI property just mirrors it.
    // Real GPS needs the runtime permission; the first START RIDE asks for it.
    std::function<void()> on_ride_start = [] {};
#ifdef __ANDROID__
    if (gps_source) on_ride_start = [gps_source] { gps_source->request_permission(); };
#endif
    ui->on_toggle_ride(
        [ui = slint::ComponentWeakHandle(ui), mock, engine, camera, sensors,
         publish_nav, on_ride_start] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if (engine->state() == core::RideState::Idle) {
                on_ride_start();
                if (mock) mock->reset_ride();
                publish_idle(w, sensors);
                publish_nav(w);
                camera->relock(); // camera back onto the rider
            }
            w.set_ride_state((int)engine->toggle());
        });

    // Finalize the ride: save a summary row, then return to idle.
    ui->on_stop_ride(
        [ui = slint::ComponentWeakHandle(ui), engine, log, sessions, sensors,
         publish_nav] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if (engine->state() == core::RideState::Idle) return;
            if (auto s = engine->stop(std::time(nullptr))) {
                log->add(*s);
                sessions->insert(0, to_row(*s)); // newest first
            }
            publish_idle(w, sensors);
            publish_nav(w);
            w.set_selected_session(-1);
            w.set_ride_state((int)engine->state());
        });

    // ~500 ms telemetry tick; only advances while RUNNING (pause freezes all).
    static slint::Timer timer;
    timer.start(slint::TimerMode::Repeated, std::chrono::milliseconds(500),
        [ui = slint::ComponentWeakHandle(ui), source, engine, camera, cam_sink,
         sensors, publish_nav] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            // Keeps the GPS status strip live even before the ride starts.
            publish_nav(w);
            if (engine->state() != core::RideState::Running) return;
            auto s = source->sample(0.5);
            engine->tick(0.5, s);
            publish_live(w, engine->live(), sensors);
            if (s.valid)
                if (auto up = camera->on_position(s.lat, s.lon)) cam_sink(*up);
        });

#if defined(CYCLOMP_MAP_GL)
    // Start the map service once the window has a real size (no GL needed).
    static slint::Timer map_start_timer;
    map_start_timer.start(
        slint::TimerMode::Repeated, std::chrono::milliseconds(200),
        [ui = slint::ComponentWeakHandle(ui)] {
            auto u = ui.lock();
            if (!u) return;
            auto sz = (*u)->window().size();
            if (sz.width > 0 && sz.height > 0) {
                mapgl::setup(sz.width, sz.height, 2.75f);
                map_start_timer.stop();
            }
        });
#endif

    ui->run();
#ifndef __ANDROID__
    return 0;
#endif
}
