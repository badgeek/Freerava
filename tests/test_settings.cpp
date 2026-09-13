#include <doctest/doctest.h>

#include "core/settings.h"

#include <cstdio>
#include <string>

namespace {
std::string tmp_path() {
    static int n = 0;
    return std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
           "/cyclomp_settings_test_" + std::to_string(n++) + ".txt";
}
} // namespace

TEST_CASE("settings round-trip") {
    core::Settings s;
    s.follow_zoom = 12.0;
    s.heading_speed_kmh = 10.0;
    s.bearing_min_delta_deg = 5.0;
    s.tunnel_base = 0.5;
    s.tunnel_cap = 1.5;
    s.tunnel_fps = 15.0;
    s.compass_interval_s = 2.0;
    s.mock_ride = true;
    auto p = tmp_path();
    REQUIRE(core::save_settings(p, s));
    core::Settings r;
    REQUIRE(core::load_settings(p, r));
    CHECK(r.follow_zoom == doctest::Approx(12.0));
    CHECK(r.heading_speed_kmh == doctest::Approx(10.0));
    CHECK(r.bearing_min_delta_deg == doctest::Approx(5.0));
    CHECK(r.tunnel_base == doctest::Approx(0.5));
    CHECK(r.tunnel_cap == doctest::Approx(1.5));
    CHECK(r.tunnel_fps == doctest::Approx(15.0));
    CHECK(r.compass_interval_s == doctest::Approx(2.0));
    CHECK(r.mock_ride);
    std::remove(p.c_str());
}

TEST_CASE("missing file keeps defaults") {
    core::Settings r;
    CHECK_FALSE(core::load_settings("/nonexistent/cyclomp.settings", r));
    CHECK(r.follow_zoom == doctest::Approx(15.0));
    CHECK(r.heading_speed_kmh == doctest::Approx(7.0));
    CHECK(r.bearing_min_delta_deg == doctest::Approx(3.0));
    CHECK_FALSE(r.mock_ride);
}

TEST_CASE("out-of-range values are clamped on load") {
    auto p = tmp_path();
    std::FILE *f = std::fopen(p.c_str(), "w");
    REQUIRE(f);
    std::fprintf(f, "follow_zoom=99\nheading_speed_kmh=-5\n"
                    "bearing_min_delta_deg=0\nfuture_key=1\n");
    std::fclose(f);
    core::Settings r;
    REQUIRE(core::load_settings(p, r));
    CHECK(r.follow_zoom == doctest::Approx(19.0));
    CHECK(r.heading_speed_kmh == doctest::Approx(0.0));
    CHECK(r.bearing_min_delta_deg == doctest::Approx(1.0));
    std::remove(p.c_str());
}

TEST_CASE("clamp_settings brings every field into range") {
    core::Settings s;
    s.follow_zoom = 1.0;
    s.heading_speed_kmh = 100.0;
    s.bearing_min_delta_deg = 50.0;
    s.tunnel_base = 9.0;
    s.tunnel_cap = 0.0;
    s.tunnel_fps = 500.0;
    s.compass_interval_s = 0.0;
    s = core::clamp_settings(s);
    CHECK(s.follow_zoom == doctest::Approx(3.0));
    CHECK(s.heading_speed_kmh == doctest::Approx(30.0));
    CHECK(s.bearing_min_delta_deg == doctest::Approx(30.0));
    CHECK(s.tunnel_base == doctest::Approx(2.0));
    // The cap is dragged up to the (already clamped) base rate.
    CHECK(s.tunnel_cap == doctest::Approx(2.0));
    CHECK(s.tunnel_fps == doctest::Approx(60.0));
    CHECK(s.compass_interval_s == doctest::Approx(0.5));
}
