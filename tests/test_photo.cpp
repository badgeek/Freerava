#include "core/photo.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace core;

namespace {
std::string tmp_file(const std::vector<uint8_t> &bytes, const char *tag) {
    std::string p = std::string("/tmp/cyclomp_photo_") + tag + ".bin";
    std::FILE *f = std::fopen(p.c_str(), "wb");
    if (f) { std::fwrite(bytes.data(), 1, bytes.size(), f); std::fclose(f); }
    return p;
}

// A minimal but structurally real JPEG: SOI, an APP1/Exif block whose IFD0
// points at an IFD1 carrying a thumbnail, then EOI. Little-endian TIFF.
std::vector<uint8_t> jpeg_with_thumb(const std::vector<uint8_t> &thumb) {
    std::vector<uint8_t> tiff;
    auto put16 = [&](uint16_t v) { tiff.push_back(v & 0xFF); tiff.push_back(v >> 8); };
    auto put32 = [&](uint32_t v) {
        tiff.push_back(v & 0xFF); tiff.push_back((v >> 8) & 0xFF);
        tiff.push_back((v >> 16) & 0xFF); tiff.push_back((v >> 24) & 0xFF);
    };
    tiff.push_back('I'); tiff.push_back('I');   // little-endian
    put16(42); put32(8);                        // magic, IFD0 offset
    // IFD0: zero entries, next = IFD1 at 8 + 2 + 4 = 14
    put16(0); put32(14);
    // IFD1: two entries (thumb offset/length), next = 0
    put16(2);
    const uint32_t entriesEnd = 14 + 2 + 2 * 12 + 4; // where the thumb will sit
    put16(0x0201); put16(4); put32(1); put32(entriesEnd);          // offset
    put16(0x0202); put16(4); put32(1); put32((uint32_t)thumb.size()); // length
    put32(0);
    tiff.insert(tiff.end(), thumb.begin(), thumb.end());

    std::vector<uint8_t> out{0xFF, 0xD8};
    const uint16_t len = (uint16_t)(2 + 6 + tiff.size()); // len + "Exif\0\0" + tiff
    out.push_back(0xFF); out.push_back(0xE1);
    out.push_back(len >> 8); out.push_back(len & 0xFF);
    for (char c : std::string("Exif")) out.push_back((uint8_t)c);
    out.push_back(0); out.push_back(0);
    out.insert(out.end(), tiff.begin(), tiff.end());
    out.push_back(0xFF); out.push_back(0xD9);
    return out;
}
} // namespace

TEST_CASE("base64 matches the RFC 4648 vectors (padding is the easy thing to get wrong)") {
    auto b = [](const char *s) {
        return std::vector<uint8_t>((const uint8_t *)s, (const uint8_t *)s + std::string(s).size());
    };
    CHECK(base64(b("")) == "");
    CHECK(base64(b("f")) == "Zg==");
    CHECK(base64(b("fo")) == "Zm8=");
    CHECK(base64(b("foo")) == "Zm9v");
    CHECK(base64(b("foob")) == "Zm9vYg==");
    CHECK(base64(b("fooba")) == "Zm9vYmE=");
    CHECK(base64(b("foobar")) == "Zm9vYmFy");
    // Binary-safe: high bytes must survive.
    CHECK(base64({0xFF, 0xD8, 0xFF}) == "/9j/");
}

TEST_CASE("exif_thumbnail pulls the JPEG out of IFD1") {
    // A "thumbnail" that is itself a valid little JPEG shell.
    std::vector<uint8_t> thumb{0xFF, 0xD8, 0xAA, 0xBB, 0xCC, 0xFF, 0xD9};
    const std::string p = tmp_file(jpeg_with_thumb(thumb), "thumb");

    const auto got = exif_thumbnail(p);
    REQUIRE(got.size() == thumb.size());
    CHECK(got == thumb);
    std::remove(p.c_str());
}

TEST_CASE("exif_thumbnail refuses anything it cannot vouch for") {
    // Not a JPEG at all.
    CHECK(exif_thumbnail(tmp_file({1, 2, 3, 4}, "junk")).empty());
    // A JPEG with no APP1 segment.
    CHECK(exif_thumbnail(tmp_file({0xFF, 0xD8, 0xFF, 0xD9}, "bare")).empty());
    // Missing file.
    CHECK(exif_thumbnail("/tmp/cyclomp_photo_does_not_exist.jpg").empty());
    // IFD1 present but the thumbnail bytes are not a JPEG -> rejected, so the
    // viewer never receives a data: URI it cannot draw.
    std::vector<uint8_t> notJpeg{0x00, 0x01, 0x02, 0x03};
    CHECK(exif_thumbnail(tmp_file(jpeg_with_thumb(notJpeg), "notjpeg")).empty());
    // Starts like a JPEG but never terminates: no EOI to trim back to.
    std::vector<uint8_t> noEoi{0xFF, 0xD8, 0x11, 0x22, 0x33};
    CHECK(exif_thumbnail(tmp_file(jpeg_with_thumb(noEoi), "noeoi")).empty());
}

TEST_CASE("exif_thumbnail trims the HAL's zero padding") {
    // Exactly the shape the Redmi produces: a real little JPEG followed by a
    // long zero run, all inside the declared length.
    std::vector<uint8_t> padded{0xFF, 0xD8, 0xAA, 0xBB, 0xFF, 0xD9};
    const size_t real = padded.size();
    padded.resize(real + 4096, 0x00);

    const std::string p = tmp_file(jpeg_with_thumb(padded), "padded");
    const auto got = exif_thumbnail(p);
    CHECK(got.size() == real); // padding gone, not 4102 bytes of mostly zeros
    CHECK(got[got.size() - 1] == 0xD9);
    std::remove(p.c_str());
}

// The generated fixtures above prove the parser; this proves it against what
// the Redmi's camera HAL actually produces. Skipped when the sample isn't
// present, so the suite stays green on a machine that never ran the spike.
TEST_CASE("exif_thumbnail handles a real camera JPEG" * doctest::skip(false)) {
    const char *real = "/tmp/selfie/selfie_1789377076.jpg";
    std::FILE *f = std::fopen(real, "rb");
    if (!f) return; // sample not on this machine
    std::fclose(f);

    const auto t = exif_thumbnail(real);
    REQUIRE(t.size() > 1000);
    CHECK(t[0] == 0xFF);            // SOI
    CHECK(t[1] == 0xD8);
    CHECK(t[t.size() - 2] == 0xFF); // EOI — only true because we trim the
    CHECK(t[t.size() - 1] == 0xD9); // HAL's zero padding off the slot
    // The slot this HAL declares is ~7x the image; make sure we didn't ship it.
    CHECK(t.size() < 20000);
    // Base64 of it is what actually ships in the GeoJSON.
    const auto b64 = base64(t);
    CHECK(b64.size() > 1000);
    CHECK(b64.substr(0, 4) == "/9j/"); // every JPEG starts FF D8 FF
}
