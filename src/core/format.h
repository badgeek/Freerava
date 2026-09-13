// cyclomp core — pure formatting helpers (no Slint, unit-tested).
#pragma once

#include <ctime>
#include <string>

namespace core::fmt {

std::string fmt1(double v);            // one decimal, "12.3"
std::string fmt0(double v);            // rounded integer, "18"
std::string fmt_int(int v);
std::string mmss(int seconds);         // "MM:SS"
std::string metres(int m);             // "400 m" / "1.2 km"
std::string date_label(std::time_t t); // "07 SEP 14:32" (localtime, uppercased)

} // namespace core::fmt
