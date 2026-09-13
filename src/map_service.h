// cyclomp — off-screen MapLibre map renderer.
// Runs mbgl (HeadlessFrontend) on a worker thread and hands back CPU RGBA
// frames whenever the camera changes. Compiled when CYCLOMP_HAVE_MAPLIBRE is
// defined (see CMakeLists.txt) — macOS and Android arm64; other platforms
// show a placeholder.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class MapService {
public:
    // Called on the map thread with an opaque premultiplied-RGBA frame.
    using FrameCallback = std::function<void(uint32_t width, uint32_t height,
                                             std::vector<uint8_t> rgba)>;

    MapService(uint32_t width, uint32_t height, FrameCallback on_frame);
    ~MapService();

    // Thread-safe; coalesces bursts. Triggers a re-render on change.
    void set_camera(double lat, double lon, double zoom);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
