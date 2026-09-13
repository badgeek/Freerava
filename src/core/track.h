// cyclomp core — ride track recording + tiny SVG-path generators for the
// history infographics (Slint Path consumes SVG command strings).
#pragma once

#include <string>
#include <vector>

namespace core {

struct TrackPoint {
    double lat = 0;
    double lon = 0;
    float speed_kmh = 0;
    float t_s = 0;   // moving time at this point
    float alt_m = 0; // 0 when the source has no altitude
};

// Collects points during a ride; halves itself when the cap is reached so
// memory stays bounded on long rides (coarse LOD, order preserved).
class TrackRecorder {
public:
    static constexpr size_t kMaxPoints = 4096;

    void add(double lat, double lon, double speed_kmh, double t_s,
             double alt_m = 0);
    void reset() { pts_.clear(); }
    const std::vector<TrackPoint> &points() const { return pts_; }

private:
    std::vector<TrackPoint> pts_;
};

// "M x y L x y ..." speed-over-time inside a w x h viewbox (y grows down,
// so higher speed = smaller y). Empty string when fewer than 2 points.
std::string speed_sparkline_path(const std::vector<TrackPoint> &pts,
                                 double w, double h);

// The ride's shape: lat/lon normalised into w x h, aspect preserved and
// centred, lon scaled by cos(mid-lat). Empty when fewer than 2 points.
std::string track_shape_path(const std::vector<TrackPoint> &pts,
                             double w, double h);

// Altitude-over-time profile; the vertical scale is padded to at least 8 m
// so a flat ride doesn't zoom into GPS noise. Empty when < 2 points.
std::string elevation_profile_path(const std::vector<TrackPoint> &pts,
                                   double w, double h);

// Total climb: the sum of positive altitude steps above a small noise
// threshold (0.3 m per sample).
double elevation_gain_m(const std::vector<TrackPoint> &pts);

} // namespace core
