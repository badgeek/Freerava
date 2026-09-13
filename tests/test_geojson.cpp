#include "core/geojson.h"

#include <doctest/doctest.h>

using namespace core;

TEST_CASE("session_to_geojson emits a valid FeatureCollection with a track") {
    SessionSummary s;
    s.ended_at = 1757740000;
    s.dist_km = 5.234;
    s.moving_s = 1200;
    s.avg_kmh = 15.7;
    s.max_kmh = 32.1;
    s.has_hr = true;
    s.avg_hr = 143;
    s.track.push_back({-6.20, 106.81, 12.5f, 0.f, 20.f});
    s.track.push_back({-6.21, 106.82, 25.0f, 60.f, 25.f});

    std::string g = session_to_geojson(s);

    CHECK(g.find("\"type\":\"FeatureCollection\"") != std::string::npos);
    CHECK(g.find("\"LineString\"") != std::string::npos);
    // GeoJSON axis order is [lon,lat,alt].
    CHECK(g.find("[106.8100000,-6.2000000,20.00]") != std::string::npos);
    CHECK(g.find("\"speed_kmh\":[12.500,25.000]") != std::string::npos);
    CHECK(g.find("\"t_s\":[0.00,60.00]") != std::string::npos);
    CHECK(g.find("\"avg_hr\":143") != std::string::npos);
    CHECK(g.find("\"has_hr\":true") != std::string::npos);
    // Balanced-ish braces/brackets as a cheap sanity check.
    CHECK(g.front() == '{');
    CHECK(g.back() == '}');
}

TEST_CASE("session_to_geojson stays valid for an empty track") {
    SessionSummary s;
    s.ended_at = 42;
    std::string g = session_to_geojson(s);
    CHECK(g.find("\"features\":[]") != std::string::npos);
    CHECK(g.find("\"LineString\"") == std::string::npos);
}
