// cyclomp — mock ride driver with session tracking.
// Feeds fake but plausible telemetry into the Slint UI on a repeating timer,
// and turns finished rides into SessionRow entries for the history list.

#include "cyclomp.h"

#include "core/follow_camera.h"
#include "core/format.h"
#include "core/geojson.h"
#include "core/mock_telemetry.h"
#include "core/ride_engine.h"
#include "core/session_db.h"
#include "core/session_store.h"
#include "core/settings.h"
#include "core/track.h"

#ifdef __ANDROID__
#include "camera_android.h"
#include "compass_android.h"
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
#include <thread>

#ifdef __ANDROID__
#include <android/log.h>
#include <dlfcn.h>
#include <exception>
#include <unwind.h>
#endif

namespace {

#ifdef __ANDROID__
// Temporary crash triage: dump a library-relative backtrace on terminate so
// the throwing frame (which MIUI's tombstone truncates at abort_message) is
// symbolizable with llvm-addr2line against the unstripped libcyclomp.so.
struct BtState { void **cur; void **end; };
_Unwind_Reason_Code cyclomp_bt_cb(_Unwind_Context *ctx, void *arg) {
    auto *st = static_cast<BtState *>(arg);
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && st->cur != st->end) *st->cur++ = reinterpret_cast<void *>(pc);
    return _URC_NO_REASON;
}
void cyclomp_terminate() {
    void *pcs[64];
    BtState st{pcs, pcs + 64};
    _Unwind_Backtrace(&cyclomp_bt_cb, &st);
    size_t n = (size_t)(st.cur - pcs);
    __android_log_print(ANDROID_LOG_FATAL, "cyclomp-crash",
                        "uncaught exception; %zu frames", n);
    Dl_info info;
    for (size_t i = 0; i < n; ++i) {
        uintptr_t pc = (uintptr_t)pcs[i], off = 0;
        const char *lib = "?";
        if (dladdr(pcs[i], &info) && info.dli_fbase) {
            off = pc - (uintptr_t)info.dli_fbase;
            lib = info.dli_fname ? info.dli_fname : "?";
        }
        __android_log_print(ANDROID_LOG_FATAL, "cyclomp-crash",
                            "#%02zu off=0x%zx %s", i, off, lib);
    }
    std::abort();
}
#endif

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

// Where finished rides live between runs (legacy text log; still read once to
// migrate old data into the SQLite DB below).
std::string sessions_path() {
#ifdef __ANDROID__
    ::mkdir("/data/data/dev.bauhouse.cyclomp/files", 0700); // EEXIST is fine
    return "/data/data/dev.bauhouse.cyclomp/files/sessions.txt";
#else
    const char *h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/.cyclomp_sessions.txt";
#endif
}

// SQLite store: finished rides + the single in-progress ride.
std::string db_path() {
#ifdef __ANDROID__
    ::mkdir("/data/data/dev.bauhouse.cyclomp/files", 0700);
    return "/data/data/dev.bauhouse.cyclomp/files/cyclomp.db";
#else
    const char *h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/.cyclomp.db";
#endif
}

// Per-ride GeoJSON export. Android: an app-files subfolder (pull with
// `adb shell run-as dev.bauhouse.cyclomp cat files/exports/<name>`); desktop:
// the home dir. Returns the full path for the given ride timestamp.
std::string export_path(std::time_t ended_at, std::string *name_out = nullptr) {
    std::string name = "ride_" + std::to_string((long long)ended_at) + ".geojson";
    if (name_out) *name_out = name;
#ifdef __ANDROID__
    ::mkdir("/data/data/dev.bauhouse.cyclomp/files", 0700);
    ::mkdir("/data/data/dev.bauhouse.cyclomp/files/exports", 0700);
    return "/data/data/dev.bauhouse.cyclomp/files/exports/" + name;
#else
    const char *h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/cyclomp_" + name;
#endif
}

// Ride selfies land in an app-private folder (no storage permission needed).
// Named by capture time so they sort, and so a later photo->track join can be
// done on the timestamp alone.
std::string photo_path(std::time_t when) {
#ifdef __ANDROID__
    ::mkdir("/data/data/dev.bauhouse.cyclomp/files", 0700);
    ::mkdir("/data/data/dev.bauhouse.cyclomp/files/photos", 0700);
    return "/data/data/dev.bauhouse.cyclomp/files/photos/selfie_"
           + std::to_string((long long)when) + ".jpg";
#else
    const char *h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/cyclomp_selfie_"
           + std::to_string((long long)when) + ".jpg";
#endif
}

// Set from slint_main so the interposed onPause can flush the active ride
// before Android suspends/kills us in the background. Runs on the main thread
// (same as the telemetry timer), so it shares state without locking.
std::function<void()> g_flush_active = [] {};

// Where the tunable parameters live between runs.
std::string settings_path() {
#ifdef __ANDROID__
    ::mkdir("/data/data/dev.bauhouse.cyclomp/files", 0700); // EEXIST is fine
    return "/data/data/dev.bauhouse.cyclomp/files/settings.txt";
#else
    const char *h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/.cyclomp_settings.txt";
#endif
}

