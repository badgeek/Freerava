#include <doctest/doctest.h>

#include "core/session_store.h"
#include "core/track.h"

#include <cstdio>
#include <string>

using namespace core;

TEST_CASE("recorder caps and halves") {
    TrackRecorder r;
    for (size_t i = 0; i < TrackRecorder::kMaxPoints + 10; ++i)
        r.add(-6.9 + i * 1e-5, 107.6, 20.0, i * 0.5);
    CHECK(r.points().size() <= TrackRecorder::kMaxPoints);
    CHECK(r.points().size() > TrackRecorder::kMaxPoints / 2);
    // order preserved
    CHECK(r.points().front().t_s < r.points().back().t_s);
}

TEST_CASE("sparkline path shape") {
    std::vector<TrackPoint> pts = {
        {-6.9, 107.6, 10.f, 0.f},
        {-6.9, 107.6, 40.f, 10.f},
        {-6.9, 107.6, 20.f, 20.f},
    };
    auto p = speed_sparkline_path(pts, 300, 70);
    CHECK(p.substr(0, 2) == "M ");
    CHECK(p.find(" L ") != std::string::npos);
    CHECK(speed_sparkline_path({}, 300, 70).empty());
    CHECK(speed_sparkline_path({pts[0]}, 300, 70).empty());
}

TEST_CASE("track shape path stays inside the viewbox") {
    std::vector<TrackPoint> pts;
    for (int i = 0; i < 50; ++i)
        pts.push_back({-6.91 + i * 0.0004, 107.60 + (i % 7) * 0.0003, 20.f,
                       (float)i});
    auto p = track_shape_path(pts, 300, 120);
    REQUIRE(!p.empty());
    // crude bounds scan over the "x y" numbers
    double x, y;
    const char *c = p.c_str();
    int n = 0;
    while (*c) {
        if ((*c == 'M' || *c == 'L') && std::sscanf(c + 1, "%lf %lf", &x, &y) == 2) {
            CHECK(x >= 0.0);
            CHECK(x <= 300.0);
            CHECK(y >= 0.0);
            CHECK(y <= 120.0);
            ++n;
        }
        ++c;
    }
    CHECK(n == 50);
}

TEST_CASE("elevation gain sums positive steps above the noise floor") {
    std::vector<TrackPoint> pts = {
        {0, 0, 0, 0, 700.f},  {0, 0, 0, 1, 705.f},  // +5
        {0, 0, 0, 2, 705.2f},                       // +0.2 (noise, ignored)
        {0, 0, 0, 3, 702.f},                        // downhill, ignored
        {0, 0, 0, 4, 710.f},                        // +8
    };
    CHECK(elevation_gain_m(pts) == doctest::Approx(13.0).epsilon(0.01));
    CHECK(elevation_gain_m({}) == doctest::Approx(0));
}

TEST_CASE("elevation profile pads a flat ride") {
    std::vector<TrackPoint> flat = {
        {0, 0, 0, 0, 700.f}, {0, 0, 0, 10, 700.4f}, {0, 0, 0, 20, 700.2f}};
    auto p = elevation_profile_path(flat, 300, 70);
    REQUIRE(!p.empty());
    // With the 8m pad, a 0.4m wiggle must stay well inside the box: every y
    // is near the middle (not clamped to 1 / 69).
    double x, y;
    const char *c = p.c_str();
    while (*c) {
        if ((*c == 'M' || *c == 'L') &&
            std::sscanf(c + 1, "%lf %lf", &x, &y) == 2) {
            CHECK(y > 20.0);
            CHECK(y < 50.0);
        }
        ++c;
    }
    CHECK(elevation_profile_path({flat[0]}, 300, 70).empty());
}

TEST_CASE("session store round-trip with tracks") {
    SessionLog log;
    SessionSummary a;
    a.ended_at = 100;
    a.dist_km = 1.25;
    a.moving_s = 300;
    a.avg_kmh = 15;
    a.max_kmh = 30;
    a.has_hr = true;
    a.avg_hr = 140;
    a.track = {{-6.9, 107.6, 12.f, 0.f, 701.f},
               {-6.901, 107.601, 18.f, 5.f, 703.5f}};
    SessionSummary b;
    b.ended_at = 200; // newer, no channels, no track
    log.add(a);
    log.add(b);

    std::string path = std::string(std::tmpnam(nullptr)) + "-cyc";
    REQUIRE(save_sessions(path, log));
    SessionLog back;
    REQUIRE(load_sessions(path, back));
    std::remove(path.c_str());

    REQUIRE(back.newest_first().size() == 2);
    CHECK(back.newest_first()[0].ended_at == 200); // order survives
    const auto &ra = back.newest_first()[1];
    CHECK(ra.ended_at == 100);
    CHECK(ra.dist_km == doctest::Approx(1.25));
    CHECK(ra.has_hr);
    CHECK(ra.avg_hr == 140);
    REQUIRE(ra.track.size() == 2);
    CHECK(ra.track[1].lat == doctest::Approx(-6.901));
    CHECK(ra.track[1].speed_kmh == doctest::Approx(18.f));
    CHECK(ra.track[1].alt_m == doctest::Approx(703.5f));
    CHECK_FALSE(back.newest_first()[0].has_hr);
    CHECK(back.newest_first()[0].track.empty());
}
