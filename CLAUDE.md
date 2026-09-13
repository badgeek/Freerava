# cyclomp — Bauhaus bike computer (Slint C++, no Java/Kotlin)

One shared Slint UI (`ui/cyclomp.slint`) drives a desktop preview (CMake exe)
and an Android APK (NativeActivity, arm64-v8a). **Hard constraint: zero
Java/Kotlin** — everything Android goes through the NDK or JNI from C++.

## Build (get these wrong and nothing works)

- ALWAYS `export PATH="$HOME/.cargo/bin:$PATH"` first. Slint `release/1`
  needs rustup rustc ≥1.92 (currently 1.98); the nix rustc 1.84 on PATH is
  too old and fails with lint errors deep inside Slint.
- Desktop: `cmake -B build && cmake --build build && ctest --test-dir build`.
  Run app: `CYCLOMP_SCREEN=<0|1|2> ./build/cyclomp` (debug: start screen).
  Don't `cmake --build -j` — the cargo jobserver clashes ("multiple
  --jobserver-fds"). A leftover `build/CMakeCache.txt` keeps the OLD
  `CYCLOMP_MLN_DIR`; pass `-DCYCLOMP_MLN_DIR=...` once or wipe `build/`.
- Android: `export ANDROID_HOME="$HOME/Library/Android/sdk"
  ANDROID_NDK_ROOT="$ANDROID_HOME/ndk/27.1.12297006"`, then
  `cd android && gradle assembleDebug`. Changing Slint cargo features
  requires `rm -rf android/.cxx` (full ~16 min rebuild; incremental ~40 s).
- MapLibre prebuilts live in `../deps/maplibre-native/{build-macos-metal,
  build-android}` — rebuild with `scripts/build-maplibre.sh`
  (see `docs/maplibre.md`). No prebuilts = map silently left out (placeholder).
- Unit tests: doctest, desktop-only, `tests/`. `-DCYCLOMP_BUILD_APP=OFF`
  configures a tests-only lane that never touches Slint/rust.

## Architecture — where a new feature goes

```
src/core/            PURE C++ (no Slint/mbgl/Android includes). Unit-tested.
  ride_engine        state machine (authoritative!) + stats; SessionSummary is
                     TYPED (numbers + time_t); SessionLog newest-first
  telemetry.h        Sample {valid, speed, lat/lon, optional hr/cad/alt/heading}
                     + TelemetrySource — a PULL interface, one sample per tick
  mock_telemetry     the fake ride (desktop + CYCLOMP_MOCK_RIDE=1 on Android)
  follow_camera      camera follow/release/first-fire decisions
  track / session_store / format   recording, persistence, SVG-path charts
  session_db         SQLite store (sqlite3 amalgamation is already linked via
                     mbgl: android libmbgl-vendor-sqlite.a, desktop system
                     -lsqlite3). Finished rides + ONE in-progress ride
                     (active_ride table). Crash-safe txns; migrates the legacy
                     sessions.txt in once. RideEngine::snapshot/restore lets a
                     held ride survive a background kill (save on HOLD + every
                     ~10s + interposed onPause flush; resumed at launch)
src/main.cpp         THIN GLUE ONLY: constructs core objects, wires Slint
                     callbacks, 500 ms slint::Timer = sample→tick→publish.
                     Also the interposed ANativeActivity_onCreate (see below).
src/map_gl.*         Android GPU map service (own thread, headless mbgl,
                     AHardwareBuffer). Talk to it ONLY via its mailbox API.
src/map_service.*    desktop CPU map path (glReadPixels — fine for dev).
src/gps_telemetry_android.*  real GPS TelemetrySource (JNI LocationManager)
src/compass_android.*        rotation-vector compass (NDK sensor C API)
ui/cyclomp.slint     screens: 0 RIDE, 1 MAP, 2 HISTORY, 3 ride detail.
```

Rules that kept this codebase healthy:
- New domain logic → `src/core/` + a test in `tests/`. If it needs Slint or
  Android to compile, it's in the wrong place.
- The Slint `ride-state` property MIRRORS `engine->state()`; never treat the
  UI property as the source of truth.
- Strings are built in main.cpp (`to_row()`, `publish_*`); core stays typed.
  Absent sensor channels are `optional` → UI shows "--".
- Sensors: implement `TelemetrySource` (pull; a push source becomes a
  latest-fix mailbox whose `sample()` returns `valid=false` until data).
