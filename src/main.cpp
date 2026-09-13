// cyclomp — mock ride driver with session tracking.
// Feeds fake but plausible telemetry into the Slint UI on a repeating timer,
// and turns finished rides into SessionRow entries for the history list.

#include "cyclomp.h"

#include "core/follow_camera.h"
#include "core/format.h"
#include "core/mock_telemetry.h"
#include "core/ride_engine.h"
#include "core/session_store.h"
#include "core/track.h"

#ifdef __ANDROID__
#include <sys/stat.h>
#endif

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
#include <atomic>
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

// Where finished rides live between runs.
std::string sessions_path() {
#ifdef __ANDROID__
    ::mkdir("/data/data/dev.bauhouse.cyclomp/files", 0700); // EEXIST is fine
    return "/data/data/dev.bauhouse.cyclomp/files/sessions.txt";
#else
    const char *h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/.cyclomp_sessions.txt";
#endif
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

    // Where the rider currently is — for the map marker and the LOCATE
    // button. False before the first fix; the mock always knows.
    std::function<bool(double &, double &)> current_position =
        [](double &, double &) { return false; };
    if (mock) {
        current_position = [mock](double &la, double &lo) {
            la = mock->lat();
            lo = mock->lon();
            return true;
        };
    }
#ifdef __ANDROID__
    if (gps_source) {
        current_position = [gps_source](double &la, double &lo) {
            return gps_source->last_position(la, lo);
        };
    }
#endif

    // Hand the rider's position to the map and read back where the map
    // thread projected it. Desktop keeps the built-in centre defaults.
    std::function<void(AppWindow &)> publish_marker = [](AppWindow &) {};
#if defined(CYCLOMP_MAP_GL)
    publish_marker = [current_position](AppWindow &w) {
        double la, lo;
        if (current_position(la, lo)) mapgl::set_marker(la, lo);
        double nx, ny, aspect;
        if (mapgl::marker_offset(nx, ny, aspect)) {
            w.set_marker_nx((float)nx);
            w.set_marker_ny((float)ny);
            w.set_frame_aspect((float)aspect);
            w.set_marker_valid(true);
        } else {
            w.set_marker_valid(false);
        }
    };
#endif

    auto engine = std::make_shared<core::RideEngine>();
    auto log = std::make_shared<core::SessionLog>();
    auto recorder = std::make_shared<core::TrackRecorder>();

    // History of finished rides, newest first; persisted across restarts.
    auto sessions = std::make_shared<slint::VectorModel<SessionRow>>();
    core::load_sessions(sessions_path(), *log);
    for (const auto &s : log->newest_first()) sessions->push_back(to_row(s));
    ui->set_sessions(sessions);

    // Tap on a history row: toggle selection + build its infographics.
    ui->on_select_session([ui = slint::ComponentWeakHandle(ui), log](int i) {
        auto u = ui.lock();
        if (!u) return;
        auto &w = **u;
        if (w.get_selected_session() == i) {
            w.set_selected_session(-1);
            return;
        }
        const auto &rows = log->newest_first();
        if (i < 0 || (size_t)i >= rows.size()) return;
        w.set_sel_speed_path(
            slint::SharedString(core::speed_sparkline_path(rows[i].track, 300, 70)));
        w.set_sel_track_path(
            slint::SharedString(core::track_shape_path(rows[i].track, 300, 120)));
        w.set_selected_session(i);
    });
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
    // Pinch preview: the UI scales the last frame visually during the
    // gesture; on commit mbgl renders once, and the preview is reset only
    // when a frame carrying that command generation arrives (avoids the
    // one-frame "snap back to the old zoom" glitch).
    auto pinch_commit_gen = std::make_shared<std::atomic<uint64_t>>(0);
    mapgl::set_frame_sink([ui = slint::ComponentWeakHandle(ui),
                           publish_marker, pinch_commit_gen](
                              uint32_t idx, uint32_t w, uint32_t h,
                              uint64_t gen) {
        slint::invoke_from_event_loop([ui, idx, w, h, gen, publish_marker,
                                       pinch_commit_gen] {
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
            uint64_t want = pinch_commit_gen->load();
            if (want != 0 && gen >= want) {
                (*u)->set_map_preview_scale(1.f);
                pinch_commit_gen->store(0);
            }
            // The projection belongs to the frame we just published.
            publish_marker(**u);
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
    publish_marker(*ui);

    // LOCATE: fly back to the rider and relock the camera. Nothing to do
    // before the first fix.
    ui->on_map_locate([ui = slint::ComponentWeakHandle(ui), camera, cam_sink,
                       current_position, publish_marker] {
        auto u = ui.lock();
        if (!u) return;
        double la, lo;
        if (!current_position(la, lo)) return;
        camera->relock();
        if (auto up = camera->on_position(la, lo)) cam_sink(*up);
        (*u)->set_map_following(camera->following());
        publish_marker(**u);
    });

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
    ui->on_map_pan([ui = slint::ComponentWeakHandle(ui), camera](float dx,
                                                                 float dy) {
        camera->release();
        mapgl::drag_by(dx, dy);
        if (auto u = ui.lock()) (*u)->set_map_following(false);
    });
    // During the pinch only the Slint-side preview moves (60fps); mbgl gets
    // ONE scale_by on commit. renderStill blocks until every tile of the new
    // zoom level is ready, which made per-update commits stutter.
    ui->on_map_pinch_begin([ui = slint::ComponentWeakHandle(ui), camera] {
        camera->release();
        if (auto u = ui.lock()) (*u)->set_map_following(false);
    });
    ui->on_map_pinch_commit([ui = slint::ComponentWeakHandle(ui),
                             pinch_commit_gen](float s, float cx, float cy) {
        if (s <= 0) return;
        pinch_commit_gen->store(mapgl::scale_by(s, cx, cy));
        // The preview stays applied until the sharp frame lands (see the
        // frame sink); nothing else to do here.
        (void)ui;
    });
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
         publish_nav, on_ride_start, recorder] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if (engine->state() == core::RideState::Idle) {
                on_ride_start();
                if (mock) mock->reset_ride();
                recorder->reset();
#if defined(CYCLOMP_MAP_GL)
                mapgl::set_track({}); // clear the previous ride's line
#endif
                publish_idle(w, sensors);
                publish_nav(w);
                camera->relock(); // camera back onto the rider
                w.set_map_following(true);
            }
            w.set_ride_state((int)engine->toggle());
        });

    // Finalize the ride: save a summary row, then return to idle.
    ui->on_stop_ride(
        [ui = slint::ComponentWeakHandle(ui), engine, log, sessions, sensors,
         publish_nav, recorder] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if (engine->state() == core::RideState::Idle) return;
            if (auto s = engine->stop(std::time(nullptr))) {
                s->track = recorder->points();
                log->add(*s);
                sessions->insert(0, to_row(*s)); // newest first
                core::save_sessions(sessions_path(), *log);
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
         sensors, publish_nav, publish_marker, recorder] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            // Keeps the GPS status strip and the map marker live even before
            // the ride starts.
            publish_nav(w);
            publish_marker(w);
            if (engine->state() != core::RideState::Running) return;
            auto s = source->sample(0.5);
            engine->tick(0.5, s);
            publish_live(w, engine->live(), sensors);
            if (s.valid) {
                recorder->add(s.lat, s.lon, s.speed_kmh,
                              engine->live().elapsed_s);
#if defined(CYCLOMP_MAP_GL)
                // Refresh the route line every other point (~1 Hz).
                if (recorder->points().size() % 2 == 0) {
                    std::vector<std::pair<double, double>> pts;
                    pts.reserve(recorder->points().size());
                    for (const auto &tp : recorder->points())
                        pts.emplace_back(tp.lat, tp.lon);
                    mapgl::set_track(std::move(pts));
                }
#endif
                if (auto up = camera->on_position(s.lat, s.lon)) cam_sink(*up);
            }
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
