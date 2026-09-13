#include "core/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace core {

Settings clamp_settings(Settings s) {
    s.follow_zoom = std::clamp(s.follow_zoom, 3.0, 19.0);
    s.heading_speed_kmh = std::clamp(s.heading_speed_kmh, 0.0, 30.0);
    s.bearing_min_delta_deg = std::clamp(s.bearing_min_delta_deg, 1.0, 30.0);
    return s;
}

bool save_settings(const std::string &path, const Settings &s) {
    std::FILE *f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    std::fprintf(f, "follow_zoom=%.1f\n", s.follow_zoom);
    std::fprintf(f, "heading_speed_kmh=%.1f\n", s.heading_speed_kmh);
    std::fprintf(f, "bearing_min_delta_deg=%.1f\n", s.bearing_min_delta_deg);
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
        else if (!std::strcmp(line, "mock_ride")) out.mock_ride = v != 0;
        // unknown keys: ignored
    }
    std::fclose(f);
    out = clamp_settings(out);
    return true;
}

} // namespace core
