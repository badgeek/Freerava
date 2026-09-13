// cyclomp core — telemetry abstraction.
// Pulled once per tick by the app glue; implementations decide where the
// numbers come from (mock RNG today, real GPS later via a latest-fix
// mailbox adapter whose sample() returns valid=false until a fix exists).
#pragma once

#include <optional>

namespace core {

struct Sample {
    bool valid = false;              // false => no fix; clock-only tick
    double speed_kmh = 0.0;
    double lat = 0.0;
    double lon = 0.0;
    std::optional<int> heart_rate;   // mock synthesizes; GPS era: nullopt
    std::optional<int> cadence;      // mock derives; GPS era: nullopt
    std::optional<double> altitude_m; // GPS altitude / mock hills
};

class TelemetrySource {
public:
    virtual ~TelemetrySource() = default;
    virtual Sample sample(double dt_s) = 0;
};

} // namespace core
