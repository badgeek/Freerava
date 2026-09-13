#include <doctest/doctest.h>

#include "core/format.h"
#include "core/ride_engine.h"

using namespace core;

namespace {
Sample fix(double speed, double lat = -6.9, double lon = 107.6,
           std::optional<int> hr = 140, std::optional<int> cad = 80) {
    Sample s;
    s.valid = true;
    s.speed_kmh = speed;
    s.lat = lat;
    s.lon = lon;
    s.heart_rate = hr;
    s.cadence = cad;
    return s;
}
} // namespace

TEST_CASE("hr_zone boundaries") {
    CHECK(hr_zone(104) == 1);
    CHECK(hr_zone(105) == 2);
    CHECK(hr_zone(125) == 3);
    CHECK(hr_zone(145) == 4);
    CHECK(hr_zone(165) == 5);
}

TEST_CASE("state transitions") {
    RideEngine e;
    CHECK(e.state() == RideState::Idle);
    CHECK(e.toggle() == RideState::Running);
    CHECK(e.toggle() == RideState::Paused);
    CHECK(e.toggle() == RideState::Running);
    e.tick(0.5, fix(20));
    CHECK(e.stop(1000).has_value());
    CHECK(e.state() == RideState::Idle);
}

TEST_CASE("stop while Idle yields nothing") {
    RideEngine e;
    CHECK_FALSE(e.stop(1000).has_value());
    CHECK(e.state() == RideState::Idle);
}

TEST_CASE("stop with zero samples yields nothing") {
    RideEngine e;
    e.toggle(); // Running, no ticks
    CHECK_FALSE(e.stop(1000).has_value());
    CHECK(e.state() == RideState::Idle);
}

TEST_CASE("stop from Paused keeps the ride's stats") {
    RideEngine e;
    e.toggle();
    e.tick(0.5, fix(30));
    e.toggle(); // Paused
    auto s = e.stop(42);
    REQUIRE(s.has_value());
    CHECK(s->ended_at == 42);
    CHECK(s->max_kmh == doctest::Approx(30));
}

TEST_CASE("tick is a no-op unless Running") {
    RideEngine e;
    e.tick(0.5, fix(20)); // Idle
    CHECK(e.live().elapsed_s == doctest::Approx(0));
    e.toggle();
    e.toggle(); // Paused
    e.tick(0.5, fix(20));
    CHECK(e.live().elapsed_s == doctest::Approx(0));
}

TEST_CASE("elapsed accumulates as float (2x-time regression)") {
    RideEngine e;
    e.toggle();
    for (int i = 0; i < 3; ++i) e.tick(0.5, fix(20));
    CHECK(e.live().elapsed_s == doctest::Approx(1.5));
    // The old bug did lround(0.5)=1 per tick: 3 ticks showed 00:03, real 00:01.
    CHECK(core::fmt::mmss((int)e.live().elapsed_s) == "00:01");

    RideEngine hour;
    hour.toggle();
    for (int i = 0; i < 7200; ++i) hour.tick(0.5, fix(20));
    CHECK(hour.live().elapsed_s == doctest::Approx(3600.0));
}

TEST_CASE("avg and max") {
    RideEngine e;
    e.toggle();
    e.tick(0.5, fix(10));
    e.tick(0.5, fix(20));
    e.tick(0.5, fix(30));
    CHECK(e.live().avg_kmh == doctest::Approx(20));
    auto s = e.stop(0);
    REQUIRE(s.has_value());
    CHECK(s->avg_kmh == doctest::Approx(20));
    CHECK(s->max_kmh == doctest::Approx(30));
}

TEST_CASE("invalid samples advance the clock only") {
    RideEngine e;
    e.toggle();
    e.tick(0.5, fix(20));
    Sample nofix;
    e.tick(0.5, nofix);
    e.tick(0.5, nofix);
    CHECK(e.live().elapsed_s == doctest::Approx(1.5));
    CHECK(e.live().dist_km == doctest::Approx(20 * 0.5 / 3600.0));
    auto s = e.stop(0);
    REQUIRE(s.has_value());
    CHECK(s->avg_kmh == doctest::Approx(20)); // one sample, not three
}

TEST_CASE("optional HR/cadence channels") {
    RideEngine e;
    e.toggle();
    e.tick(0.5, fix(20, -6.9, 107.6, std::nullopt, std::nullopt));
    auto s1 = e.stop(0);
    REQUIRE(s1.has_value());
    CHECK_FALSE(s1->has_hr);
    CHECK_FALSE(s1->has_cadence);

    e.toggle();
    e.tick(0.5, fix(20, -6.9, 107.6, 100, std::nullopt));
    e.tick(0.5, fix(20, -6.9, 107.6, 140, 90));
    auto s2 = e.stop(0);
    REQUIRE(s2.has_value());
    CHECK(s2->has_hr);
    CHECK(s2->avg_hr == 120); // averaged over PRESENT values only
    CHECK(s2->has_cadence);
    CHECK(s2->avg_cadence == 90);
}

TEST_CASE("a new ride starts with clean stats") {
    RideEngine e;
    e.toggle();
    e.tick(0.5, fix(40));
    e.stop(0);
    e.toggle();
    CHECK(e.live().dist_km == doctest::Approx(0));
    CHECK(e.live().avg_kmh == doctest::Approx(0));
    e.tick(0.5, fix(10));
    auto s = e.stop(0);
    REQUIRE(s.has_value());
    CHECK(s->max_kmh == doctest::Approx(10)); // no leak from the 40 km/h ride
}

TEST_CASE("SessionLog is newest-first") {
    SessionLog log;
    SessionSummary a;
    a.ended_at = 1;
    SessionSummary b;
    b.ended_at = 2;
    log.add(a);
    log.add(b);
    REQUIRE(log.newest_first().size() == 2);
    CHECK(log.newest_first()[0].ended_at == 2);
    CHECK(log.newest_first()[1].ended_at == 1);
}
