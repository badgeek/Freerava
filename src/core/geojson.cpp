#include "core/geojson.h"

#include <cstdio>

namespace core {

namespace {
void appendf(std::string &out, const char *fmt, double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, v);
    out += buf;
}
void appendi(std::string &out, const char *fmt, int v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, fmt, v);
    out += buf;
}
} // namespace

std::string session_to_geojson(const SessionSummary &s) {
    std::string o;
    o.reserve(s.track.size() * 64 + 512);

    o += "{\"type\":\"FeatureCollection\",\"properties\":{";
    appendf(o, "\"ended_at\":%.0f", (double)s.ended_at);
    appendf(o, ",\"dist_km\":%.3f", s.dist_km);
    appendf(o, ",\"moving_s\":%.1f", s.moving_s);
    appendf(o, ",\"avg_kmh\":%.2f", s.avg_kmh);
    appendf(o, ",\"max_kmh\":%.2f", s.max_kmh);
    o += ",\"has_cadence\":";
    o += s.has_cadence ? "true" : "false";
    appendi(o, ",\"avg_cadence\":%d", s.avg_cadence);
    o += ",\"has_hr\":";
    o += s.has_hr ? "true" : "false";
    appendi(o, ",\"avg_hr\":%d", s.avg_hr);
    o += "},\"features\":[";

    if (!s.track.empty()) {
        // Geometry: [lon, lat, alt] per the GeoJSON axis order.
        o += "{\"type\":\"Feature\",\"geometry\":{\"type\":\"LineString\","
             "\"coordinates\":[";
        for (size_t i = 0; i < s.track.size(); ++i) {
            const TrackPoint &p = s.track[i];
            if (i) o += ',';
            o += '[';
            appendf(o, "%.7f", p.lon);
            appendf(o, ",%.7f", p.lat);
            appendf(o, ",%.2f", (double)p.alt_m);
            o += ']';
        }
        // Per-vertex channels the visualiser reads (colour by speed, scrub by
        // time), kept parallel to the coordinate array.
        o += "]},\"properties\":{\"speed_kmh\":[";
        for (size_t i = 0; i < s.track.size(); ++i) {
            if (i) o += ',';
            appendf(o, "%.3f", (double)s.track[i].speed_kmh);
        }
        o += "],\"t_s\":[";
        for (size_t i = 0; i < s.track.size(); ++i) {
            if (i) o += ',';
            appendf(o, "%.2f", (double)s.track[i].t_s);
        }
        o += "]}}";
    }

    o += "]}";
    return o;
}

} // namespace core
