#include "core/mock_telemetry.h"

#include <cmath>

namespace core {

Sample MockTelemetrySource::sample(double dt_s) {
    // Speed random-walk, clamped to a sane cycling range.
    speed_ += rng_.centered() * 6.f;
    if (speed_ < 8.0) speed_ = 8.0;
    if (speed_ > 42.0) speed_ = 42.0;

    // Wander the position along a slowly-turning heading.
    heading_deg_ += rng_.centered() * 24.f;
    double d_km = speed_ * (dt_s / 3600.0);
    double rad = heading_deg_ * M_PI / 180.0;
    lat_ += (d_km * std::cos(rad)) / 111.32;
    lon_ += (d_km * std::sin(rad)) / (111.32 * std::cos(lat_ * M_PI / 180.0));

    // Mock turn-by-turn countdown.
    nav_m_ -= (int)std::lround(d_km * 1000.0);
    if (nav_m_ <= 0) nav_m_ = 400 + (int)(rng_.next() * 800.f);

    Sample s;
    s.valid = true;
    s.speed_kmh = speed_;
    s.lat = lat_;
    s.lon = lon_;
    // Heart rate tracks effort with jitter; cadence derives from speed.
    s.heart_rate = (int)std::lround(90 + speed_ * 1.9 + rng_.centered() * 6.f);
    s.cadence = speed_ > 0 ? (int)std::lround(speed_ * 2.4 + 12) : 0;
    return s;
}

void MockTelemetrySource::reset_ride() {
    speed_ = 0.0;
    nav_m_ = 400;
}

} // namespace core
