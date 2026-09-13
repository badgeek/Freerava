// cyclomp — real GPS telemetry on Android via JNI. See the header.
#ifdef __ANDROID__
#include "gps_telemetry_android.h"

#include "android_env.h"

#include <android/log.h>

#include <chrono>
#include <cmath>
#include <cstdio>

#define GPS_LOG(...) __android_log_print(ANDROID_LOG_INFO, "cyclomp-gps", __VA_ARGS__)

namespace gps {
namespace {

constexpr double kMaxAccuracyM = 30.0;   // reject sloppier fixes
constexpr long long kMaxFixAgeMs = 5000; // and stale ones
constexpr long long kPollIntervalMs = 1000;
constexpr double kJitterFloorM = 2.0;    // below this, a standing rider

long long steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Great-circle distance in metres.
double haversine_m(double lat1, double lon1, double lat2, double lon2) {
    constexpr double kR = 6371000.0;
    constexpr double kDeg = M_PI / 180.0;
    double dlat = (lat2 - lat1) * kDeg, dlon = (lon2 - lon1) * kDeg;
    double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
               std::cos(lat1 * kDeg) * std::cos(lat2 * kDeg) *
                   std::sin(dlon / 2) * std::sin(dlon / 2);
    return 2 * kR * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

// Swallow (and report) any pending Java exception.
bool cleared(JNIEnv *e) {
    if (!e->ExceptionCheck()) return false;
    e->ExceptionClear();
    return true;
}

} // namespace

AndroidTelemetrySource::~AndroidTelemetrySource() {
    JNIEnv *e = env();
    if (e && manager_ && updates_) {
        jclass mgrCls = e->FindClass("android/location/LocationManager");
        jmethodID remove =
            mgrCls ? e->GetMethodID(mgrCls, "removeUpdates", "(Landroid/app/PendingIntent;)V")
                   : nullptr;
        if (remove) e->CallVoidMethod(manager_, remove, updates_);
        cleared(e);
    }
    if (e && updates_) e->DeleteGlobalRef(updates_);
    if (e && manager_) e->DeleteGlobalRef(manager_);
    updates_ = manager_ = nullptr;
}

JNIEnv *AndroidTelemetrySource::env() {
    JavaVM *vm = cyclomp_java_vm();
    if (!vm) return nullptr;
    JNIEnv *e = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&e), JNI_VERSION_1_6) == JNI_OK) return e;
    // The UI thread is already attached; this covers any other caller.
    return vm->AttachCurrentThread(&e, nullptr) == JNI_OK ? e : nullptr;
}

void AndroidTelemetrySource::set_state(State s, const char *detail) {
    if (s == state_) return;
    state_ = s;
    GPS_LOG("%s", detail);
}

void AndroidTelemetrySource::request_permission() {
    if (requested_ || granted_) return;
    requested_ = true;
    JNIEnv *e = env();
    jobject activity = cyclomp_activity();
    if (!e || !activity) return;

    e->PushLocalFrame(8);
    jclass activityCls = e->FindClass("android/app/Activity");
    jmethodID req = activityCls ? e->GetMethodID(activityCls, "requestPermissions",
                                                "([Ljava/lang/String;I)V")
                                : nullptr;
    jclass stringCls = e->FindClass("java/lang/String");
    if (req && stringCls) {
        jobjectArray perms = e->NewObjectArray(1, stringCls, nullptr);
        e->SetObjectArrayElement(
            perms, 0, e->NewStringUTF("android.permission.ACCESS_FINE_LOCATION"));
        e->CallVoidMethod(activity, req, perms, 1);
        // NativeActivity never forwards onRequestPermissionsResult, so the
        // answer shows up as a checkSelfPermission flip on a later poll.
        if (!cleared(e)) GPS_LOG("requested ACCESS_FINE_LOCATION");
    }
    cleared(e);
    e->PopLocalFrame(nullptr);
}

jobject AndroidTelemetrySource::location_manager(JNIEnv *e) {
    jobject activity = cyclomp_activity();
    if (!activity) return nullptr;

    // Permission can be granted (or revoked) at any time and there is no
    // callback to hear it on, so re-check on every poll.
    jclass contextCls = e->FindClass("android/content/Context");
    if (!contextCls) { cleared(e); return nullptr; }
    jmethodID check =
        e->GetMethodID(contextCls, "checkSelfPermission", "(Ljava/lang/String;)I");
    if (!check) { cleared(e); return nullptr; }
    jint res = e->CallIntMethod(
        activity, check, e->NewStringUTF("android.permission.ACCESS_FINE_LOCATION"));
    granted_ = !cleared(e) && res == 0; // PackageManager.PERMISSION_GRANTED
    if (!granted_) {
        set_state(State::NoPermission, "location permission not granted");
        return nullptr;
    }

    if (manager_) return manager_;
    jmethodID getService = e->GetMethodID(contextCls, "getSystemService",
                                          "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject mgr = getService ? e->CallObjectMethod(activity, getService,
                                                   e->NewStringUTF("location"))
                             : nullptr;
    if (cleared(e) || !mgr) {
        set_state(State::NoProvider, "no LocationManager");
        return nullptr;
    }
    manager_ = e->NewGlobalRef(mgr);
    start_updates(e, manager_);
    return manager_;
}

