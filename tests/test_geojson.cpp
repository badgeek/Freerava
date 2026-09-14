#include "core/geojson.h"

#include <doctest/doctest.h>

using namespace core;

TEST_CASE("session_to_geojson emits a valid FeatureCollection with a track") {
    SessionSummary s;
    s.ended_at = 1757740000;
    s.dist_km = 5.234;
    s.moving_s = 1200;
    s.avg_kmh = 15.7;
    s.max_kmh = 32.1;
    s.has_hr = true;
    s.avg_hr = 143;
    s.track.push_back({-6.20, 106.81, 12.5f, 0.f, 20.f});
    s.track.push_back({-6.21, 106.82, 25.0f, 60.f, 25.f});

    std::string g = session_to_geojson(s);

    CHECK(g.find("\"type\":\"FeatureCollection\"") != std::string::npos);
    CHECK(g.find("\"LineString\"") != std::string::npos);
    // GeoJSON axis order is [lon,lat,alt].
    CHECK(g.find("[106.8100000,-6.2000000,20.00]") != std::string::npos);
    CHECK(g.find("\"speed_kmh\":[12.500,25.000]") != std::string::npos);
    CHECK(g.find("\"t_s\":[0.00,60.00]") != std::string::npos);
    CHECK(g.find("\"avg_hr\":143") != std::string::npos);
    CHECK(g.find("\"has_hr\":true") != std::string::npos);
    // Balanced-ish braces/brackets as a cheap sanity check.
    CHECK(g.front() == '{');
    CHECK(g.back() == '}');
}

TEST_CASE("session_to_geojson stays valid for an empty track") {
    SessionSummary s;
    s.ended_at = 42;
    std::string g = session_to_geojson(s);
    CHECK(g.find("\"features\":[]") != std::string::npos);
    CHECK(g.find("\"LineString\"") == std::string::npos);
}

namespace {
// A JPEG carrying a tiny EXIF thumbnail, written to disk so the exporter has
// something real to read. Mirrors the fixture in test_photo.cpp.
std::string photo_fixture(const char *tag) {
    std::vector<uint8_t> thumb{0xFF, 0xD8, 0x41, 0x42, 0xFF, 0xD9};
    std::vector<uint8_t> tiff;
    auto put16 = [&](uint16_t v) { tiff.push_back(v & 0xFF); tiff.push_back(v >> 8); };
    auto put32 = [&](uint32_t v) {
        tiff.push_back(v & 0xFF); tiff.push_back((v >> 8) & 0xFF);
        tiff.push_back((v >> 16) & 0xFF); tiff.push_back((v >> 24) & 0xFF);
    };
    tiff.push_back('I'); tiff.push_back('I'); put16(42); put32(8);
    put16(0); put32(14);
    put16(2);
    const uint32_t at = 14 + 2 + 2 * 12 + 4;
    put16(0x0201); put16(4); put32(1); put32(at);
    put16(0x0202); put16(4); put32(1); put32((uint32_t)thumb.size());
    put32(0);
    tiff.insert(tiff.end(), thumb.begin(), thumb.end());

    std::vector<uint8_t> j{0xFF, 0xD8};
    const uint16_t len = (uint16_t)(2 + 6 + tiff.size());
    j.push_back(0xFF); j.push_back(0xE1);
    j.push_back(len >> 8); j.push_back(len & 0xFF);
    for (char c : std::string("Exif")) j.push_back((uint8_t)c);
    j.push_back(0); j.push_back(0);
    j.insert(j.end(), tiff.begin(), tiff.end());
    j.push_back(0xFF); j.push_back(0xD9);

    std::string p = std::string("/tmp/cyclomp_gj_") + tag + ".jpg";
    std::FILE *f = std::fopen(p.c_str(), "wb");
    if (f) { std::fwrite(j.data(), 1, j.size(), f); std::fclose(f); }
    return p;
}
} // namespace

TEST_CASE("photos become Point features carrying their thumbnail") {
    SessionSummary s;
    s.ended_at = 5000;
    s.track.push_back({-6.20, 106.81, 10.f, 0.f, 20.f});
    s.track.push_back({-6.30, 106.91, 20.f, 100.f, 25.f});

    Photo fixed;
    fixed.path = photo_fixture("fixed");
    fixed.has_fix = true; fixed.lat = -6.25; fixed.lon = 106.86;
    fixed.taken_at = 4321; fixed.t_s = 50;

    const std::string g = session_to_geojson(s, {fixed});
    CHECK(g.find("\"kind\":\"photo\"") != std::string::npos);
    CHECK(g.find("[106.8600000,-6.2500000]") != std::string::npos); // its own fix
    CHECK(g.find("\"taken_at\":4321") != std::string::npos);
    CHECK(g.find("\"has_fix\":true") != std::string::npos);
    // The fixture's third byte isn't 0xFF, so this won't be the familiar
    // "/9j/" prefix — that shape is asserted against a real camera JPEG in
    // test_photo.cpp. Here, just that a non-empty data URI was emitted.
    CHECK(g.find("data:image/jpeg;base64,/9hB") != std::string::npos);
    // The track feature is still there alongside it.
    CHECK(g.find("\"LineString\"") != std::string::npos);
    std::remove(fixed.path.c_str());
}

TEST_CASE("a photo with no fix is placed on the track by elapsed time") {
    SessionSummary s;
    s.track.push_back({-6.00, 106.00, 10.f, 0.f, 0.f});
    s.track.push_back({-6.10, 106.10, 10.f, 100.f, 0.f});

    Photo drifting;
    drifting.path = photo_fixture("nofix");
    drifting.has_fix = false; drifting.t_s = 50; // exactly halfway

    const std::string g = session_to_geojson(s, {drifting});
    CHECK(g.find("\"has_fix\":false") != std::string::npos);
    // Interpolated to the midpoint rather than dumped at 0,0.
    CHECK(g.find("[106.0500000,-6.0500000]") != std::string::npos);
    CHECK(g.find("[0.0000000,0.0000000]") == std::string::npos);
    std::remove(drifting.path.c_str());
}

TEST_CASE("unusable photos are dropped, and one can be the only feature") {
    SessionSummary s; // no track at all

    Photo missing;    // file does not exist -> no thumbnail -> skipped
    missing.path = "/tmp/cyclomp_gj_nope.jpg";
    missing.has_fix = true; missing.lat = 1; missing.lon = 2;

    const std::string none = session_to_geojson(s, {missing});
    CHECK(none.find("\"features\":[]") != std::string::npos);

    // A single photo with no track must still yield valid JSON: the feature
    // array cannot open with a stray comma.
    Photo solo;
    solo.path = photo_fixture("solo");
    solo.has_fix = true; solo.lat = -7.7; solo.lon = 110.3;
    const std::string g = session_to_geojson(s, {solo});
    CHECK(g.find("\"features\":[{") != std::string::npos);
    CHECK(g.find("[,") == std::string::npos);
    std::remove(solo.path.c_str());
}
