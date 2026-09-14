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

// add_session() takes a mutable reference (it stamps the new rowid onto the
// summary), so it needs a named local rather than a temporary. Returns the id.
long long add(SessionStore &db, std::time_t ended, double dist, int npts) {
    SessionSummary s = make_session(ended, dist, npts);
    return db.add_session(s) ? s.id : 0;
}
} // namespace

TEST_CASE("SessionStore round-trips sessions newest-first") {
    SessionStore db;
    REQUIRE(db.open(tmp_db()));

    add(db, 1000, 3.0, 3);
    add(db, 2000, 5.0, 4);
    add(db, 1500, 4.0, 0); // no track

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
    // Every loaded row carries the rowid the UI deletes by.
    CHECK(rows[0].id > 0);
    CHECK(rows[1].id != rows[0].id);
}

TEST_CASE("SessionStore deletes one session and its track") {
    SessionStore db;
    REQUIRE(db.open(tmp_db()));

    long long keep_old = add(db, 1000, 3.0, 3);
    long long doomed = add(db, 2000, 5.0, 4);
    long long keep_mid = add(db, 1500, 4.0, 0);
    REQUIRE(doomed > 0);
    CHECK(db.session_count() == 3);

    CHECK(db.delete_session(doomed));
    CHECK(db.session_count() == 2);

    SessionLog log;
    REQUIRE(db.load_all(log));
    const auto &rows = log.newest_first();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].id == keep_mid); // ended_at 1500
    CHECK(rows[1].id == keep_old); // ended_at 1000
    // The neighbours' tracks are untouched.
    CHECK(rows[1].track.size() == 3);

    // Deleting again, or an id that never existed, reports "nothing removed"
    // instead of silently succeeding.
    CHECK_FALSE(db.delete_session(doomed));
    CHECK_FALSE(db.delete_session(99999));
    CHECK_FALSE(db.delete_session(0));
    CHECK(db.session_count() == 2);
}

TEST_CASE("SessionLog::remove drops the right row") {
    SessionLog log;
    log.add(make_session(1000, 3.0, 0)); // becomes index 1 after the next add
    log.add(make_session(2000, 5.0, 0)); // newest -> index 0

    CHECK(log.remove(0));
    REQUIRE(log.newest_first().size() == 1);
    CHECK(log.newest_first()[0].ended_at == 1000);

    CHECK_FALSE(log.remove(5)); // out of range is a no-op
    CHECK(log.newest_first().size() == 1);
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

TEST_CASE("photos are taken unbound, then claimed by the finished ride") {
    SessionStore db;
    REQUIRE(db.open(tmp_db()));

    // Mid-ride: the session does not exist yet, so photos land with id 0.
    Photo a; a.taken_at = 1000; a.lat = -7.73; a.lon = 110.38; a.has_fix = true;
    a.t_s = 12.5; a.path = "/files/photos/a.jpg";
    Photo b; b.taken_at = 1100; b.has_fix = false; b.t_s = 90; b.path = "/files/photos/b.jpg";
    REQUIRE(db.add_photo(a));
    REQUIRE(db.add_photo(b));
    CHECK(a.id > 0);
    CHECK(b.id != a.id);

    // Nothing is visible under a session until it is bound.
    std::vector<Photo> none;
    REQUIRE(db.photos_for(42, none));
    CHECK(none.empty());

    // The ride ends and takes ownership of everything outstanding.
    SessionSummary s = make_session(2000, 5.0, 3);
    REQUIRE(db.add_session(s));
    CHECK(db.bind_photos(s.id) == 2);
    CHECK(db.bind_photos(s.id) == 0); // nothing left unbound

    std::vector<Photo> got;
    REQUIRE(db.photos_for(s.id, got));
    REQUIRE(got.size() == 2);
    CHECK(got[0].taken_at == 1000);      // ordered by capture time
    CHECK(got[1].taken_at == 1100);
    CHECK(got[0].has_fix);
    CHECK_FALSE(got[1].has_fix);         // shutter beat the first fix
    CHECK(got[0].lat == doctest::Approx(-7.73));
    CHECK(got[0].t_s == doctest::Approx(12.5));
    CHECK(got[0].path == "/files/photos/a.jpg");
    CHECK(got[0].session_id == s.id);
}

TEST_CASE("a later ride cannot steal the previous ride's photos") {
    SessionStore db;
    REQUIRE(db.open(tmp_db()));

    Photo p1; p1.taken_at = 10; p1.path = "/1.jpg";
    REQUIRE(db.add_photo(p1));
    SessionSummary s1 = make_session(100, 1.0, 0);
    REQUIRE(db.add_session(s1));
    CHECK(db.bind_photos(s1.id) == 1);

    Photo p2; p2.taken_at = 20; p2.path = "/2.jpg";
    REQUIRE(db.add_photo(p2));
    SessionSummary s2 = make_session(200, 2.0, 0);
    REQUIRE(db.add_session(s2));
    CHECK(db.bind_photos(s2.id) == 1); // only the new one

    std::vector<Photo> a, b;
    REQUIRE(db.photos_for(s1.id, a));
    REQUIRE(db.photos_for(s2.id, b));
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == 1);
    CHECK(a[0].path == "/1.jpg");
    CHECK(b[0].path == "/2.jpg");

    // Deleting one ride's photos leaves the other's alone.
    CHECK(db.delete_photos(s1.id));
    std::vector<Photo> a2, b2;
    db.photos_for(s1.id, a2);
    db.photos_for(s2.id, b2);
    CHECK(a2.empty());
    CHECK(b2.size() == 1);
}