// Refresh the SETTINGS screen's value strings from the current values.
void publish_settings(AppWindow &w, const core::Settings &s) {
    w.set_set_zoom_value(fmt0((float)s.follow_zoom));
    w.set_set_heading_speed_value(fmt0((float)s.heading_speed_kmh));
    w.set_set_bearing_delta_value(fmt0((float)s.bearing_min_delta_deg));
    w.set_set_tunnel_base_value(fmt1((float)s.tunnel_base));
    w.set_set_tunnel_cap_value(fmt1((float)s.tunnel_cap));
    w.set_set_tunnel_fps_value(fmt0((float)s.tunnel_fps));
    w.set_set_compass_rate_value(fmt1((float)s.compass_interval_s));
    w.set_set_road_bright_value(fmt0((float)s.road_brightness));
    w.set_set_tunnel_on_value(slint::SharedString(s.tunnel_enabled ? "ON" : "OFF"));
    w.set_set_mock_value(slint::SharedString(s.mock_ride ? "ON" : "OFF"));
    // The tunnel animation consumes the numeric values directly.
    w.set_tunnel_base((float)s.tunnel_base);
    w.set_tunnel_cap((float)s.tunnel_cap);
    w.set_tunnel_fps((float)s.tunnel_fps);
    w.set_tunnel_enabled(s.tunnel_enabled);
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
#include <unistd.h>

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

namespace {
// Chain onto Slint's own onPause so we flush the in-progress ride the instant
// we lose the foreground (e.g. the user opens a QRIS/banking app) — before
// Android may kill the backgrounded process. Slint's callback still runs.
void (*g_prev_on_pause)(ANativeActivity *) = nullptr;
void cyclomp_on_pause(ANativeActivity *a) {
    g_flush_active();
    if (g_prev_on_pause) g_prev_on_pause(a);
}

// BACK finishes the activity but Android KEEPS the process — and with it this
// .so and every global in it. (HOME only pauses, which is why HOME was fine.)
// The Slint runtime cannot start twice in one process: android-activity runs
// slint_main on a fresh thread for the recreated activity, while Slint's
// assert_main_thread() has cached the FIRST thread's id in a function-local
// static that outlived the activity — so the second AppWindow::create() hits
// `std::abort()` (SIGABRT in slint::private_api::assert_main_thread).
// onPause has already flushed the in-progress ride to SQLite by now, so let
// the process end with the activity; the next launch restores from disk.
void (*g_prev_on_destroy)(ANativeActivity *) = nullptr;
void cyclomp_on_destroy(ANativeActivity *a) {
    if (g_prev_on_destroy) g_prev_on_destroy(a); // joins Slint's app thread
    _exit(0);                                    // skip static dtors; state is on disk
}
} // namespace

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
    // Slint has now installed its lifecycle callbacks; wrap onPause/onDestroy.
    if (activity->callbacks && activity->callbacks->onPause != &cyclomp_on_pause) {
        g_prev_on_pause = activity->callbacks->onPause;
        activity->callbacks->onPause = &cyclomp_on_pause;
    }
    if (activity->callbacks && activity->callbacks->onDestroy != &cyclomp_on_destroy) {
        g_prev_on_destroy = activity->callbacks->onDestroy;
        activity->callbacks->onDestroy = &cyclomp_on_destroy;
    }
}
#endif

