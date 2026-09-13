// cyclomp core — mock telemetry: plausible fake ride data, deterministic RNG.
#pragma once

#include "core/telemetry.h"

#include <cstdint>

namespace core {

class MockTelemetrySource final : public TelemetrySource {
public:
    Sample sample(double dt_s) override;

    // New ride: speed and the nav countdown reset; the POSITION survives so
    // consecutive rides continue where the last one ended.
    void reset_ride();

    int nav_m() const { return nav_m_; }
    double lat() const { return lat_; }
    double lon() const { return lon_; }

private:
    struct Rng {
        uint32_t s = 0x9e3779b9u;
        float next() { // [0,1)
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            return (s >> 8) * (1.0f / 16777216.0f);
        }
        float centered() { return next() - 0.5f; } // [-0.5,0.5)
    };

    Rng rng_;
    double speed_ = 0.0;               // km/h
    double lat_ = -6.9147;             // Bandung
    double lon_ = 107.6098;
    double heading_deg_ = 90.0;
    double alt_m_ = 715.0;             // Bandung-ish; wanders like hills
    int nav_m_ = 400;                  // metres to next (mock) turn
};

} // namespace core
