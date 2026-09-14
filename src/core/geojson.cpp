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
    return session_to_geojson(s, {});
}

std::string session_to_geojson(const SessionSummary &s,
                               const std::vector<Photo> &photos) {
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

    bool wrote = false; // guards the separators in the feature array
    if (!s.track.empty()) {
        wrote = true;
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

    // One Point per photo. The thumbnail travels inline as a data: URI so the
    // viewer keeps its single-file drop; the full-resolution JPEG stays on the
    // phone and is only referenced by name.
    for (const Photo &ph : photos) {
        double lon = ph.lon, lat = ph.lat;
        if (!ph.has_fix) {
            // No fix at the shutter: place it on the track by elapsed time.
            if (s.track.size() < 2) continue;
            size_t j = 1;
            while (j < s.track.size() && (double)s.track[j].t_s < ph.t_s) ++j;
            const TrackPoint &a = s.track[j - 1];
            const TrackPoint &b = s.track[j < s.track.size() ? j : s.track.size() - 1];
            const double span = (double)b.t_s - (double)a.t_s;
            const double f = span > 1e-6 ? (ph.t_s - (double)a.t_s) / span : 0.0;
            const double ff = f < 0 ? 0 : (f > 1 ? 1 : f);
            lon = a.lon + ff * (b.lon - a.lon);
            lat = a.lat + ff * (b.lat - a.lat);
        }

        const std::string thumb = base64(exif_thumbnail(ph.path));
        if (thumb.empty()) continue; // no picture to show: skip rather than emit a blank pin

        if (wrote) o += ',';   // a photo can be the FIRST feature when the
        wrote = true;          // ride recorded no track at all
        o += "{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\","
             "\"coordinates\":[";
        appendf(o, "%.7f", lon);
        appendf(o, ",%.7f", lat);
        o += "]},\"properties\":{\"kind\":\"photo\"";
        appendf(o, ",\"taken_at\":%.0f", (double)ph.taken_at);
        appendf(o, ",\"t_s\":%.2f", ph.t_s);
        o += ",\"has_fix\":";
        o += ph.has_fix ? "true" : "false";
        o += ",\"thumb\":\"data:image/jpeg;base64,";
        o += thumb;
        o += "\"}}";
    }

    o += "]}";
    return o;
}

} // namespace core
