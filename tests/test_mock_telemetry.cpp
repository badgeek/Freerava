#include <doctest/doctest.h>

#include "core/mock_telemetry.h"

using namespace core;

TEST_CASE("mock produces plausible, present-channel samples") {
    MockTelemetrySource src;
    for (int i = 0; i < 1000; ++i) {
        Sample s = src.sample(0.5);
        CHECK(s.valid);
        CHECK(s.speed_kmh >= 8.0);
        CHECK(s.speed_kmh <= 42.0);
        CHECK(s.heart_rate.has_value());
        CHECK(s.cadence.has_value());
    }
}

TEST_CASE("nav countdown decreases and wraps") {
    MockTelemetrySource src;
    int start = src.nav_m();
    CHECK(start == 400);
    int prev = start;
    bool wrapped = false;
    for (int i = 0; i < 5000 && !wrapped; ++i) {
        src.sample(0.5);
        int now = src.nav_m();
        if (now > prev) {
            wrapped = true;
            CHECK(now >= 400);
            CHECK(now < 1200);
        }
        prev = now;
    }
    CHECK(wrapped);
}

TEST_CASE("reset_ride keeps the position") {
    MockTelemetrySource src;
    for (int i = 0; i < 100; ++i) src.sample(0.5);
    double lat = src.lat(), lon = src.lon();
    CHECK(lat != doctest::Approx(-6.9147)); // wandered away from the start
    src.reset_ride();
    CHECK(src.lat() == doctest::Approx(lat));
    CHECK(src.lon() == doctest::Approx(lon));
    CHECK(src.nav_m() == 400);
}