#ifdef __ANDROID__
extern "C" void slint_main()
#else
int main(int, char **)
#endif
{
#ifdef __ANDROID__
    std::set_terminate(&cyclomp_terminate);
#endif
    auto ui = AppWindow::create();

    // Tunables (SETTINGS screen). Loaded before anything consumes them.
    auto settings = std::make_shared<core::Settings>();
    core::load_settings(settings_path(), *settings);
    publish_settings(*ui, *settings);
#if defined(CYCLOMP_MAP_GL)
    mapgl::set_road_brightness((int)std::lround(settings->road_brightness));
#endif

    // Telemetry: real GPS on Android, the RNG mock everywhere else.
    // CYCLOMP_MOCK_RIDE=1 or the settings toggle forces the mock on Android
    // too (demo mode) — env vars can't be injected via adb, settings can.
    std::shared_ptr<core::MockTelemetrySource> mock;
    std::shared_ptr<core::TelemetrySource> source;
#ifdef __ANDROID__
    std::shared_ptr<gps::AndroidTelemetrySource> gps_source;
    const char *mock_env = std::getenv("CYCLOMP_MOCK_RIDE");
    if (!((mock_env && mock_env[0] == '1') || settings->mock_ride)) {
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

    // While a ride replay runs, the replay owns the map marker; the live
    // rider position must not overwrite it.
    auto replay_active = std::make_shared<std::atomic<bool>>(false);

    // Hand the rider's position to the map and read back where the map
    // thread projected it. Desktop keeps the built-in centre defaults.
    std::function<void(AppWindow &)> publish_marker = [](AppWindow &) {};
#if defined(CYCLOMP_MAP_GL)
    publish_marker = [current_position, replay_active](AppWindow &w) {
        double la, lo;
        if (!replay_active->load() && current_position(la, lo))
            mapgl::set_marker(la, lo);
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

    // SQLite persistence. First run after the text era: migrate the old
    // sessions.txt in, then rename it aside so we don't import it twice.
    auto store = std::make_shared<core::SessionStore>();
    store->open(db_path());
    if (store->ok() && store->session_count() == 0) {
        if (store->import_text_log(sessions_path()) > 0)
            std::rename(sessions_path().c_str(),
                        (sessions_path() + ".imported").c_str());
    }

    // History of finished rides, newest first; persisted across restarts.
    auto sessions = std::make_shared<slint::VectorModel<SessionRow>>();
    if (store->ok())
        store->load_all(*log);
    else
        core::load_sessions(sessions_path(), *log); // DB unavailable: fall back
    for (const auto &s : log->newest_first()) sessions->push_back(to_row(s));
    ui->set_sessions(sessions);

    // Persist the in-progress ride so a HOLD that gets backgrounded (paying
    // QRIS, a call) survives the process being killed. Snapshot = engine state
    // + track so far; cleared when the ride ends.
    auto save_active = [engine, recorder, store] {
        if (!store->ok()) return;
        if (engine->state() == core::RideState::Idle) {
            store->clear_active();
            return;
        }
        core::ActiveRide a;
        a.engine = engine->snapshot();
        a.track = recorder->points();
        a.saved_at = std::time(nullptr);
        store->save_active(a);
    };
    g_flush_active = save_active; // interposed onPause flushes through this

    // Resume an interrupted ride left over from a previous run.
    core::ActiveRide resumed;
    if (store->ok() && store->load_active(resumed)) {
        engine->restore(resumed.engine);
        recorder->restore(std::move(resumed.track));
        auto &w = *ui;
        w.set_ride_state((int)engine->state());
        publish_live(w, engine->live(), sensors);
        w.set_live_elev_path(slint::SharedString(
            core::elevation_profile_path(recorder->points(), 160, 60)));
        w.set_live_elev_label(slint::SharedString(
            "ELEV +" +
            core::fmt::fmt0(core::elevation_gain_m(recorder->points())) + " m"));
#if defined(CYCLOMP_MAP_GL)
        std::vector<std::pair<double, double>> pts;
        pts.reserve(recorder->points().size());
        for (const auto &tp : recorder->points())
            pts.emplace_back(tp.lat, tp.lon);
        if (pts.size() >= 2) mapgl::set_track(std::move(pts));
#endif
    }

    // Tap on a history row: open the ride-detail page (screen 3).
    ui->on_select_session([ui = slint::ComponentWeakHandle(ui), log](int i) {
        auto u = ui.lock();
        if (!u) return;
        auto &w = **u;
        const auto &rows = log->newest_first();
        if (i < 0 || (size_t)i >= rows.size()) return;
        // 300x200 viewbox: the charts now split the tab's full height, and a
        // 300x70 one stretched into that box exaggerated every spike.
        w.set_sel_speed_path(slint::SharedString(
            core::speed_sparkline_path(rows[i].track, 300, 200)));
        w.set_sel_elev_path(slint::SharedString(
            core::elevation_profile_path(rows[i].track, 300, 200)));
        // Value only ("+49 m"); the UI supplies the "ELEV" prefix, so the same
        // string serves both the chart caption and the PERFORMANCE grid cell.
        w.set_sel_elev_label(slint::SharedString(
            "+" + core::fmt::fmt0(core::elevation_gain_m(rows[i].track)) + " m"));
        w.set_selected_session(i);
        w.set_detail_tab(0);
        w.set_replay_playing(false);
        w.set_replay_progress(0.f);
        w.set_export_status(slint::SharedString(""));
        // Defer the screen switch. This callback runs inside the history
        // ListView's own pointer-event dispatch; switching screen here would
        // destroy screen 2 (the ListView) synchronously, and Slint then walks
        // the now-orphaned list's parent_node -> empty parent weak ->
        // std::bad_optional_access (device-only; the emulator's event ordering
        // happened to hide it). Hop to the next event-loop turn so the click
        // finishes with the list still alive, then tear it down cleanly.
        slint::invoke_from_event_loop([ui] {
            if (auto u = ui.lock()) (**u).set_screen(3);
        });
    });

    // Heading-up mode flag (toggled from the MAP screen; also drives the
    // replay flyover). last_course remembers the GPS course; last_sent
    // throttles bearing updates.
    auto heading_up = std::make_shared<bool>(false);
    auto last_course = std::make_shared<std::optional<double>>();
    auto last_sent_bearing = std::make_shared<double>(-999.0);
    // Seconds since the last rotation sent to the map (compass rate limit).
    auto compass_elapsed = std::make_shared<double>(0.0);

    // Detail-map 2.5D "chase camera": tilt + rotate to the biker's replayed
    // course. Deliberately SEPARATE from `heading_up` (the main MAP compass)
    // so toggling one never touches the other.
    auto detail_chase = std::make_shared<bool>(false);
    static constexpr double kChasePitch = 55.0; // degrees toward the horizon

    // 2.5D on the LIVE MAP screen — a SEPARATE flag from `detail_chase` so the
    // two map views stay independent (they share one physical camera, but only
    // one map screen is visible at a time and each re-applies its own pitch).
    // `map_pitch_applied` tracks what we last pushed so the tick only re-sends
    // on change (and forces a re-apply when the MAP screen is re-entered).
    auto map_3d = std::make_shared<bool>(false);
    auto map_pitch_applied = std::make_shared<double>(-1.0);

    // ---- Ride replay (detail page, MAP tab) ----
    struct Replay {
        std::vector<core::TrackPoint> trk;
        double t = 0, total = 0;
        int speedIdx = 1; // 10x / 30x / 60x
        double lastLat = 0, lastLon = 0, lastBearing = 0;
        bool haveLast = false;    // have a previous position (for the course)
        bool haveBearing = false; // have sent a chase bearing at least once
    };
    static constexpr int kReplaySpeeds[3] = {10, 30, 60};
    auto rp = std::make_shared<Replay>();
    static slint::Timer replay_timer;

    auto replay_tick = [ui = slint::ComponentWeakHandle(ui), rp,
                        replay_active, detail_chase] {
        auto u = ui.lock();
        if (!u) return;
        auto &w = **u;
        const auto &T = rp->trk;
        if (T.size() < 2 || rp->total <= 0) return;
        rp->t = std::min(rp->t + 0.1 * kReplaySpeeds[rp->speedIdx], rp->total);
        size_t j = 1;
        while (j < T.size() && T[j].t_s < rp->t) ++j;
        double lat, lon;
        if (j >= T.size()) {
            lat = T.back().lat;
            lon = T.back().lon;
        } else {
            const auto &a = T[j - 1];
            const auto &b = T[j];
            double f = std::clamp(
                (rp->t - a.t_s) / std::max(1e-6, (double)(b.t_s - a.t_s)),
                0.0, 1.0);
            lat = a.lat + f * (b.lat - a.lat);
            lon = a.lon + f * (b.lon - a.lon);
        }
#if defined(CYCLOMP_MAP_GL)
        mapgl::set_marker(lat, lon);
        // Chase camera: rotate the (already tilted) map along the replayed
        // course. Gated on the detail-only flag, never the main-map compass.
        if (*detail_chase && rp->haveLast) {
            double la1 = rp->lastLat * M_PI / 180.0;
            double la2 = lat * M_PI / 180.0;
            double dlo = (lon - rp->lastLon) * M_PI / 180.0;
            double yb = std::sin(dlo) * std::cos(la2);
            double xb = std::cos(la1) * std::sin(la2) -
                        std::sin(la1) * std::cos(la2) * std::cos(dlo);
            if (yb != 0.0 || xb != 0.0) {
                double c = std::atan2(yb, xb) * 180.0 / M_PI;
                if (c < 0) c += 360.0;
                // Normalise the delta with fmod BEFORE the >180 wrap so it
                // survives any lastBearing value (the -999 sentinel trap).
                double d = std::fmod(std::fabs(c - rp->lastBearing), 360.0);
                if (d > 180.0) d = 360.0 - d;
                if (!rp->haveBearing || d > 5.0) { // throttle small wiggles
                    mapgl::set_bearing(c);
                    rp->lastBearing = c;
                    rp->haveBearing = true;
                }
            }
        }
        rp->lastLat = lat;
        rp->lastLon = lon;
        rp->haveLast = true;
        double nx, ny, aspect;
        if (mapgl::marker_offset(nx, ny, aspect)) {
            w.set_marker_nx((float)nx);
            w.set_marker_ny((float)ny);
            w.set_frame_aspect((float)aspect);
            w.set_marker_valid(true);
        }
#else
        (void)lat;
        (void)lon;
#endif
        w.set_replay_progress((float)(rp->t / rp->total));
        if (rp->t >= rp->total) {
            replay_timer.stop();
            replay_active->store(false);
            w.set_replay_playing(false);
        }
    };

    // MAP tab opened: show this ride's line, fit the camera, park the
    // marker at the start.
    ui->on_detail_map_shown([ui = slint::ComponentWeakHandle(ui), log, rp,
                             replay_active, detail_chase] {
        auto u = ui.lock();
        if (!u) return;
        auto &w = **u;
        replay_timer.stop();
        replay_active->store(false);
        w.set_replay_playing(false);
        w.set_replay_progress(0.f);
        // Enter the detail map flat and top-down, regardless of what the main
        // MAP compass left the shared camera at. Chase defaults off.
        *detail_chase = false;
        w.set_detail_chase(false);
#if defined(CYCLOMP_MAP_GL)
        mapgl::set_bearing(0);
        mapgl::set_pitch(0);
#endif
        int i = w.get_selected_session();
        const auto &rows = log->newest_first();
        if (i < 0 || (size_t)i >= rows.size()) return;
        rp->trk = rows[i].track;
        rp->total = rp->trk.empty() ? 0.0 : (double)rp->trk.back().t_s;
        rp->t = 0;
        rp->haveLast = false;
        rp->haveBearing = false;
        rp->lastBearing = 0;
#if defined(CYCLOMP_MAP_GL)
        if (rp->trk.size() >= 2) {
            std::vector<std::pair<double, double>> pts;
            pts.reserve(rp->trk.size());
            double lat0 = rp->trk[0].lat, lat1 = lat0;
            double lon0 = rp->trk[0].lon, lon1 = lon0;
            for (const auto &p : rp->trk) {
                pts.emplace_back(p.lat, p.lon);
                lat0 = std::min(lat0, p.lat);
                lat1 = std::max(lat1, p.lat);
                lon0 = std::min(lon0, p.lon);
                lon1 = std::max(lon1, p.lon);
            }
            mapgl::set_track(std::move(pts));
            // Fit: web-mercator zoom from the bbox vs the (logical) view.
            auto sz = w.window().size();
            double lw = sz.width / 2.75, lh = sz.height / 2.75;
            double k = std::cos((lat0 + lat1) / 2.0 * M_PI / 180.0);
            double zx = std::log2(360.0 / std::max((lon1 - lon0) * k, 1e-4) *
                                  lw / 512.0);
            double zy = std::log2(180.0 / std::max(lat1 - lat0, 1e-4) *
                                  lh / 512.0);
            // -0.9: comfortable margin so the track clears the replay bar
            // and the header instead of hugging the view edges.
            double z = std::clamp(std::min(zx, zy) - 0.9, 3.0, 19.0);
            mapgl::set_camera((lat0 + lat1) / 2.0, (lon0 + lon1) / 2.0, z);
            mapgl::set_marker(rp->trk.front().lat, rp->trk.front().lon);
            // Static start/stop bullets for the idle (non-replay) view.
            mapgl::set_endpoints(rp->trk.front().lat, rp->trk.front().lon,
                                 rp->trk.back().lat, rp->trk.back().lon);
        }
        w.set_track_endpoints(false); // shown once the map thread projects them
        w.set_marker_valid(false);    // the moving dot only appears in replay
#endif
    });

    ui->on_replay_toggle([ui = slint::ComponentWeakHandle(ui), rp,
                          replay_active, replay_tick] {
        auto u = ui.lock();
        if (!u) return;
        auto &w = **u;
        if (replay_active->load()) {
            replay_timer.stop();
            replay_active->store(false);
            w.set_replay_playing(false);
            return;
        }
        if (rp->trk.size() < 2 || rp->total <= 0) return;
        if (rp->t >= rp->total) rp->t = 0;
        replay_active->store(true);
        w.set_replay_playing(true);
        replay_timer.start(slint::TimerMode::Repeated,
                           std::chrono::milliseconds(100), replay_tick);
    });

    ui->on_replay_speed_cycle([ui = slint::ComponentWeakHandle(ui), rp] {
        rp->speedIdx = (rp->speedIdx + 1) % 3;
        if (auto u = ui.lock())
            (*u)->set_replay_speed_label(slint::SharedString(
                std::to_string(kReplaySpeeds[rp->speedIdx]) + "×"));
    });

    // 2.5D chase-camera toggle (detail map only). ON = tilt the camera and
    // let the replay flyover rotate to the biker's course; OFF = flat top-down
    // north-up. Never touches the main-map compass (`heading_up`).
    ui->on_detail_chase_toggle([ui = slint::ComponentWeakHandle(ui), rp,
                                detail_chase] {
        auto u = ui.lock();
        if (!u) return;
        *detail_chase = !*detail_chase;
        (*u)->set_detail_chase(*detail_chase);
#if defined(CYCLOMP_MAP_GL)
        if (*detail_chase) {
            mapgl::set_pitch(kChasePitch);
            rp->haveBearing = false; // force the next flyover tick to rotate
        } else {
            mapgl::set_pitch(0);
            mapgl::set_bearing(0); // back to flat north-up
        }
#endif
    });

    // Export the selected ride to a GeoJSON file (feeds the Three.js viewer).
    ui->on_export_ride([ui = slint::ComponentWeakHandle(ui), log, store] {
        auto u = ui.lock();
        if (!u) return;
        auto &w = **u;
        int i = w.get_selected_session();
        const auto &rows = log->newest_first();
        if (i < 0 || (size_t)i >= rows.size()) return;
        std::string name;
        std::string path = export_path(rows[i].ended_at, &name);
        std::vector<core::Photo> shots;
        if (store->ok() && rows[i].id > 0) store->photos_for(rows[i].id, shots);
        std::string js = core::session_to_geojson(rows[i], shots);
        std::FILE *f = std::fopen(path.c_str(), "w");
        if (f && std::fwrite(js.data(), 1, js.size(), f) == js.size()) {
            std::fclose(f);
            w.set_export_status(slint::SharedString("SAVED " + name));
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_INFO, "cyclomp-export",
                                "wrote %s (%zu bytes)", path.c_str(), js.size());
#endif
        } else {
            if (f) std::fclose(f);
            w.set_export_status(slint::SharedString("EXPORT FAILED"));
        }
    });

    // Delete the selected sortie, for good. The UI already armed and confirmed
    // (PURGE -> CONFIRM), so this does not second-guess the tap.
    ui->on_purge_ride([ui = slint::ComponentWeakHandle(ui), log, sessions,
                       store] {
        auto u = ui.lock();
        if (!u) return;
        int i = (*u)->get_selected_session();
        const auto &rows = log->newest_first();
        if (i < 0 || (size_t)i >= rows.size()) return;
        long long id = rows[i].id; // 0 when it was never persisted
        // Same hop as on_select_session: we are inside the PURGE button's own
        // pointer-event dispatch, and both leaving the page and shrinking the
        // model destroy the element we were clicked from. Do it next turn.
        slint::invoke_from_event_loop([ui, i, id, log, sessions, store] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if ((size_t)i >= log->newest_first().size()) return;
            w.invoke_detail_back();     // replay/marker teardown + back to LOG
            w.set_selected_session(-1); // the index is about to move
            if (store->ok()) store->delete_session(id);
            log->remove((size_t)i);
            sessions->erase((size_t)i);
            if (!store->ok()) core::save_sessions(sessions_path(), *log);
        });
    });

    // Leave the detail page: stop the replay, hand the marker back to the
    // live rider.
    ui->on_detail_back([ui = slint::ComponentWeakHandle(ui), replay_active,
                        current_position, detail_chase] {
        auto u = ui.lock();
        if (!u) return;
        replay_timer.stop();
        replay_active->store(false);
        (*u)->set_replay_playing(false);
        *detail_chase = false;
        (*u)->set_detail_chase(false);
        (*u)->set_track_endpoints(false);
        (*u)->set_screen(2);
#if defined(CYCLOMP_MAP_GL)
        mapgl::clear_endpoints();
        mapgl::set_bearing(0); // leave the detail page flat north-up
        mapgl::set_pitch(0);
        double la, lo;
        if (current_position(la, lo)) mapgl::set_marker(la, lo);
#else
        (void)current_position;
#endif
    });
    // Don't clobber a ride resumed above; only reset the view when truly idle.
    if (engine->state() == core::RideState::Idle) publish_idle(*ui, sensors);
    publish_nav(*ui);

    // Debug: start on a given screen (0 ride / 1 map / 2 history).
    if (const char *s = std::getenv("CYCLOMP_SCREEN"))
        ui->set_screen(std::atoi(s));

    // ---- Camera: core follow-mode decisions + a sink to the platform map ----
    auto camera = std::make_shared<core::FollowCamera>(settings->follow_zoom);
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
                // App switch: the GL context dies with the surface — forget
                // the imported textures so resume re-imports them.
                if (st == slint::RenderingState::RenderingTeardown) {
                    mapgl::ui_reset();
                    return;
                }
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

    // SETTINGS: step / toggle / per-parameter reset (dir 0), then persist.
    // A follow-zoom change is pushed to the map right away while following.
    auto apply_settings = [ui = slint::ComponentWeakHandle(ui), settings,
                           camera, cam_sink, current_position](bool zoom_changed) {
        *settings = core::clamp_settings(*settings);
        core::save_settings(settings_path(), *settings);
        auto u = ui.lock();
        if (!u) return;
        publish_settings(**u, *settings);
#if defined(CYCLOMP_MAP_GL)
        mapgl::set_road_brightness((int)std::lround(settings->road_brightness));
#endif
        double la, lo;
        if (zoom_changed && camera->following() && current_position(la, lo))
            cam_sink({la, lo, settings->follow_zoom});
    };
    ui->on_setting_adjust([settings, apply_settings](int id, int dir) {
        const core::Settings def;
        switch (id) {
        case 0:
            settings->follow_zoom =
                dir == 0 ? def.follow_zoom : settings->follow_zoom + dir;
            break;
        case 1:
            settings->heading_speed_kmh =
                dir == 0 ? def.heading_speed_kmh
                         : settings->heading_speed_kmh + dir;
            break;
        case 2:
            settings->bearing_min_delta_deg =
                dir == 0 ? def.bearing_min_delta_deg
                         : settings->bearing_min_delta_deg + dir;
            break;
        case 3:
            settings->mock_ride = dir == 0 ? def.mock_ride : !settings->mock_ride;
            break;
        case 4:
            settings->tunnel_base =
                dir == 0 ? def.tunnel_base : settings->tunnel_base + dir * 0.1;
            break;
        case 5:
            settings->tunnel_cap =
                dir == 0 ? def.tunnel_cap : settings->tunnel_cap + dir * 0.1;
            break;
        case 6:
            settings->tunnel_fps =
                dir == 0 ? def.tunnel_fps : settings->tunnel_fps + dir * 5.0;
            break;
        case 7:
            settings->tunnel_enabled =
                dir == 0 ? def.tunnel_enabled : !settings->tunnel_enabled;
            break;
        case 8:
            settings->compass_interval_s =
                dir == 0 ? def.compass_interval_s
                         : settings->compass_interval_s + dir * 0.5;
            break;
        case 9:
            settings->road_brightness =
                dir == 0 ? def.road_brightness
                         : settings->road_brightness + dir * 5.0;
            break;
        }
        apply_settings(id == 0);
    });
    ui->on_settings_reset([settings, apply_settings] {
        *settings = core::Settings{};
        apply_settings(true);
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
    // Heading-up: rotate the camera to the bike's course while following.
    ui->on_map_heading_toggle([ui = slint::ComponentWeakHandle(ui),
                               heading_up] {
        auto u = ui.lock();
        if (!u) return;
        *heading_up = !*heading_up;
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_INFO, "cyclomp-compass",
                            "heading-up toggled %s", *heading_up ? "ON" : "OFF");
#endif
        (*u)->set_map_heading_up(*heading_up);
#if defined(CYCLOMP_MAP_GL)
        if (!*heading_up) mapgl::set_bearing(0); // back to north-up
#ifdef __ANDROID__
        if (*heading_up) compass::start();
#endif
#endif
    });

    // 2.5D toggle on the LIVE MAP screen. Independent of the detail-map chase.
    ui->on_map_3d_toggle([ui = slint::ComponentWeakHandle(ui), map_3d,
                          map_pitch_applied] {
        auto u = ui.lock();
        if (!u) return;
        *map_3d = !*map_3d;
        (*u)->set_map_3d(*map_3d);
#if defined(CYCLOMP_MAP_GL)
        double want = *map_3d ? kChasePitch : 0.0;
        mapgl::set_pitch(want);
        *map_pitch_applied = want;
#else
        (void)map_pitch_applied;
#endif
    });

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
         publish_nav, on_ride_start, recorder, save_active] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if (engine->state() == core::RideState::Idle) {
                on_ride_start();
                if (mock) mock->reset_ride();
                recorder->reset();
                w.set_live_elev_path(slint::SharedString(""));
                w.set_live_elev_label(slint::SharedString("ELEV +0 m"));
#if defined(CYCLOMP_MAP_GL)
                mapgl::set_track({}); // clear the previous ride's line
#endif
                publish_idle(w, sensors);
                publish_nav(w);
                camera->relock(); // camera back onto the rider
                w.set_map_following(true);
            }
            w.set_ride_state((int)engine->toggle());
            // Persist immediately: START, and especially HOLD (the moment
            // before the user switches to another app).
            save_active();
        });

    // ---- Ride selfie ----------------------------------------------------
    // selfie::capture blocks for a few hundred ms while the HAL converges and
    // delivers, so it runs on a detached thread; the result hops back to the
    // Slint loop because touching window state off-thread aborts.
