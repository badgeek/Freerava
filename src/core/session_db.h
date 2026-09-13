// cyclomp core — SQLite persistence for finished rides and the single
// in-progress ride. Crash-safe (transactions) and incremental, unlike the
// legacy full-rewrite text store in session_store.h (still used to migrate
// old data in once). Pure C++: sqlite3 is a plain C dependency, no Slint /
// mbgl / Android here.
#pragma once

#include "core/ride_engine.h"
#include "core/track.h"

#include <ctime>
#include <string>
#include <vector>

struct sqlite3; // opaque; only session_db.cpp includes <sqlite3.h>

namespace core {

// A ride interrupted mid-flight (Running or Paused): the engine's full state
// plus the track recorded so far. Persisted on pause / periodically, so the
// app being killed in the background doesn't lose it.
struct ActiveRide {
    RideEngine::Snapshot engine;
    std::vector<TrackPoint> track;
    std::time_t saved_at = 0;
};

class SessionStore {
public:
    SessionStore() = default;
    ~SessionStore();
    SessionStore(const SessionStore &) = delete;
    SessionStore &operator=(const SessionStore &) = delete;

    // Open (creating it and the schema if needed). False on failure.
    bool open(const std::string &db_path);
    void close();
    bool ok() const { return db_ != nullptr; }

    // ---- Finished sessions ----
    // One session + its track in a single transaction (atomic; a crash can't
    // leave a half-written row the way the text store could).
    bool add_session(const SessionSummary &s);
    // Rebuilds `out` newest-first (matches SessionLog's own ordering).
    bool load_all(SessionLog &out) const;
    long session_count() const;

    // ---- In-progress ride (single slot) ----
    bool save_active(const ActiveRide &a);
    bool load_active(ActiveRide &out) const; // false when none is stored
    bool clear_active();

    // One-time import of a legacy sessions.txt into the DB (used when the DB
    // is empty but an old text log exists). Returns rows imported, -1 on error.
    long import_text_log(const std::string &sessions_txt_path);

private:
    bool exec(const char *sql);
    sqlite3 *db_ = nullptr;
};

} // namespace core
