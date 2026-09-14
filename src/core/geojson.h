// cyclomp core — serialise a finished ride to GeoJSON for export. Pure C++
// (string building only); feeds the Three.js run visualiser in viz/.
#pragma once

#include "core/photo.h"
#include "core/ride_engine.h" // SessionSummary

#include <vector>

#include <string>

namespace core {

// A GeoJSON FeatureCollection: one LineString of [lon,lat,alt] vertices, with
// per-vertex speed_kmh / t_s arrays and the ride summary in top-level
// properties. Empty-track rides still produce a valid (feature-less) document.
std::string session_to_geojson(const SessionSummary &s);

// Same document, plus one Point feature per photo carrying the JPEG's own EXIF
// thumbnail as a data: URI. Photos with no GPS fix are placed by interpolating
// the track at their t_s instead, and dropped entirely when even that is
// impossible — a marker at 0,0 is worse than no marker. Photos whose thumbnail
// cannot be read are skipped rather than emitted picture-less.
std::string session_to_geojson(const SessionSummary &s,
                               const std::vector<Photo> &photos);

} // namespace core
