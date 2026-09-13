#include "core/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace core {

Settings clamp_settings(Settings s) {
    s.follow_zoom = std::clamp(s.follow_zoom, 3.0, 19.0);
    s.heading_speed_kmh = std::clamp(s.heading_speed_kmh, 0.0, 30.0);
    s.bearing_min_delta_deg = std::clamp(s.bearing_min_delta_deg, 1.0, 30.0);
    s.tunnel_base = std::clamp(s.tunnel_base, 0.1, 2.0);
    // The cap can never sit below the base rate.
    s.tunnel_cap = std::clamp(s.tunnel_cap, s.tunnel_base, 3.0);
    s.tunnel_fps = std::clamp(s.tunnel_fps, 5.0, 60.0);
    s.compass_interval_s = std::clamp(s.compass_interval_s, 0.5, 5.0);
    return s;
}

bool save_settings(const std::string &path, const Settings &s) {
    std::FILE *f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "follow_zoom=%.1f\n", s.follow_zoom);
    std::fprintf(f, "heading_speed_kmh=%.1f\n", s.heading_speed_kmh);
    std::fprintf(f, "bearing_min_delta_deg=%.1f\n", s.bearing_min_delta_deg);
    std::fprintf(f, "tunnel_base=%.2f\n", s.tunnel_base);
    std::fprintf(f, "tunnel_cap=%.2f\n", s.tunnel_cap);
    std::fprintf(f, "tunnel_fps=%.0f\n", s.tunnel_fps);
    std::fprintf(f, "tunnel_enabled=%d\n", s.tunnel_enabled ? 1 : 0);
    std::fprintf(f, "compass_interval_s=%.1f\n", s.compass_interval_s);
    std::fprintf(f, "mock_ride=%d\n", s.mock_ride ? 1 : 0);
    std::fclose(f);
    return true;
}

bool load_settings(const std::string &path, Settings &out) {
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char line[128];
    while (std::fgets(line, sizeof line, f)) {
        char *eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        double v = std::atof(eq + 1);
        if (!std::strcmp(line, "follow_zoom")) out.follow_zoom = v;
        else if (!std::strcmp(line, "heading_speed_kmh")) out.heading_speed_kmh = v;
        else if (!std::strcmp(line, "bearing_min_delta_deg")) out.bearing_min_delta_deg = v;
        else if (!std::strcmp(line, "tunnel_base")) out.tunnel_base = v;
        else if (!std::strcmp(line, "tunnel_cap")) out.tunnel_cap = v;
        else if (!std::strcmp(line, "tunnel_fps")) out.tunnel_fps = v;
        else if (!std::strcmp(line, "tunnel_enabled")) out.tunnel_enabled = v != 0;
        else if (!std::strcmp(line, "compass_interval_s")) out.compass_interval_s = v;
        else if (!std::strcmp(line, "mock_ride")) out.mock_ride = v != 0;
        // unknown keys: ignored
    }
    std::fclose(f);
    out = clamp_settings(out);
    return true;
}

} // namespace core
