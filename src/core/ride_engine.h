// cyclomp core — ride state machine + statistics accumulation.
// Owns the Idle/Running/Paused lifecycle authoritatively; the UI merely
// mirrors state() into its `ride-state` property.
#pragma once

#include "core/telemetry.h"
#include "core/track.h"

#include <ctime>
#include <optional>
#include <vector>

namespace core {

// Values match the Slint `ride-state` property.
enum class RideState : int { Idle = 0, Running = 1, Paused = 2 };

struct LiveStats {
    double speed_kmh = 0;
    double dist_km = 0;
    double elapsed_s = 0;   // moving seconds, float-accumulated (pauses excluded)
    double avg_kmh = 0;
    double lat = 0, lon = 0; // last valid fix
    int cadence = 0;
    int heart_rate = 0;
};

// Typed summary — numbers and a timestamp; strings are a UI concern.
struct SessionSummary {
    std::time_t ended_at = 0;
    double dist_km = 0;
    double moving_s = 0;
    double avg_kmh = 0;
    double max_kmh = 0;
    int avg_cadence = 0;
    bool has_cadence = false;
    int avg_hr = 0;
    bool has_hr = false;
    std::vector<TrackPoint> track; // recorded ride path (may be empty)
};

// 1..5 from bpm; thresholds 105/125/145/165.
int hr_zone(int bpm);

class RideEngine {
public:
    RideState state() const { return state_; }

    // Idle->Running (stats reset), Running->Paused, Paused->Running.
    RideState toggle();

    // Any state -> Idle. Summary only when a ride with samples ended.
    std::optional<SessionSummary> stop(std::time_t now);

    // No-op unless Running. An invalid sample advances the clock only.
    void tick(double dt_s, const Sample &s);

    const LiveStats &live() const { return live_; }

private:
    void reset_stats();

    RideState state_ = RideState::Idle;
    LiveStats live_{};
    double sum_speed_ = 0, max_kmh_ = 0;
    long sum_hr_ = 0, sum_cad_ = 0;
    int samples_ = 0, hr_samples_ = 0, cad_samples_ = 0;
};

// Finished rides, newest first.
class SessionLog {
public:
    void add(const SessionSummary &s) { rows_.insert(rows_.begin(), s); }
    const std::vector<SessionSummary> &newest_first() const { return rows_; }

private:
    std::vector<SessionSummary> rows_;
};

} // namespace core