void AndroidTelemetrySource::start_updates(JNIEnv *e, jobject mgr) {
    if (updates_) return;
    jobject activity = cyclomp_activity();
    if (!activity) return;

    const char *failed = nullptr;
    e->PushLocalFrame(16);
    do {
        jclass intentCls = e->FindClass("android/content/Intent");
        jclass piCls = e->FindClass("android/app/PendingIntent");
        jclass contextCls = e->FindClass("android/content/Context");
        jclass mgrCls = e->FindClass("android/location/LocationManager");
        if (!intentCls || !piCls || !contextCls || !mgrCls) {
            cleared(e);
            failed = "classes";
            break;
        }

        jobject intent = e->NewObject(
            intentCls, e->GetMethodID(intentCls, "<init>", "(Ljava/lang/String;)V"),
            e->NewStringUTF("dev.bauhouse.cyclomp.LOCATION"));
        jobject pkg = e->CallObjectMethod(
            activity,
            e->GetMethodID(contextCls, "getPackageName", "()Ljava/lang/String;"));
        if (cleared(e) || !intent || !pkg) { failed = "intent"; break; }
        // Explicit target: nothing listens for it, the request itself is the point.
        e->CallObjectMethod(
            intent,
            e->GetMethodID(intentCls, "setPackage",
                           "(Ljava/lang/String;)Landroid/content/Intent;"),
            pkg);

        // LocationManager rejects an immutable PendingIntent ("pending intent
        // must be mutable") — it fills the fix in. Harmless: setPackage above
        // makes the intent explicit, so only we can ever receive it.
        constexpr jint kUpdateCurrent = 0x08000000, kMutable = 0x02000000;
        jobject pi = e->CallStaticObjectMethod(
            piCls,
            e->GetStaticMethodID(
                piCls, "getBroadcast",
                "(Landroid/content/Context;ILandroid/content/Intent;I)Landroid/app/PendingIntent;"),
            activity, 0, intent, kUpdateCurrent | kMutable);
        if (cleared(e) || !pi) { failed = "PendingIntent"; break; }

        e->CallVoidMethod(
            mgr,
            e->GetMethodID(mgrCls, "requestLocationUpdates",
                           "(Ljava/lang/String;JFLandroid/app/PendingIntent;)V"),
            e->NewStringUTF("gps"), (jlong)1000, (jfloat)0, pi);
        if (cleared(e)) { failed = "requestLocationUpdates"; break; }
        updates_ = e->NewGlobalRef(pi);
        GPS_LOG("gps updates requested (1 Hz)");
    } while (false);
    cleared(e);
    e->PopLocalFrame(nullptr);
    // Not fatal — last known location may still carry a usable fix.
    if (failed) GPS_LOG("gps updates unavailable (%s)", failed);
}

