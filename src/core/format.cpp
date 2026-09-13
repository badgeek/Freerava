#include "core/format.h"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace core::fmt {

std::string fmt1(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

std::string fmt0(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d", (int)std::lround(v));
    return buf;
}

std::string fmt_int(int v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d", v);
    return buf;
}

std::string mmss(int seconds) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d", seconds / 60, seconds % 60);
    return buf;
}

std::string metres(int m) {
    char buf[24];
    if (m >= 1000) std::snprintf(buf, sizeof buf, "%.1f km", m / 1000.f);
    else           std::snprintf(buf, sizeof buf, "%d m", m);
    return buf;
}

std::string date_label(std::time_t t) {
    char buf[32];
    std::strftime(buf, sizeof buf, "%d %b %H:%M", std::localtime(&t));
    for (char *p = buf; *p; ++p) *p = (char)std::toupper((unsigned char)*p);
    return buf;
}

} // namespace core::fmt
