// Front-camera stills via the NDK Camera2 C API — no Java, no preview.
//
// You are on a bike: there is no viewfinder to compose with, so a capture is
// fire-and-forget. `capture()` opens the camera, takes ONE frame, writes the
// JPEG the HAL hands back, and closes everything again. Nothing is held open
// between shots — the Redmi 9 has 2 GB and mbgl's headless renderer is already
// living in it.
//
// CAMERA is a runtime permission and NativeActivity never forwards
// onRequestPermissionsResult, so this follows the same shape as the GPS
// source: ask once, then re-check checkSelfPermission on the next attempt.
#pragma once

#include <functional>
#include <string>

namespace selfie {

// Which way the lens points. Front = the rider (a selfie), Back = the road
// ahead (the view you actually stopped for).
enum class Lens { Front, Back };

// Is a capture worth offering on this lens (the device has one + we can see
// the NDK)? Cheap, cached per lens after the first call.
bool available(Lens lens);

// Permission state, re-checked live (there is no callback to listen on).
bool permission_granted();
// Fires the system dialog once per process. Harmless to call repeatedly.
void request_permission();

struct Shot {
    bool ok = false;
    std::string path;   // where the JPEG landed, empty on failure
    std::string error;  // human-readable reason when !ok
    int width = 0, height = 0;
};

// Take one still on `lens` and write it to `path`. BLOCKS until the frame
// arrives or the timeout expires, so call it off the UI thread (or accept a
// ~300-600 ms hitch). `done` is invoked with the result on the CALLING thread —
// the caller is responsible for hopping back to the Slint loop via
// slint::invoke_from_event_loop before touching any UI.
void capture(const std::string &path, Lens lens, std::function<void(Shot)> done);

} // namespace selfie
