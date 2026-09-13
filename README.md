# cyclomp

Bauhaus-styled **bike-computer (cyclo-computer) dashboard** — mock prototype.
One shared Slint UI (`ui/cyclomp.slint`) driven by C++, targeting both **desktop
(instant preview)** and **Android via the native C++ NDK path**.

The dashboard shows:
- **Turn-by-turn nav strip** (geometric arrow + distance to next turn)
- **Big live speed** readout
- **Core ride metrics**: distance, ride time, average speed
- **Cadence (rpm)** and **heart rate (bpm)** with HR zone colour

`src/main.cpp` feeds plausible fake telemetry on a 500 ms timer, so nothing
requires real sensors. Tap **START RIDE** to animate.

```
cyclomp/
├── ui/cyclomp.slint      # shared UI markup
├── src/main.cpp          # mock ride driver (desktop main() + Android slint_main())
├── src/map_service.*     # off-screen MapLibre renderer (MAP tab)
├── scripts/              # build-maplibre.sh — builds the mbgl cores
├── docs/maplibre.md      # the map dependency, in detail
├── CMakeLists.txt        # desktop exe / android shared lib
└── android/              # Gradle wrapper around the CMake project
```

## Desktop preview (works now)

Slint C++ is fetched and built from source, so you need **CMake ≥ 3.21**, a
**C++20 compiler**, and **Rust** (`cargo`/`rustc`).

```bash
cmake -B build
cmake --build build        # first build is slow (Slint + Skia compile once)
./build/cyclomp
```

## Map tab (MapLibre Native, optional)

The MAP tab embeds MapLibre's C++ core (`mbgl`) — no Java SDK — and needs a
locally built checkout in `../deps/maplibre-native`:

```bash
./scripts/build-maplibre.sh        # both platforms; or: macos | android
```

Without it the app still builds; the MAP tab just shows a placeholder. Details,
overrides and the `-DCYCLOMP_MAP_VERBOSE=ON` log switch: [docs/maplibre.md](docs/maplibre.md).

## Android APK (C++ NDK)

Uses the official Slint C++ Android flow: Gradle wraps the same `CMakeLists.txt`,
Slint binds to the NDK through the `android-activity` backend.

Verified running on the `fluxspike` AVD (arm64-v8a, android-35). Prerequisites:
- Android SDK + **NDK** `27.1.12297006` + CMake (SDK Tools).
- Gradle (`brew install gradle` → 9.7.1) or Android Studio.
- **Use the rustup toolchain, not nix.** Put `~/.cargo/bin` first on PATH so
  `rustc` = 1.98 (nix `rustc` 1.84 is too old for `release/1` Slint). Add the
  target matching your device/emulator ABI:
  ```bash
  rustup target add aarch64-linux-android   # arm64 device/emu (Apple Silicon)
  # x86_64-linux-android for an x86_64 emulator (also add to abiFilters)
  ```

Build + install on a running emulator / device:

```bash
export PATH="$HOME/.cargo/bin:$PATH"          # rustup rustc, has android target
export ANDROID_HOME="$HOME/Library/Android/sdk"
export ANDROID_NDK_ROOT="$ANDROID_HOME/ndk/27.1.12297006"
cd android
gradle installDebug
adb shell am start -n dev.bauhouse.cyclomp/android.app.NativeActivity
adb logcat -s slint                           # view logs
```

Boot the emulator first: `$ANDROID_HOME/emulator/emulator -avd fluxspike`.

More ABIs: add to `abiFilters` in `android/build.gradle.kts` and install the
matching Rust targets (`armv7-linux-androideabi`, `x86_64-linux-android`, …).

Ref: [Slint Android guide](https://docs.slint.dev/latest/docs/slint/guide/platforms/mobile/android/) ·
[slint-cpp-template](https://github.com/slint-ui/slint-cpp-template)
