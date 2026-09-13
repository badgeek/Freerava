// cyclomp core — user-tunable parameters, persisted as key=value lines.
// The SETTINGS screen (gear button) edits these; main.cpp applies them.
#pragma once

#include <string>

namespace core {

struct Settings {
    // Camera zoom used when the follow camera first locks on.
    double follow_zoom = 15.0;
    // Above this speed heading-up trusts the GPS course over the compass.
    double heading_speed_kmh = 7.0;
    // Minimum bearing change (deg) before a rotation is sent to the map.
    double bearing_min_delta_deg = 3.0;
    // Use the fake ride on Android too (applied at next launch).
    bool mock_ride = false;
};

// Bring every field back into its legal range.
Settings clamp_settings(Settings s);

bool save_settings(const std::string &path, const Settings &s);
// Missing file leaves defaults; unknown keys are ignored (forward compat).
bool load_settings(const std::string &path, Settings &out);

} // namespace core