#ifdef __ANDROID__
    ui->set_camera_available(selfie::available());
    ui->on_take_photo([ui = slint::ComponentWeakHandle(ui), current_position,
                       store, engine] {
        auto u = ui.lock();
        if (!u) return;
        (*u)->set_photo_status(slint::SharedString("CAPTURING..."));

        // Stamp the position at the moment of the shutter, not when the frame
        // lands — by then you have moved.
        double la = 0, lo = 0;
        const bool fixed = current_position(la, lo);
        const std::time_t when = std::time(nullptr);
        const std::string path = photo_path(when);
        // Elapsed ride time is what places the photo on the track later when
        // there was no fix at the shutter.
        const double t_s = engine->live().elapsed_s;

        std::thread([ui, path, when, la, lo, fixed, t_s, store] {
            selfie::capture(path, [&](selfie::Shot shot) {
                std::string msg;
                if (shot.ok) {
                    char buf[128];
                    if (fixed)
                        std::snprintf(buf, sizeof buf, "SAVED %dx%d @ %.5f,%.5f",
                                      shot.width, shot.height, la, lo);
                    else
                        std::snprintf(buf, sizeof buf, "SAVED %dx%d (NO FIX)",
                                      shot.width, shot.height);
                    msg = buf;
                    __android_log_print(ANDROID_LOG_INFO, "cyclomp-camera",
                                        "selfie %s lat=%.6f lon=%.6f fix=%d t=%lld",
                                        path.c_str(), la, lo, (int)fixed,
                                        (long long)when);
                    // Row first, file already on disk: a photo the DB never
                    // heard about would silently miss every future export.
                    if (store->ok()) {
                        core::Photo rec;
                        rec.taken_at = when;
                        rec.lat = la; rec.lon = lo; rec.has_fix = fixed;
                        rec.t_s = t_s; rec.path = path;
                        if (!store->add_photo(rec))
                            __android_log_print(ANDROID_LOG_ERROR, "cyclomp-camera",
                                                "photo saved but NOT recorded: %s",
                                                path.c_str());
                    }
                } else {
                    msg = "FAILED: " + shot.error;
                }
                slint::invoke_from_event_loop([ui, msg] {
                    if (auto u = ui.lock())
                        (*u)->set_photo_status(slint::SharedString(msg));
                });
            });
        }).detach();
    });
