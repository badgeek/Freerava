# MapLibre dependency

The MAP tab renders with **MapLibre Native's C++ core (`mbgl`) embedded
directly** — no MapLibre Java SDK, so the Android app stays pure NDK
(`android:hasCode="false"`, no Java/Kotlin sources).

MapLibre publishes no prebuilt static core for this use, so cyclomp links
against a local checkout you build once:

```
deps/maplibre-native/          # sibling of the cyclomp repo
├── build-macos-metal/         # desktop core (Metal)
└── build-android/             # arm64-v8a core (OpenGL ES)
```

`CMakeLists.txt` finds it via the `CYCLOMP_MLN_DIR` cache variable, which
defaults to `<repo>/../deps/maplibre-native`. Point it elsewhere with
`cmake -B build -DCYCLOMP_MLN_DIR=/path/to/maplibre-native`. **The map is
optional**: when the directory (or the built `libmbgl-core.a`) is missing,
CMake silently drops `map_service.cpp` and the MAP tab shows a placeholder —
the rest of the app builds and runs as before.

## Building the cores

```bash
./scripts/build-maplibre.sh            # both; or: macos | android
```

The script clones maplibre-native (with submodules) if needed, then configures
and builds. First build is long (~500 translation units per platform);
re-running is incremental. Notes baked into the script:

- `-DMLN_WITH_GLFW=OFF` — GLFW is only for MapLibre's own demo apps.
- `-DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=` — the upstream
  preset assumes ccache; clearing the launchers avoids a configure failure when
  it isn't installed.
- Desktop needs libuv on `PKG_CONFIG_PATH`
  (`/opt/homebrew/opt/libuv/lib/pkgconfig`).
- Android builds against NDK `27.1.12297006`, `android-26`, arm64-v8a,
  `-DMLN_WITH_OPENGL=ON`. Override with `ANDROID_HOME` / `NDK_VERSION`.

**Do not put the checkout in `/tmp`.** CMake build directories bake in absolute
paths, so a move invalidates both build trees (recoverable, but every object
recompiles).

## How it is wired up

| Piece | What it does |
| --- | --- |
| `src/map_service.{h,cpp}` | Worker thread owning a `RunLoop` + `HeadlessFrontend`; renders CPU RGBA frames on camera change |
| `src/main.cpp` | Feeds the camera from the rider position; pushes frames to the UI thread as a Slint `Image` |
| `src/http_file_source_android.cpp` | `mln::HTTPFileSource` over `java.net.HttpURLConnection` via JNI — mbgl-core ships no HTTP transport for a pure-NDK app |
| `src/android_env.h` | The `JavaVM` captured by the interposed `ANativeActivity_onCreate` |

Android also compiles three files straight out of the maplibre checkout
(collator / number-format stubs and the default glyph rasterizer) because the
Java SDK normally supplies those.

Tiles and style come from [OpenFreeMap](https://openfreemap.org) (liberty
style); the Android manifest therefore needs `android.permission.INTERNET`.

## Debug logging

Per-frame (`cyclomp-map`) and per-request (`cyclomp-http`) logs are compiled
out by default. Turn them on with:

```bash
cmake -B build -DCYCLOMP_MAP_VERBOSE=ON
```

mbgl's own error channel (`mln::Log::platformRecord` → logcat tag `maplibre`)
is always on.
