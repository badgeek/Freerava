// cyclomp core — SQLite persistence for finished rides and the single
// in-progress ride. Crash-safe (transactions) and incremental, unlike the
// legacy full-rewrite text store in session_store.h (still used to migrate
// old data in once). Pure C++: sqlite3 is a plain C dependency, no Slint /
// mbgl / Android here.
#pragma once

#include "core/photo.h"
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
    // leave a half-written row the way the text store could). On success `s.id`
    // is set to the new rowid so the caller can delete it later.
    bool add_session(SessionSummary &s);
    // Rebuilds `out` newest-first (matches SessionLog's own ordering).
    bool load_all(SessionLog &out) const;
    long session_count() const;
    // Drop one stored session and its trackpoints. True when a row went away;
    // false if `id` matched nothing (already deleted, or never persisted).
    bool delete_session(long long id);

    // ---- Ride photos ----
    // A selfie is taken mid-ride, before the session it belongs to has a rowid,
    // so it lands with session_id 0 and is claimed by bind_photos() when the
    // ride is finalised. On success `p.id` is set.
    bool add_photo(Photo &p);
    // Give every unbound photo to `session_id`. Returns how many were claimed.
    long bind_photos(long long session_id);
    bool photos_for(long long session_id, std::vector<Photo> &out) const;
    // Dropped alongside their session; the JPEGs on disk are the caller's
    // problem (this store never deletes files it did not write).
    bool delete_photos(long long session_id);

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
