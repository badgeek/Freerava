// cyclomp — B-lite GPU map path.
// mbgl runs the PROVEN CPU-path stack (HeadlessFrontend, its own internal EGL
// context, Static mode, renderStill) — but instead of glReadPixels the frame
// is GPU-blitted into an AHardwareBuffer-backed texture. The same
// AHardwareBuffer is imported (via EGLImage) into Slint's GL context and shown
// as a borrowed-GL-texture Image. Pixels never touch the CPU. Android only.
#pragma once

#include <cstdint>
#include <functional>

namespace mapgl {

// Start the map service. No GL required on the calling thread.
void setup(uint32_t width_px, uint32_t height_px, float pixel_ratio);

// Called from the MAP THREAD when a frame is finished (buffer fully blitted).
// idx selects the AHardwareBuffer; hop to the UI thread before touching Slint.
void set_frame_sink(std::function<void(uint32_t idx, uint32_t width,
                                       uint32_t height)> sink);

// Import the shared buffers into the current GL context. Call ONLY from the
// rendering notifier (Slint's GL context current). Returns true once done.
bool ui_import_all();

// Texture id (in Slint's context) for buffer idx; valid after ui_import_all.
uint32_t ui_texture(uint32_t idx);

// Queue a camera move; safe from any thread. zoom <= 0 keeps the current
// zoom level (used by follow-mode so pinch zoom isn't overridden).
void set_camera(double lat, double lon, double zoom);

// Where the rider is. mbgl's Map belongs to the map thread, so the
// lat/lon -> screen projection happens there, right after each render.
void set_marker(double lat, double lon);

// Latest projection of the marker: offsets from the frame centre as a
// FRACTION of the frame's width/height, plus the frame's aspect ratio (the
// UI needs it to undo `image-fit: cover`). False until a marker has been
// set and a frame carrying it has rendered.
bool marker_offset(double &nx, double &ny, double &aspect);

// One-finger pan: shift the map by a screen delta (logical px).
void drag_by(double dx, double dy);

// Pinch: multiply the scale by `factor` around `anchor` (logical px).
void scale_by(double factor, double ax, double ay);

// +/- buttons: one zoom level in/out around the map centre.
void zoom_step(int delta);

// Re-render with the current camera (used when a frame had to be dropped
// because the UI textures weren't imported yet).
void poke();

} // namespace mapgl
