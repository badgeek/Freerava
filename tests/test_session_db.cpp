#include "core/session_db.h"
#include "core/session_store.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <string>

using namespace core;

namespace {
std::string tmp_db() {
    // A unique-ish temp path; unlinked at the start of each test.
    static int n = 0;
    std::string p =
        std::string("/tmp/cyclomp_test_") + std::to_string(++n) + ".db";
    std::remove(p.c_str());
    std::remove((p + "-wal").c_str());
    std::remove((p + "-shm").c_str());
    return p;
}

SessionSummary make_session(std::time_t ended, double dist, int npts) {
    SessionSummary s;
    s.ended_at = ended;
    s.dist_km = dist;
    s.moving_s = 600;
    s.avg_kmh = 18.5;
    s.max_kmh = 33.2;
    s.has_cadence = true;
    s.avg_cadence = 82;
    s.has_hr = false;
    for (int i = 0; i < npts; ++i)
        s.track.push_back({-6.2 + i * 1e-4, 106.8 + i * 1e-4, (float)(10 + i),
                           (float)(i * 5), (float)(20 + i)});
    return s;
}
} // namespace

TEST_CASE("SessionStore round-trips sessions newest-first") {
    SessionStore db;
    REQUIRE(db.open(tmp_db()));

    db.add_session(make_session(1000, 3.0, 3));
    db.add_session(make_session(2000, 5.0, 4));
    db.add_session(make_session(1500, 4.0, 0)); // no track

    CHECK(db.session_count() == 3);

    SessionLog log;
    REQUIRE(db.load_all(log));
    const auto &rows = log.newest_first();
    REQUIRE(rows.size() == 3);
    // Newest ended_at first.
    CHECK(rows[0].ended_at == 2000);
    CHECK(rows[1].ended_at == 1500);
    CHECK(rows[2].ended_at == 1000);
    // Track + typed fields survive.
    CHECK(rows[0].track.size() == 4);
    CHECK(rows[0].has_cadence);
    CHECK(rows[0].avg_cadence == 82);
    CHECK(rows[1].track.empty());
    CHECK(rows[2].track.size() == 3);
    CHECK(rows[2].track[0].alt_m == doctest::Approx(20.0));
}

TEST_CASE("SessionStore active ride save / load / clear") {
    SessionStore db;
    REQUIRE(db.open(tmp_db()));

    ActiveRide none;
    CHECK_FALSE(db.load_active(none)); // nothing stored yet

    ActiveRide a;
    a.saved_at = 4242;
    a.engine.state = 2; // Paused
    a.engine.live.dist_km = 7.3;
    a.engine.live.elapsed_s = 812.5;
    a.engine.live.avg_kmh = 19.1;
    a.engine.sum_speed = 955.0;
    a.engine.max_kmh = 41.0;
    a.engine.samples = 50;
    a.engine.sum_cad = 4000;
    a.engine.cad_samples = 50;
    a.track.push_back({-6.20, 106.81, 12.f, 0.f, 20.f});
    a.track.push_back({-6.21, 106.82, 20.f, 60.f, 22.f});
    REQUIRE(db.save_active(a));

    ActiveRide b;
    REQUIRE(db.load_active(b));
    CHECK(b.saved_at == 4242);
    CHECK(b.engine.state == 2);
    CHECK(b.engine.live.dist_km == doctest::Approx(7.3));
    CHECK(b.engine.live.elapsed_s == doctest::Approx(812.5));
    CHECK(b.engine.samples == 50);
    CHECK(b.engine.sum_cad == 4000);
    REQUIRE(b.track.size() == 2);
    CHECK(b.track[1].t_s == doctest::Approx(60.0));

    // Saving again replaces (no duplicate rows).
    REQUIRE(db.save_active(a));
    ActiveRide c;
    REQUIRE(db.load_active(c));
    CHECK(c.track.size() == 2);

    REQUIRE(db.clear_active());
    ActiveRide gone;
    CHECK_FALSE(db.load_active(gone));
}

TEST_CASE("SessionStore imports a legacy text log") {
    // Write a legacy sessions.txt via the old writer, then import it.
    std::string txt = std::string("/tmp/cyclomp_legacy_") + ".txt";
    std::remove(txt.c_str());
    SessionLog seed;
    seed.add(make_session(1000, 3.0, 2));
    seed.add(make_session(2000, 5.0, 3)); // newest
    REQUIRE(save_sessions(txt, seed));

    SessionStore db;
    REQUIRE(db.open(tmp_db()));
    CHECK(db.import_text_log(txt) == 2);
    CHECK(db.session_count() == 2);

    SessionLog log;
    REQUIRE(db.load_all(log));
    REQUIRE(log.newest_first().size() == 2);
    CHECK(log.newest_first()[0].ended_at == 2000);
    std::remove(txt.c_str());
}
