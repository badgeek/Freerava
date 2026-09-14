#include "core/session_db.h"

#include "core/session_store.h" // legacy text reader, for one-time migration

#include <sqlite3.h>

namespace core {

namespace {

constexpr const char *kSchema =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS session("
    " id INTEGER PRIMARY KEY,"
    " ended_at INTEGER, dist_km REAL, moving_s REAL, avg_kmh REAL,"
    " max_kmh REAL, avg_cadence INTEGER, has_cadence INTEGER,"
    " avg_hr INTEGER, has_hr INTEGER);"
    "CREATE TABLE IF NOT EXISTS trackpoint("
    " session_id INTEGER, seq INTEGER, lat REAL, lon REAL,"
    " speed REAL, t_s REAL, alt REAL);"
    "CREATE INDEX IF NOT EXISTS idx_tp ON trackpoint(session_id, seq);"
    // One-row tables for the in-progress ride (id/rowid fixed at 1).
    "CREATE TABLE IF NOT EXISTS active_ride("
    " id INTEGER PRIMARY KEY CHECK(id=1), state INTEGER, saved_at INTEGER,"
    " speed REAL, dist_km REAL, elapsed_s REAL, avg_kmh REAL, lat REAL,"
    " lon REAL, cadence INTEGER, heart_rate INTEGER, sum_speed REAL,"
    " max_kmh REAL, sum_hr INTEGER, sum_cad INTEGER, samples INTEGER,"
    " hr_samples INTEGER, cad_samples INTEGER);"
    "CREATE TABLE IF NOT EXISTS active_trackpoint("
    " seq INTEGER, lat REAL, lon REAL, speed REAL, t_s REAL, alt REAL);";

} // namespace

SessionStore::~SessionStore() { close(); }

bool SessionStore::exec(const char *sql) {
    char *err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool SessionStore::open(const std::string &db_path) {
    close();
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        close();
        return false;
    }
    sqlite3_busy_timeout(db_, 2000);
    if (!exec(kSchema)) {
        close();
        return false;
    }
    return true;
}

void SessionStore::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SessionStore::add_session(SessionSummary &s) {
    if (!db_) return false;
    if (!exec("BEGIN")) return false;
    bool okv = true;
    sqlite3_int64 sid = 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO session(ended_at,dist_km,moving_s,"
                           "avg_kmh,max_kmh,avg_cadence,has_cadence,avg_hr,"
                           "has_hr) VALUES(?,?,?,?,?,?,?,?,?)",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)s.ended_at);
        sqlite3_bind_double(st, 2, s.dist_km);
        sqlite3_bind_double(st, 3, s.moving_s);
        sqlite3_bind_double(st, 4, s.avg_kmh);
        sqlite3_bind_double(st, 5, s.max_kmh);
        sqlite3_bind_int(st, 6, s.avg_cadence);
        sqlite3_bind_int(st, 7, s.has_cadence ? 1 : 0);
        sqlite3_bind_int(st, 8, s.avg_hr);
        sqlite3_bind_int(st, 9, s.has_hr ? 1 : 0);
        okv = sqlite3_step(st) == SQLITE_DONE;
        if (okv) sid = sqlite3_last_insert_rowid(db_);
        sqlite3_finalize(st);
    } else {
        okv = false;
    }

    if (okv && !s.track.empty()) {
        sqlite3_stmt *tp = nullptr;
        if (sqlite3_prepare_v2(db_,
                               "INSERT INTO trackpoint(session_id,seq,lat,lon,"
                               "speed,t_s,alt) VALUES(?,?,?,?,?,?,?)",
                               -1, &tp, nullptr) == SQLITE_OK) {
            for (size_t i = 0; okv && i < s.track.size(); ++i) {
                const TrackPoint &p = s.track[i];
                sqlite3_bind_int64(tp, 1, sid);
                sqlite3_bind_int(tp, 2, (int)i);
                sqlite3_bind_double(tp, 3, p.lat);
                sqlite3_bind_double(tp, 4, p.lon);
                sqlite3_bind_double(tp, 5, p.speed_kmh);
                sqlite3_bind_double(tp, 6, p.t_s);
                sqlite3_bind_double(tp, 7, p.alt_m);
                okv = sqlite3_step(tp) == SQLITE_DONE;
                sqlite3_reset(tp);
            }
            sqlite3_finalize(tp);
        } else {
            okv = false;
        }
    }

    exec(okv ? "COMMIT" : "ROLLBACK");
    if (okv) s.id = (long long)sid;
    return okv;
}

