// cyclomp core — plain-text persistence for the session log (+tracks).
// One "S" line per session followed by its "P" point lines. No JSON dep.
#pragma once

#include "core/ride_engine.h"

#include <string>

namespace core {

bool save_sessions(const std::string &path, const SessionLog &log);
bool load_sessions(const std::string &path, SessionLog &out); // appends oldest-first

} // namespace core
