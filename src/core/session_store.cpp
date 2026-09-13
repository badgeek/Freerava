#include "core/session_store.h"

#include <cstdio>

namespace core {

bool save_sessions(const std::string &path, const SessionLog &log) {
    std::FILE *f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    const auto &rows = log.newest_first();
    // Oldest first on disk, so load()'s add() rebuilds newest-first.
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const SessionSummary &s = *it;
        std::fprintf(f, "S %lld %.6f %.3f %.6f %.6f %d %d %d %d %zu\n",
                     (long long)s.ended_at, s.dist_km, s.moving_s, s.avg_kmh,
                     s.max_kmh, s.avg_cadence, s.has_cadence ? 1 : 0,
                     s.avg_hr, s.has_hr ? 1 : 0, s.track.size());
        for (const TrackPoint &p : s.track)
            std::fprintf(f, "P %.7f %.7f %.3f %.3f %.2f\n", p.lat, p.lon,
                         (double)p.speed_kmh, (double)p.t_s, (double)p.alt_m);
    }
    std::fclose(f);
    return true;
}

bool load_sessions(const std::string &path, SessionLog &out) {
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    long long ended;
    int cad, hasCad, hr, hasHr;
    size_t n;
    SessionSummary s;
    while (std::fscanf(f, "S %lld %lf %lf %lf %lf %d %d %d %d %zu\n", &ended,
                       &s.dist_km, &s.moving_s, &s.avg_kmh, &s.max_kmh, &cad,
                       &hasCad, &hr, &hasHr, &n) == 10) {
        s.ended_at = (std::time_t)ended;
        s.avg_cadence = cad;
        s.has_cadence = hasCad != 0;
        s.avg_hr = hr;
        s.has_hr = hasHr != 0;
        s.track.clear();
        s.track.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            double lat, lon, sp, t, alt = 0;
            int got = std::fscanf(f, "P %lf %lf %lf %lf %lf\n", &lat, &lon,
                                  &sp, &t, &alt);
            if (got == 4) alt = 0; // pre-altitude files
            else if (got != 5) {
                std::fclose(f);
                return false; // truncated file
            }
            s.track.push_back({lat, lon, (float)sp, (float)t, (float)alt});
        }
        out.add(s);
    }
    std::fclose(f);
    return true;
}

} // namespace core