bool SessionStore::delete_session(long long id) {
    if (!db_ || id <= 0) return false;
    if (!exec("BEGIN")) return false;
    bool okv = true;
    int removed = 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM session WHERE id=?", -1, &st,
                           nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        okv = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (okv) removed = sqlite3_changes(db_);
    } else {
        okv = false;
    }

    // No foreign keys in the schema, so the track goes with it by hand.
    if (okv) {
        sqlite3_stmt *tp = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM trackpoint WHERE session_id=?",
                               -1, &tp, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(tp, 1, (sqlite3_int64)id);
            okv = sqlite3_step(tp) == SQLITE_DONE;
            sqlite3_finalize(tp);
        } else {
            okv = false;
        }
    }

    exec(okv ? "COMMIT" : "ROLLBACK");
    return okv && removed > 0;
}

bool SessionStore::load_all(SessionLog &out) const {
    if (!db_) return false;
    sqlite3_stmt *st = nullptr;
    // Oldest first, so SessionLog::add (which prepends) yields newest-first.
    if (sqlite3_prepare_v2(db_,
                           "SELECT id,ended_at,dist_km,moving_s,avg_kmh,"
                           "max_kmh,avg_cadence,has_cadence,avg_hr,has_hr "
                           "FROM session ORDER BY ended_at ASC, id ASC",
                           -1, &st, nullptr) != SQLITE_OK)
        return false;

    sqlite3_stmt *tp = nullptr;
    sqlite3_prepare_v2(db_,
                       "SELECT lat,lon,speed,t_s,alt FROM trackpoint "
                       "WHERE session_id=? ORDER BY seq ASC",
                       -1, &tp, nullptr);

    while (sqlite3_step(st) == SQLITE_ROW) {
        SessionSummary s;
        sqlite3_int64 id = sqlite3_column_int64(st, 0);
        s.id = (long long)id;
        s.ended_at = (std::time_t)sqlite3_column_int64(st, 1);
        s.dist_km = sqlite3_column_double(st, 2);
        s.moving_s = sqlite3_column_double(st, 3);
        s.avg_kmh = sqlite3_column_double(st, 4);
        s.max_kmh = sqlite3_column_double(st, 5);
        s.avg_cadence = sqlite3_column_int(st, 6);
        s.has_cadence = sqlite3_column_int(st, 7) != 0;
        s.avg_hr = sqlite3_column_int(st, 8);
        s.has_hr = sqlite3_column_int(st, 9) != 0;
        if (tp) {
            sqlite3_bind_int64(tp, 1, id);
            while (sqlite3_step(tp) == SQLITE_ROW) {
                s.track.push_back({sqlite3_column_double(tp, 0),
                                   sqlite3_column_double(tp, 1),
                                   (float)sqlite3_column_double(tp, 2),
                                   (float)sqlite3_column_double(tp, 3),
                                   (float)sqlite3_column_double(tp, 4)});
            }
            sqlite3_reset(tp);
        }
        out.add(s);
    }
    if (tp) sqlite3_finalize(tp);
    sqlite3_finalize(st);
    return true;
}