- Map camera commands (`set_camera/drag_by/scale_by/set_bearing/set_track/
  set_marker`) are coalesced under the service mutex and applied on the map
  thread before each render. `set_camera` zoom<=0 means "keep zoom".
- UI callbacks fired from non-UI threads MUST hop via
  `slint::invoke_from_event_loop`; touching `window()` off-thread aborts
  (`assert_main_thread`).

## Hard-learned lessons (each cost hours — do not rediscover)

**mbgl / MapLibre (no Java SDK — mbgl core embedded directly):**
- `mln::android::theJVM` MUST be assigned in our interposed
  `ANativeActivity_onCreate` (main.cpp). Unset → mbgl worker threads fail to
  attach → thread-spawn storm → 1.4 GB RSS → lowmemorykiller. Looks like a
  leak; it's a missing global.
- mbgl core ships NO HTTP transport for pure-NDK apps:
  `src/http_file_source_android.cpp` (JNI HttpURLConnection) provides it.
- Custom `RendererFrontend`/`RendererBackend` + `Map` = deterministic
  ResourceLoader crash on TWO platforms (even with zero GL). Only the
  `HeadlessFrontend` stack is stable — that's why the GPU path is
  "headless render → glCopyTexSubImage2D → AHardwareBuffer → EGLImage in
  Slint's context → borrowed-GL-texture Image" (no shared EGL context
  needed). History: `.claude/worktrees/spike-mapgl-c/HANDOVER.md`.
- Never touch `getDefaultRenderable()` outside a `gfx::BackendScope`: lazy
  FBO creation throws "Couldn't create framebuffer: other".
- Anything subclassing mbgl polymorphic types needs `-fno-rtti` (core is
  built without RTTI → undefined typeinfo at link).
- `renderStill` waits for EVERY tile of the view — a zoom-level change swaps
  the whole tile set. That's why per-update pinch commits stutter; the pinch
  is a Slint-side visual preview committed once on gesture end, synced by a
  command-generation counter in the frame sink.
- GL underlay in Slint's own context is a dead end: Skia (the Android
  renderer — `SLINT_FEATURE_RENDERER_*` only steers winit renderers!) caches
  GL state and Slint has no resetContext hook.

**Android platform:**
- The emulator lies: `hasSpeed()`/`hasBearing()` true with a hard 0 while
  moving — treat 0 as "unknown" and compute from movement (haversine).
- `getLastKnownLocation` alone leaves GNSS ASLEEP. Keep a standing
  `requestLocationUpdates` alive — via a mutable PendingIntent broadcast
  nobody receives (the only listener overload that needs no Java class).
- NativeActivity never forwards `onRequestPermissionsResult` — re-check
  `checkSelfPermission` on every poll instead of waiting for a callback.
- The interposed `ANativeActivity_onCreate` works because the framework
  dlsym's OUR lib before libslint_cpp.so; store `NewGlobalRef(clazz)` there
  (`cyclomp_activity()`), then forward to Slint's real entry point.
- Sensors need NO permission and NO Java (`ASensorManager` C API).
- Goldfish-emulator GL renderer crashes are ENDEMIC (5 distinct kinds this
  project: calcIndexRange null, DrawableGL under bearing streams, ...).
  The Mali phone never reproduced one. Suspect the emulator first; the
  real device is the referee. `adb emu geo fix lon lat [alt]` feeds GPS;
  `adb emu sensor set magnetic-field x:y:z` steers the compass.
