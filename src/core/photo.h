// cyclomp core — ride photos: the typed record, plus the two pure helpers the
// export needs (pull the JPEG's own EXIF thumbnail, base64 it).
//
// No image decoding anywhere: the camera HAL already embeds a small JPEG
// thumbnail in the EXIF APP1 segment (176x128 on the Redmi), so the viewer can
// be handed a picture without this project ever linking an image library.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace core {

struct Photo {
    long long id = 0;          // SQLite rowid once stored
    long long session_id = 0;  // 0 = taken mid-ride, not yet bound to a session
    std::time_t taken_at = 0;
    double lat = 0, lon = 0;
    bool has_fix = false;      // false when the shutter beat the first GPS fix
    bool front = true;         // front lens = a selfie; back = the view ahead
    double t_s = 0;            // seconds into the ride, for ordering on the track
    std::string path;          // full-resolution JPEG on the device
};

// The JPEG thumbnail the camera stored in EXIF IFD1, or empty when the file has
// none (not every HAL writes one). Bytes are a complete JPEG.
std::vector<uint8_t> exif_thumbnail(const std::string &jpeg_path);

// Standard base64, no line breaks — destined for a data: URI.
std::string base64(const std::vector<uint8_t> &bytes);

} // namespace core
