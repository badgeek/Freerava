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
    // Dashboard tunnel animation: idle rate and the speed-coupled cap
    // (cycles per second).
    double tunnel_base = 0.3;
    double tunnel_cap = 1.0;
    // Animation tick rate (frames per second) — lower = less CPU/battery.
    double tunnel_fps = 30.0;
    // Master switch for the tunnel animation.
    bool tunnel_enabled = true;
    // Heading-up: minimum seconds between map rotations (the telemetry
    // tick runs at 0.5 s, so effective values are multiples of that).
    double compass_interval_s = 0.5;
    // Use the fake ride on Android too (applied at next launch).
    bool mock_ride = false;
    // Road line brightness on the dark map, 0 (off) .. 100 (2x the tuned
    // reference). 50 = the tuned reference look (default).
    double road_brightness = 50.0;
};

// Bring every field back into its legal range.
Settings clamp_settings(Settings s);

bool save_settings(const std::string &path, const Settings &s);
// Missing file leaves defaults; unknown keys are ignored (forward compat).
bool load_settings(const std::string &path, Settings &out);

} // namespace core