long SessionStore::session_count() const {
    if (!db_) return 0;
    sqlite3_stmt *st = nullptr;
    long n = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM session", -1, &st,
                           nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = (long)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

bool SessionStore::save_active(const ActiveRide &a) {
    if (!db_) return false;
    if (!exec("BEGIN")) return false;
    bool okv = exec("DELETE FROM active_ride; DELETE FROM active_trackpoint;");

    if (okv) {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(
                db_,
                "INSERT INTO active_ride(id,state,saved_at,speed,dist_km,"
                "elapsed_s,avg_kmh,lat,lon,cadence,heart_rate,sum_speed,"
                "max_kmh,sum_hr,sum_cad,samples,hr_samples,cad_samples) "
                "VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                -1, &st, nullptr) == SQLITE_OK) {
            const auto &e = a.engine;
            sqlite3_bind_int(st, 1, e.state);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)a.saved_at);
            sqlite3_bind_double(st, 3, e.live.speed_kmh);
            sqlite3_bind_double(st, 4, e.live.dist_km);
            sqlite3_bind_double(st, 5, e.live.elapsed_s);
            sqlite3_bind_double(st, 6, e.live.avg_kmh);
            sqlite3_bind_double(st, 7, e.live.lat);
            sqlite3_bind_double(st, 8, e.live.lon);
            sqlite3_bind_int(st, 9, e.live.cadence);
            sqlite3_bind_int(st, 10, e.live.heart_rate);
            sqlite3_bind_double(st, 11, e.sum_speed);
            sqlite3_bind_double(st, 12, e.max_kmh);
            sqlite3_bind_int64(st, 13, e.sum_hr);
            sqlite3_bind_int64(st, 14, e.sum_cad);
            sqlite3_bind_int(st, 15, e.samples);
            sqlite3_bind_int(st, 16, e.hr_samples);
            sqlite3_bind_int(st, 17, e.cad_samples);
            okv = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        } else {
            okv = false;
        }
    }

    if (okv && !a.track.empty()) {
        sqlite3_stmt *tp = nullptr;
        if (sqlite3_prepare_v2(db_,
                               "INSERT INTO active_trackpoint(seq,lat,lon,"
                               "speed,t_s,alt) VALUES(?,?,?,?,?,?)",
                               -1, &tp, nullptr) == SQLITE_OK) {
            for (size_t i = 0; okv && i < a.track.size(); ++i) {
                const TrackPoint &p = a.track[i];
                sqlite3_bind_int(tp, 1, (int)i);
                sqlite3_bind_double(tp, 2, p.lat);
                sqlite3_bind_double(tp, 3, p.lon);
                sqlite3_bind_double(tp, 4, p.speed_kmh);
                sqlite3_bind_double(tp, 5, p.t_s);
                sqlite3_bind_double(tp, 6, p.alt_m);
                okv = sqlite3_step(tp) == SQLITE_DONE;
                sqlite3_reset(tp);
            }
            sqlite3_finalize(tp);
        } else {
            okv = false;
        }
    }

    exec(okv ? "COMMIT" : "ROLLBACK");
    return okv;
}

bool SessionStore::load_active(ActiveRide &out) const {
    if (!db_) return false;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT state,saved_at,speed,dist_km,elapsed_s,"
                           "avg_kmh,lat,lon,cadence,heart_rate,sum_speed,"
                           "max_kmh,sum_hr,sum_cad,samples,hr_samples,"
                           "cad_samples FROM active_ride WHERE id=1",
                           -1, &st, nullptr) != SQLITE_OK)
        return false;
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    auto &e = out.engine;
    e.state = sqlite3_column_int(st, 0);
    out.saved_at = (std::time_t)sqlite3_column_int64(st, 1);
    e.live.speed_kmh = sqlite3_column_double(st, 2);
    e.live.dist_km = sqlite3_column_double(st, 3);
    e.live.elapsed_s = sqlite3_column_double(st, 4);
    e.live.avg_kmh = sqlite3_column_double(st, 5);
    e.live.lat = sqlite3_column_double(st, 6);
    e.live.lon = sqlite3_column_double(st, 7);
    e.live.cadence = sqlite3_column_int(st, 8);
    e.live.heart_rate = sqlite3_column_int(st, 9);
    e.sum_speed = sqlite3_column_double(st, 10);
    e.max_kmh = sqlite3_column_double(st, 11);
    e.sum_hr = (long)sqlite3_column_int64(st, 12);
    e.sum_cad = (long)sqlite3_column_int64(st, 13);
    e.samples = sqlite3_column_int(st, 14);
    e.hr_samples = sqlite3_column_int(st, 15);
    e.cad_samples = sqlite3_column_int(st, 16);
    sqlite3_finalize(st);

    out.track.clear();
    sqlite3_stmt *tp = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT lat,lon,speed,t_s,alt FROM "
                           "active_trackpoint ORDER BY seq ASC",
                           -1, &tp, nullptr) == SQLITE_OK) {
        while (sqlite3_step(tp) == SQLITE_ROW) {
            out.track.push_back({sqlite3_column_double(tp, 0),
                                 sqlite3_column_double(tp, 1),
                                 (float)sqlite3_column_double(tp, 2),
                                 (float)sqlite3_column_double(tp, 3),
                                 (float)sqlite3_column_double(tp, 4)});
        }
        sqlite3_finalize(tp);
    }
    return true;
}

bool SessionStore::clear_active() {
    if (!db_) return false;
    return exec("DELETE FROM active_ride; DELETE FROM active_trackpoint;");
}

long SessionStore::import_text_log(const std::string &sessions_txt_path) {
    if (!db_) return -1;
    SessionLog legacy;
    if (!load_sessions(sessions_txt_path, legacy)) return 0; // no/empty file
    const auto &rows = legacy.newest_first();
    long n = 0;
    // Oldest first so ids ascend with time (rows is newest-first).
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        SessionSummary s = *it; // add_session stamps the new rowid onto it
        if (add_session(s)) ++n;
    }
    return n;
}

} // namespace core
