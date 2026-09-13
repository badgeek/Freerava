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

// Push the engine's live values into the UI.
void publish_live(AppWindow &w, const core::LiveStats &m, int nav_m) {
    w.set_speed((float)m.speed_kmh);
    w.set_distance(fmt1((float)m.dist_km));
    w.set_ride_time(mmss((int)m.elapsed_s));
    w.set_avg_speed((float)m.avg_kmh);
    w.set_cadence(m.cadence);
    w.set_heart_rate(m.heart_rate);
    w.set_hr_zone(core::hr_zone(m.heart_rate));
    w.set_nav_distance(metres(nav_m));
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
    auto source = std::make_shared<core::MockTelemetrySource>();
    auto engine = std::make_shared<core::RideEngine>();
    auto log = std::make_shared<core::SessionLog>();

    // History of finished rides, newest first. In-memory for the mock.
    auto sessions = std::make_shared<slint::VectorModel<SessionRow>>();
    ui->set_sessions(sessions);
    publish_idle(*ui);

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
    // Initial camera placement (consumes FollowCamera's first-fire).
    if (auto up = camera->on_position(source->lat(), source->lon()))
        cam_sink(*up);

    ui->on_map_zoom([=](int delta) {
#if defined(CYCLOMP_MAP_GL)
        mapgl::zoom_step(delta);
#elif defined(CYCLOMP_HAVE_MAPLIBRE)
        if (map_service)
            map_service->set_camera(source->lat(), source->lon(),
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
    ui->on_toggle_ride(
        [ui = slint::ComponentWeakHandle(ui), source, engine, camera] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if (engine->state() == core::RideState::Idle) {
                source->reset_ride();
                publish_idle(w);
                camera->relock(); // camera back onto the rider
            }
            w.set_ride_state((int)engine->toggle());
        });

    // Finalize the ride: save a summary row, then return to idle.
    ui->on_stop_ride(
        [ui = slint::ComponentWeakHandle(ui), engine, log, sessions] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if (engine->state() == core::RideState::Idle) return;
            if (auto s = engine->stop(std::time(nullptr))) {
                log->add(*s);
                sessions->insert(0, to_row(*s)); // newest first
            }
            publish_idle(w);
            w.set_selected_session(-1);
            w.set_ride_state((int)engine->state());
        });

    // ~500 ms telemetry tick; only advances while RUNNING (pause freezes all).
    static slint::Timer timer;
    timer.start(slint::TimerMode::Repeated, std::chrono::milliseconds(500),
        [ui = slint::ComponentWeakHandle(ui), source, engine, camera,
         cam_sink] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if (engine->state() != core::RideState::Running) return;
            auto s = source->sample(0.5);
            engine->tick(0.5, s);
            publish_live(w, engine->live(), source->nav_m());
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
