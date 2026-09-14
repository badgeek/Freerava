#include "core/photo.h"

#include <cstdio>

namespace core {
namespace {

// --- tiny endian-aware readers over the TIFF block inside EXIF ---------------
uint16_t rd16(const uint8_t *p, bool le) {
    return le ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)((p[0] << 8) | p[1]);
}
uint32_t rd32(const uint8_t *p, bool le) {
    return le ? (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24))
              : (uint32_t)(((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

std::vector<uint8_t> read_file(const std::string &path) {
    std::vector<uint8_t> out;
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize((size_t)n);
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return out;
}

} // namespace

std::vector<uint8_t> exif_thumbnail(const std::string &jpeg_path) {
    const std::vector<uint8_t> d = read_file(jpeg_path);
    if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8) return {}; // not a JPEG

    // Walk the marker segments looking for APP1/Exif. Every segment after SOI
    // carries a big-endian length; SOS means the entropy-coded data starts and
    // there is nothing more to find.
    size_t i = 2;
    while (i + 4 <= d.size() && d[i] == 0xFF) {
        const uint8_t marker = d[i + 1];
        if (marker == 0xD8 || marker == 0xD9) { i += 2; continue; }
        const size_t len = (size_t)rd16(&d[i + 2], false);
        if (marker == 0xDA || len < 2 || i + 2 + len > d.size()) break;

        if (marker == 0xE1 && len >= 8 && std::string((const char *)&d[i + 4], 4) == "Exif") {
            const uint8_t *tiff = &d[i + 10];             // skip "Exif\0\0"
            const size_t tiffLen = len - 8;
            if (tiffLen < 8) return {};
            const bool le = tiff[0] == 'I' && tiff[1] == 'I';
            const uint32_t ifd0 = rd32(tiff + 4, le);
            if (ifd0 + 2 > tiffLen) return {};
            const uint16_t n0 = rd16(tiff + ifd0, le);
            const size_t nextPos = ifd0 + 2 + (size_t)n0 * 12;
            if (nextPos + 4 > tiffLen) return {};
            const uint32_t ifd1 = rd32(tiff + nextPos, le); // thumbnail IFD
            if (!ifd1 || ifd1 + 2 > tiffLen) return {};

            const uint16_t n1 = rd16(tiff + ifd1, le);
            uint32_t off = 0, bytes = 0;
            for (uint16_t k = 0; k < n1; ++k) {
                const size_t e = ifd1 + 2 + (size_t)k * 12;
                if (e + 12 > tiffLen) break;
                const uint16_t tag = rd16(tiff + e, le);
                const uint32_t val = rd32(tiff + e + 8, le);
                if (tag == 0x0201) off = val;    // JPEGInterchangeFormat
                if (tag == 0x0202) bytes = val;  // ...Length
            }
            if (!off || !bytes || (size_t)off + bytes > tiffLen) return {};
            const uint8_t *t = tiff + off;
            if (bytes < 2 || t[0] != 0xFF || t[1] != 0xD8) return {}; // not a JPEG

            // JPEGInterchangeFormatLength is the SLOT the HAL reserved, not the
            // image: the Redmi declares 40576 bytes for a 5719-byte thumbnail
            // and zero-pads the rest. Base64-ing the declared length would ship
            // ~34 KB of zeros per photo into the export, so trim to the real
            // end-of-image marker.
            size_t end = bytes;
            while (end >= 2 && !(t[end - 2] == 0xFF && t[end - 1] == 0xD9)) --end;
            if (end < 2) return {}; // no EOI anywhere: not a usable JPEG
            return std::vector<uint8_t>(t, t + end);
        }
        i += 2 + len;
    }
    return {};
}

std::string base64(const std::vector<uint8_t> &b) {
    static const char *T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((b.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < b.size(); i += 3) {
        const uint32_t v = ((uint32_t)b[i] << 16) | ((uint32_t)b[i + 1] << 8) | b[i + 2];
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];
        out += T[v & 63];
    }
    if (i + 1 == b.size()) {              // one byte left
        const uint32_t v = (uint32_t)b[i] << 16;
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == b.size()) {       // two bytes left
        const uint32_t v = ((uint32_t)b[i] << 16) | ((uint32_t)b[i + 1] << 8);
        out += T[(v >> 18) & 63];
        out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

} // namespace core