void AndroidTelemetrySource::poll() {
    long long now = steady_ms();
    if (last_poll_ms_ != 0 && now - last_poll_ms_ < kPollIntervalMs) return;
    last_poll_ms_ = now;

    JNIEnv *e = env();
    if (!e) return;

    e->PushLocalFrame(16);
    do {
        jobject mgr = location_manager(e);
        if (!mgr) break;

        jclass mgrCls = e->FindClass("android/location/LocationManager");
        jmethodID last = mgrCls ? e->GetMethodID(
                                      mgrCls, "getLastKnownLocation",
                                      "(Ljava/lang/String;)Landroid/location/Location;")
                                : nullptr;
        if (!last) { cleared(e); break; }
        jobject loc = e->CallObjectMethod(mgr, last, e->NewStringUTF("gps"));
        if (cleared(e) || !loc) {
            set_state(State::Waiting, "no fix yet (gps provider)");
            break;
        }

        jclass locCls = e->FindClass("android/location/Location");
        if (!locCls) { cleared(e); break; }
        double lat = e->CallDoubleMethod(loc, e->GetMethodID(locCls, "getLatitude", "()D"));
        double lon = e->CallDoubleMethod(loc, e->GetMethodID(locCls, "getLongitude", "()D"));
        double acc = e->CallFloatMethod(loc, e->GetMethodID(locCls, "getAccuracy", "()F"));
        jlong t_ms = e->CallLongMethod(loc, e->GetMethodID(locCls, "getTime", "()J"));
        jboolean has_speed =
            e->CallBooleanMethod(loc, e->GetMethodID(locCls, "hasSpeed", "()Z"));
        double speed_mps =
            has_speed ? e->CallFloatMethod(loc, e->GetMethodID(locCls, "getSpeed", "()F")) : 0.0;
        jboolean has_alt =
            e->CallBooleanMethod(loc, e->GetMethodID(locCls, "hasAltitude", "()Z"));
        double alt_m =
            has_alt ? e->CallDoubleMethod(loc, e->GetMethodID(locCls, "getAltitude", "()D")) : 0.0;
        jboolean has_brg =
            e->CallBooleanMethod(loc, e->GetMethodID(locCls, "hasBearing", "()Z"));
        double brg =
            has_brg ? e->CallFloatMethod(loc, e->GetMethodID(locCls, "getBearing", "()F")) : 0.0;
        if (cleared(e)) break;

        jclass sysCls = e->FindClass("java/lang/System");
        jlong now_ms = e->CallStaticLongMethod(
            sysCls, e->GetStaticMethodID(sysCls, "currentTimeMillis", "()J"));
        if (cleared(e)) break;

        fix_age_ms_ = (long long)now_ms - (long long)t_ms;
        accuracy_m_ = acc;
        bool fresh = (long long)t_ms != fix_time_ms_;

        if (acc > kMaxAccuracyM || fix_age_ms_ > kMaxFixAgeMs || fix_age_ms_ < -kMaxFixAgeMs) {
            have_fix_ = false;
            char msg[96];
            std::snprintf(msg, sizeof msg, "fix rejected (±%.0f m, %lld ms old)", acc,
                          fix_age_ms_);
            set_state(State::Waiting, msg);
            if (fresh) fix_time_ms_ = t_ms;
            break;
        }

        if (fresh) {
            // The Doppler speed is authoritative when the fix carries one.
            // Some sources (the emulator's `geo fix` among them) report
            // hasSpeed() with a hard 0 while the position moves — fall back
            // to the ground distance covered since the last accepted fix.
            if (has_speed && speed_mps > 0.0) {
                speed_kmh_ = speed_mps * 3.6;
            } else if (have_prev_ && t_ms > prev_time_ms_) {
                double dt_s = (double)(t_ms - prev_time_ms_) / 1000.0;
                double d_m = haversine_m(prev_lat_, prev_lon_, lat, lon);
                speed_kmh_ = d_m < kJitterFloorM ? 0.0 : d_m / dt_s * 3.6;
            } else {
                speed_kmh_ = 0.0;
            }
            // Course over ground: the fix's own bearing when it carries a
            // real one. Like hasSpeed(), the emulator reports hasBearing()
            // with a hard 0 — treat 0 as "unknown" and compute the course
            // from the movement since the last accepted fix instead.
            if (has_brg && brg > 0.0) {
                course_deg_ = brg;
                have_course_ = true;
            } else if (have_prev_ &&
                       haversine_m(prev_lat_, prev_lon_, lat, lon) >
                           kJitterFloorM) {
                double la1 = prev_lat_ * M_PI / 180.0;
                double la2 = lat * M_PI / 180.0;
                double dlo = (lon - prev_lon_) * M_PI / 180.0;
                double y = std::sin(dlo) * std::cos(la2);
                double x = std::cos(la1) * std::sin(la2) -
                           std::sin(la1) * std::cos(la2) * std::cos(dlo);
                double c = std::atan2(y, x) * 180.0 / M_PI;
                course_deg_ = c < 0 ? c + 360.0 : c;
                have_course_ = true;
            }
            prev_lat_ = lat;
            prev_lon_ = lon;
            prev_time_ms_ = t_ms;
            have_prev_ = true;
            fix_time_ms_ = t_ms;
        }
        lat_ = lat;
        lon_ = lon;
        have_alt_ = has_alt;
        alt_m_ = alt_m;
        have_fix_ = true;
        char msg[96];
        std::snprintf(msg, sizeof msg, "fix acquired (±%.0f m)", acc);
        set_state(State::Fixed, msg);
    } while (false);
    e->PopLocalFrame(nullptr);
}

core::Sample AndroidTelemetrySource::sample(double) {
    poll();
    core::Sample s;
    if (!have_fix_) return s; // valid=false: the engine advances the clock only
    s.valid = true;
    s.speed_kmh = speed_kmh_;
    s.lat = lat_;
    s.lon = lon_;
    if (have_alt_) s.altitude_m = alt_m_;
    if (have_course_) s.heading_deg = course_deg_;
    // No heart-rate or cadence sensors yet — the UI renders these as "--".
    return s;
}

bool AndroidTelemetrySource::last_position(double &lat, double &lon) const {
    if (!have_prev_) return false;
    lat = lat_;
    lon = lon_;
    return true;
}

AndroidTelemetrySource::Status AndroidTelemetrySource::status() const {
    char buf[32];
    switch (state_) {
    case State::NoPermission:
        return {"NO GPS", "location permission needed"};
    case State::NoProvider:
        return {"NO GPS", "no location provider"};
    case State::Fixed:
        std::snprintf(buf, sizeof buf, "±%.0f m accuracy", accuracy_m_);
        return {"GPS", buf};
    case State::Waiting:
    case State::Unknown:
    default:
        return {"GPS", "waiting for fix…"};
    }
}

} // namespace gps

#endif // __ANDROID__
