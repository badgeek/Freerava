// cyclomp — real GPS telemetry on Android, no Java sources.
// A latest-fix mailbox over LocationManager.getLastKnownLocation, reached
// through the JavaVM/Activity the NativeActivity interposer captured. The
// ride engine pulls sample() every tick; until a fresh, accurate fix exists
// the samples are simply invalid (clock-only ticks).
#pragma once
#ifdef __ANDROID__

#include "core/telemetry.h"

#include <jni.h>
#include <string>

namespace gps {

class AndroidTelemetrySource final : public core::TelemetrySource {
public:
    ~AndroidTelemetrySource() override;

    // core::TelemetrySource — polls (rate limited) and reports the mailbox.
    core::Sample sample(double dt_s) override;

    // Refresh the mailbox from LocationManager, at most once per second.
    // Cheap and safe to call from the UI thread in any ride state.
    void poll();

    // Ask the user for ACCESS_FINE_LOCATION. Called when the ride starts;
    // repeated calls after the first are no-ops (NativeActivity never
    // delivers onRequestPermissionsResult, so poll() re-checks instead).
    void request_permission();

    // What the nav strip shows in place of the mock turn countdown.
    struct Status {
        std::string distance;    // "GPS" / "NO GPS"
        std::string instruction; // "waiting for fix…" / "±12 m accuracy"
    };
    Status status() const;

private:
    // JNIEnv for the calling thread, attaching it if needed.
    JNIEnv *env();
    // Resolves (and caches) the system LocationManager. Null when the
    // permission is missing or the service is unavailable.
    jobject location_manager(JNIEnv *e);

    // Coarse state of the source, logged only when it changes.
    enum class State { Unknown, NoPermission, NoProvider, Waiting, Fixed };
    void set_state(State s, const char *detail);

    jobject manager_ = nullptr; // global ref
    bool granted_ = false;
    bool requested_ = false;
    State state_ = State::Unknown;

    long long last_poll_ms_ = 0; // steady clock; 0 => never polled

    // Latest accepted fix.
    bool have_fix_ = false;
    double lat_ = 0, lon_ = 0;
    double accuracy_m_ = 0;
    double speed_kmh_ = 0;
    long long fix_time_ms_ = 0;
    long long fix_age_ms_ = 0;

    // Previous accepted fix, for the haversine fallback speed.
    bool have_prev_ = false;
    double prev_lat_ = 0, prev_lon_ = 0;
    long long prev_time_ms_ = 0;
};

} // namespace gps

#endif // __ANDROID__
