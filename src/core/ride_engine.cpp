#include "core/ride_engine.h"

namespace core {

int hr_zone(int bpm) {
    return bpm < 105 ? 1 : bpm < 125 ? 2 : bpm < 145 ? 3 : bpm < 165 ? 4 : 5;
}

RideState RideEngine::toggle() {
    switch (state_) {
    case RideState::Idle:
        reset_stats();
        state_ = RideState::Running;
        break;
    case RideState::Running:
        state_ = RideState::Paused;
        break;
    case RideState::Paused:
        state_ = RideState::Running;
        break;
    }
    return state_;
}

std::optional<SessionSummary> RideEngine::stop(std::time_t now) {
    bool had_ride = state_ != RideState::Idle && samples_ > 0;
    state_ = RideState::Idle;
    if (!had_ride) return std::nullopt;

    SessionSummary s;
    s.ended_at = now;
    s.dist_km = live_.dist_km;
    s.moving_s = live_.elapsed_s;
    s.avg_kmh = samples_ ? sum_speed_ / samples_ : 0.0;
    s.max_kmh = max_kmh_;
    s.has_cadence = cad_samples_ > 0;
    s.avg_cadence = s.has_cadence ? (int)(sum_cad_ / cad_samples_) : 0;
    s.has_hr = hr_samples_ > 0;
    s.avg_hr = s.has_hr ? (int)(sum_hr_ / hr_samples_) : 0;
    return s;
}

void RideEngine::tick(double dt_s, const Sample &s) {
    if (state_ != RideState::Running) return;

    live_.elapsed_s += dt_s; // float accumulation — never lround per tick

    if (!s.valid) return; // no fix: the clock moves, the stats don't

    live_.speed_kmh = s.speed_kmh;
    live_.dist_km += s.speed_kmh * (dt_s / 3600.0);
    live_.lat = s.lat;
    live_.lon = s.lon;
    sum_speed_ += s.speed_kmh;
    if (s.speed_kmh > max_kmh_) max_kmh_ = s.speed_kmh;
    samples_ += 1;
    live_.avg_kmh = sum_speed_ / samples_;

    if (s.cadence) {
        live_.cadence = *s.cadence;
        sum_cad_ += *s.cadence;
        cad_samples_ += 1;
    }
    if (s.heart_rate) {
        live_.heart_rate = *s.heart_rate;
        sum_hr_ += *s.heart_rate;
        hr_samples_ += 1;
    }
}

RideEngine::Snapshot RideEngine::snapshot() const {
    return {(int)state_,  live_,       sum_speed_,   max_kmh_,    sum_hr_,
            sum_cad_,     samples_,    hr_samples_,  cad_samples_};
}

void RideEngine::restore(const Snapshot &s) {
    state_ = (RideState)s.state;
    live_ = s.live;
    sum_speed_ = s.sum_speed;
    max_kmh_ = s.max_kmh;
    sum_hr_ = s.sum_hr;
    sum_cad_ = s.sum_cad;
    samples_ = s.samples;
    hr_samples_ = s.hr_samples;
    cad_samples_ = s.cad_samples;
}

void RideEngine::reset_stats() {
    live_ = LiveStats{};
    sum_speed_ = max_kmh_ = 0;
    sum_hr_ = sum_cad_ = 0;
    samples_ = hr_samples_ = cad_samples_ = 0;
}

} // namespace core