- MIUI: first `adb install` needs "Install via USB" + an on-phone confirm
  (INSTALL_FAILED_USER_RESTRICTED = the human didn't tap Allow in time).
- Env vars can't be injected into an app from adb (zygote); `wrap.` props
  are blocked by SELinux. Debug toggles must be reachable another way.

**General:**
- Angle deltas: normalise with fmod(...,360) BEFORE the >180 wrap step — a
  sentinel like -999 otherwise produces a negative delta that fails every
  threshold forever (the heading-up "does nothing" bug).
- Slint: element `parent.parent.x` doesn't resolve custom properties — give
  the owning element an id. `"▲"` renders as tofu in the bundled font —
  prefer ASCII in UI strings. `clip: true` is OFF by default on Rectangles.
  Gesture pinch = `ScaleRotateGestureHandler` (scale is CUMULATIVE; `ended`
  fires after `scale` resets — cache it in a property).
- Slint: NEVER change `screen` (or any prop that removes an `if`/`ListView`
  subtree) synchronously from inside a callback fired by an element in THAT
  subtree — the tap destroys the element mid-dispatch, then Slint walks the
  orphaned item's generated `parent_node` (`self->parent.lock().value()`),
  the parent weak is empty, and it throws `std::bad_optional_access`. Was a
  device-ONLY crash (emulator event ordering hid it): tapping a SORTIE LOG
  row ran `select-session` -> `set_screen(3)` -> killed the history ListView
  under its own click. Fix: defer the switch a turn with
  `slint::invoke_from_event_loop([ui]{ ...set_screen(3); })` so the click
  finishes with the list alive (`on_select_session` in main.cpp).
- `float`-accumulate elapsed time; `lround(0.5)` per 500 ms tick once made
  the clock run 2×. There's a regression test.
- grep-ing build output for "error" false-positives on `error_sink.cpp.o`.

## Debugging toolbox (proven on this project)

- **Native crash triage**: `adb logcat -d -b crash` (aborts land there, e.g.
  an uncaught `std::bad_optional_access`). Symbolize the `#01 pc <offset>`
  frames against the UNSTRIPPED lib:
  `$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-addr2line -Cfe
  android/build/intermediates/cxx/Debug/*/obj/arm64-v8a/libcyclomp.so <pc>`.
  FIRST verify the Build ID matches (`llvm-readelf -n` vs the crash dump) —
  every gradle build overwrites the lib, so symbolize crashes IMMEDIATELY
  or the addresses become garbage against the wrong binary. CAVEAT: MIUI 10
  on the Redmi truncates the logcat tombstone at `#01 abort_message` — the
  real throwing frame never prints, and `/data/tombstones` is root-only. For
  that, `main.cpp` installs a `std::set_terminate` handler (Android only)
  that walks `_Unwind_Backtrace` and logs library-relative offsets under tag
  `cyclomp-crash` (`adb logcat -s cyclomp-crash:F`); feed those offsets
  straight to `llvm-addr2line -Cfe <unstripped .so>` (they're already the
  base-relative addresses addr2line wants). This is how the log-detail
  `bad_optional_access` was finally located in Slint-generated `parent_node`.
- **App files without root**: `adb shell run-as dev.bauhouse.cyclomp cat
  files/settings.txt` (also sessions.txt). WRITING settings this way is the
  practical replacement for env vars (zygote blocks them): e.g. force the
  mock ride on the emulator with
  `run-as dev.bauhouse.cyclomp sh -c 'printf "mock_ride=1\n" > files/settings.txt'`.
- **Log tags**: `cyclomp-mapgl` (map service lifecycle, texture imports,
  bearing), `cyclomp-compass`; `cyclomp-map`/`cyclomp-http` only with
  `-DCYCLOMP_MAP_VERBOSE=ON`. Filter: `adb logcat -s cyclomp-mapgl:I`.
- **CPU sampling**: `adb shell "top -b -n 5 -d 2 -o PID,%CPU,RSS,ARGS |
  grep cyclomp"`. Baseline: idle dashboard ~1-10%; the tunnel animation at
  30fps adds ~55-60% (full-screen Skia redraw) — TUNNEL FPS / TUNNEL
  ANIMATION in SYSTEM PARAMETERS exist to tune that. `dumpsys batterystats`
  per-uid mAh stays empty while the phone is on the charger.
- **Driving the UI over adb**: coordinates for `input tap` come from a
  fresh `exec-out screencap -p` — the user may have navigated the app in
  the meantime, so re-screenshot before every scripted tap sequence.

## Verification routine (what "done" means here)

1. `ctest --test-dir build` green (5k+ assertions).
2. `gradle assembleDebug` green.
3. Emulator smoke: install, `am start -n
   dev.bauhouse.cyclomp/android.app.NativeActivity`, drive via
   `adb shell input tap` (MAP tab ≈ 540 2232 @1080x2400), screenshot with
   `adb exec-out screencap -p` and LOOK at it.
4. Real device d2f9dc4a0404 (Xiaomi Redmi 9, 1080x2340, MAP tab tap y≈2172):
   the final referee, especially for anything GL or sensor related.
5. Commit per feature with a trailer; do NOT push (no remote configured).