#endif

    // Finalize the ride: save a summary row, then return to idle.
    ui->on_stop_ride(
        [ui = slint::ComponentWeakHandle(ui), engine, log, sessions, sensors,
         publish_nav, recorder, store] {
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            if (engine->state() == core::RideState::Idle) return;
            if (auto s = engine->stop(std::time(nullptr))) {
                s->track = recorder->points();
                // Store first: add_session stamps the new rowid onto `s`, and
                // the log's copy needs it to be purgeable without a restart.
                if (store->ok()) {
                    store->add_session(*s); // atomic append, no full rewrite
                    // Selfies were stored unbound while riding (the session had
                    // no rowid yet); hand them over now.
                    if (s->id > 0) store->bind_photos(s->id);
                }
                log->add(*s);
                sessions->insert(0, to_row(*s)); // newest first
                if (!store->ok()) core::save_sessions(sessions_path(), *log);
            }
            if (store->ok()) store->clear_active(); // ride finished
            publish_idle(w, sensors);
            publish_nav(w);
            w.set_selected_session(-1);
            w.set_ride_state((int)engine->state());
        });

    // ~500 ms telemetry tick; only advances while RUNNING (pause freezes all).
    static slint::Timer timer;
    timer.start(slint::TimerMode::Repeated, std::chrono::milliseconds(500),
        [ui = slint::ComponentWeakHandle(ui), source, engine, camera, cam_sink,
         sensors, publish_nav, publish_marker, recorder, heading_up,
         last_course, last_sent_bearing, replay_active, current_position,
         settings, compass_elapsed, map_3d, map_pitch_applied, save_active] {
            (void)compass_elapsed;
            (void)settings; // consumed only in the Android heading-up block
            (void)map_3d;
            (void)map_pitch_applied;
            auto u = ui.lock();
            if (!u) return;
            auto &w = **u;
            // Keeps the GPS status strip and the map marker live even before
            // the ride starts. On the detail page (screen 3) the map belongs
            // to the replay/endpoints, not the live rider.
            publish_nav(w);
            if (w.get_screen() != 3) publish_marker(w);
#if defined(CYCLOMP_MAP_GL)
            // Detail map, idle: show the projected start/stop bullets and hide
            // the moving replay dot. (During replay, replay_tick owns the dot.)
            if (w.get_screen() == 3) {
                double snx, sny, enx, eny, asp;
                if (mapgl::endpoints_offset(snx, sny, enx, eny, asp)) {
                    w.set_ep_start_nx((float)snx);
                    w.set_ep_start_ny((float)sny);
                    w.set_ep_end_nx((float)enx);
                    w.set_ep_end_ny((float)eny);
                    w.set_frame_aspect((float)asp);
                    w.set_track_endpoints(true);
                }
                if (!replay_active->load()) w.set_marker_valid(false);
            }
#endif
            // With real GPS the startup camera push is skipped (nowhere to
            // point yet): place the camera on the very first fix, even while
            // idle — no LOCATE press needed after launch.
            if (!camera->placed()) {
                double la, lo;
                if (current_position(la, lo))
                    if (auto up = camera->on_position(la, lo)) cam_sink(*up);
            }
#if defined(CYCLOMP_MAP_GL) && defined(__ANDROID__)
            // Keep the live map's 2.5D pitch in sync with the MAP-screen
            // toggle. Only touches the camera on the MAP screen and only when
            // the value actually changed; leaving the screen arms a re-apply
            // (the detail map resets pitch to 0 behind our back).
            if (w.get_screen() == 1) {
                double want = *map_3d ? kChasePitch : 0.0;
                if (want != *map_pitch_applied) {
                    mapgl::set_pitch(want);
                    *map_pitch_applied = want;
                }
            } else {
                *map_pitch_applied = -1.0;
            }
            // Heading-up: compass while slow or stopped, GPS course once
            // moving briskly. Runs even outside a ride; paused during
            // replays (the flyover owns the bearing there). Confined to the
            // MAP screen: the map camera is a single shared mbgl instance, so
            // without this gate the main-map compass would keep rotating the
            // log-detail map too (they must stay independent).
            if (*heading_up && !replay_active->load() && w.get_screen() == 1) {
                *compass_elapsed += 0.5; // one tick
                std::optional<double> hdg;
                if (engine->state() == core::RideState::Running &&
                    engine->live().speed_kmh > settings->heading_speed_kmh &&
                    *last_course)
                    hdg = **last_course;
                else {
                    double az;
                    if (compass::azimuth_deg(az)) hdg = az;
                    else if (*last_course) hdg = **last_course;
                }
                if (hdg) {
                    double d = std::fmod(std::fabs(*hdg - *last_sent_bearing),
                                         360.0);
                    if (d > 180.0) d = 360.0 - d;
                    // Rotate only when the change is big enough AND the
                    // user-tuned interval has passed (first fix bypasses).
                    bool first = *last_sent_bearing < -500.0;
                    if (first ||
                        (d > settings->bearing_min_delta_deg &&
                         *compass_elapsed >= settings->compass_interval_s)) {
                        mapgl::set_bearing(*hdg);
                        *last_sent_bearing = *hdg;
                        *compass_elapsed = 0.0;
                    }
                }
            }
#endif
            if (engine->state() != core::RideState::Running) return;
            auto s = source->sample(0.5);
            engine->tick(0.5, s);
            publish_live(w, engine->live(), sensors);
            if (s.valid) {
                recorder->add(s.lat, s.lon, s.speed_kmh,
                              engine->live().elapsed_s,
                              s.altitude_m.value_or(0.0));
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
                // Live elevation HUD on the MAP screen (~1 Hz).
                if (recorder->points().size() % 2 == 0) {
                    w.set_live_elev_path(
                        slint::SharedString(core::elevation_profile_path(
                            recorder->points(), 160, 60)));
                    w.set_live_elev_label(slint::SharedString(
                        "ELEV +" +
                        core::fmt::fmt0(
                            core::elevation_gain_m(recorder->points())) +
                        " m"));
                }
                if (s.heading_deg) *last_course = *s.heading_deg;
                if (auto up = camera->on_position(s.lat, s.lon)) cam_sink(*up);
            }
            // Snapshot the running ride every ~10 s so an unexpected kill
            // (no HOLD, no onPause) loses at most a few seconds of track.
            static int autosave_tick = 0;
            if (++autosave_tick >= 20) {
                autosave_tick = 0;
                save_active();
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
