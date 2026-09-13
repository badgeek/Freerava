#include "core/track.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace core {

void TrackRecorder::add(double lat, double lon, double speed_kmh, double t_s) {
    if (pts_.size() >= kMaxPoints) {
        // Halve: keep every second point (coarse LOD, order preserved).
        std::vector<TrackPoint> half;
        half.reserve(pts_.size() / 2 + 1);
        for (size_t i = 0; i < pts_.size(); i += 2) half.push_back(pts_[i]);
        pts_.swap(half);
    }
    pts_.push_back({lat, lon, (float)speed_kmh, (float)t_s});
}

namespace {
void append_pt(std::string &out, bool first, double x, double y) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s%.1f %.1f", first ? "M " : " L ", x, y);
    out += buf;
}
} // namespace

std::string speed_sparkline_path(const std::vector<TrackPoint> &pts,
                                 double w, double h) {
    if (pts.size() < 2) return {};
    float vmax = 1.f;
    for (auto &p : pts) vmax = std::max(vmax, p.speed_kmh);
    double t0 = pts.front().t_s;
    double t1 = std::max((double)pts.back().t_s, t0 + 1.0);
    std::string out;
    out.reserve(pts.size() * 14);
    for (size_t i = 0; i < pts.size(); ++i) {
        double x = (pts[i].t_s - t0) / (t1 - t0) * w;
        double y = h - (pts[i].speed_kmh / vmax) * (h - 2.0) - 1.0;
        append_pt(out, i == 0, x, y);
    }
    return out;
}

std::string track_shape_path(const std::vector<TrackPoint> &pts,
                             double w, double h) {
    if (pts.size() < 2) return {};
    double lat0 = pts[0].lat, lat1 = pts[0].lat;
    double lon0 = pts[0].lon, lon1 = pts[0].lon;
    for (auto &p : pts) {
        lat0 = std::min(lat0, p.lat);
        lat1 = std::max(lat1, p.lat);
        lon0 = std::min(lon0, p.lon);
        lon1 = std::max(lon1, p.lon);
    }
    double k = std::cos((lat0 + lat1) / 2.0 * M_PI / 180.0);
    double spanX = std::max((lon1 - lon0) * k, 1e-9);
    double spanY = std::max(lat1 - lat0, 1e-9);
    // Fit into (w,h) with a 4px margin, preserve aspect, centre.
    double m = 4.0;
    double s = std::min((w - 2 * m) / spanX, (h - 2 * m) / spanY);
    double ox = (w - spanX * s) / 2.0;
    double oy = (h - spanY * s) / 2.0;
    std::string out;
    out.reserve(pts.size() * 14);
    for (size_t i = 0; i < pts.size(); ++i) {
        double x = ox + (pts[i].lon - lon0) * k * s;
        double y = oy + (lat1 - pts[i].lat) * s; // north = up
        append_pt(out, i == 0, x, y);
    }
    return out;
}

} // namespace core
