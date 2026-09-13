// cyclomp — real compass via the NDK sensor C API (no JNI, no permission).
// A dedicated thread polls the rotation-vector sensor and publishes the
// device azimuth (degrees clockwise from magnetic north).
#pragma once
#ifdef __ANDROID__

namespace compass {

void start();                 // idempotent
bool azimuth_deg(double &out); // false until the first sensor event

} // namespace compass
#endif
