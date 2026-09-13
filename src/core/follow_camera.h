// cyclomp core — camera follow-mode decisions (no map library involved).
// The app wires the resulting CameraUpdate into mapgl::/MapService.
#pragma once

#include <optional>

namespace core {

struct CameraUpdate {
    double lat = 0;
    double lon = 0;
    double zoom = 0; // zoom < 0 => keep the map's current zoom
};

class FollowCamera {
public:
    explicit FollowCamera(double initial_zoom = 15.0) : zoom_(initial_zoom) {}

    void relock() { follow_ = true; }   // START pressed
    void release() { follow_ = false; } // user pan/pinch
    bool following() const { return follow_; }
    // False until the first on_position() has placed the camera.
    bool placed() const { return !first_; }

    // Update for a new rider position; nullopt when released. The very
    // FIRST call always fires and carries the initial zoom.
    std::optional<CameraUpdate> on_position(double lat, double lon) {
        if (!follow_ && !first_) return std::nullopt;
        CameraUpdate up{lat, lon, first_ ? zoom_ : -1.0};
        first_ = false;
        return up;
    }

    // Desktop +/- buttons; clamped, returns the new zoom.
    double adjust_zoom(int delta) {
        zoom_ += delta;
        if (zoom_ < 3.0) zoom_ = 3.0;
        if (zoom_ > 19.0) zoom_ = 19.0;
        return zoom_;
    }
    double zoom() const { return zoom_; }

private:
    bool follow_ = true;
    bool first_ = true;
    double zoom_;
};

} // namespace core
