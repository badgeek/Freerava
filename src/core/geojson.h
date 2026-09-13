// cyclomp core — serialise a finished ride to GeoJSON for export. Pure C++
// (string building only); feeds the Three.js run visualiser in viz/.
#pragma once

#include "core/ride_engine.h" // SessionSummary

#include <string>

namespace core {

// A GeoJSON FeatureCollection: one LineString of [lon,lat,alt] vertices, with
// per-vertex speed_kmh / t_s arrays and the ride summary in top-level
// properties. Empty-track rides still produce a valid (feature-less) document.
std::string session_to_geojson(const SessionSummary &s);

} // namespace core
