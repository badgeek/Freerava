#include <doctest/doctest.h>

#include "core/format.h"

#include <cstdlib>
#include <ctime>

using namespace core::fmt;

TEST_CASE("fmt1 keeps one decimal") {
    CHECK(fmt1(12.34) == "12.3");
    CHECK(fmt1(0.0) == "0.0");
    CHECK(fmt1(0.05) == "0.1");
}

TEST_CASE("fmt0 rounds to nearest integer") {
    CHECK(fmt0(17.5) == "18");
    CHECK(fmt0(17.4) == "17");
    CHECK(fmt0(0.0) == "0");
}

TEST_CASE("fmt_int") {
    CHECK(fmt_int(0) == "0");
    CHECK(fmt_int(142) == "142");
    CHECK(fmt_int(-3) == "-3");
}

TEST_CASE("mmss") {
    CHECK(mmss(0) == "00:00");
    CHECK(mmss(61) == "01:01");
    CHECK(mmss(3599) == "59:59");
}

TEST_CASE("metres switches to km at 1000") {
    CHECK(metres(999) == "999 m");
    CHECK(metres(1000) == "1.0 km");
    CHECK(metres(1200) == "1.2 km");
    CHECK(metres(0) == "0 m");
}

TEST_CASE("date_label formats and uppercases (UTC)") {
    setenv("TZ", "UTC", 1);
    tzset();
    // 2026-09-07 14:32:00 UTC
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 8; // September
    tm.tm_mday = 7;
    tm.tm_hour = 14;
    tm.tm_min = 32;
    std::time_t t = timegm(&tm);
    CHECK(date_label(t) == "07 SEP 14:32");
}
