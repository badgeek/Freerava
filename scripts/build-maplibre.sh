#!/usr/bin/env bash
# Build the MapLibre Native cores cyclomp links against (see docs/maplibre.md).
#
#   ./scripts/build-maplibre.sh [macos|android|all]
#
# Clones maplibre-native into $CYCLOMP_MLN_DIR (default ../deps/maplibre-native,
# a sibling of the repo) if it isn't there, then builds the static cores:
#   build-macos-metal/  desktop (Metal)
#   build-android/      arm64-v8a (OpenGL ES)
# Re-running is incremental. cyclomp's CMakeLists picks the cores up from the
# same default path; override with -DCYCLOMP_MLN_DIR=...
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MLN_DIR="${CYCLOMP_MLN_DIR:-$repo_root/../deps/maplibre-native}"
NDK_VERSION="${NDK_VERSION:-27.1.12297006}"
ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
NDK_TOOLCHAIN="$ANDROID_HOME/ndk/$NDK_VERSION/build/cmake/android.toolchain.cmake"
target="${1:-all}"

# ccache isn't assumed to be installed; the presets reference it, so clear the
# launchers explicitly or configure fails.
NO_CCACHE=(-DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=)

clone() {
    if [ -d "$MLN_DIR/.git" ]; then
        echo "==> maplibre-native already at $MLN_DIR"
        return
    fi
    echo "==> cloning maplibre-native into $MLN_DIR"
    mkdir -p "$(dirname "$MLN_DIR")"
    git clone --recurse-submodules --shallow-submodules \
        https://github.com/maplibre/maplibre-native.git "$MLN_DIR"
}

build_macos() {
    echo "==> building desktop core (build-macos-metal)"
    # mbgl-core needs libuv; GLFW is only for the demo apps.
    export PKG_CONFIG_PATH="/opt/homebrew/opt/libuv/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    cd "$MLN_DIR"
    cmake --preset macos-metal -DMLN_WITH_GLFW=OFF "${NO_CCACHE[@]}"
    ninja -C build-macos-metal mbgl-render
}

build_android() {
    echo "==> building android core (build-android, arm64-v8a)"
    if [ ! -f "$NDK_TOOLCHAIN" ]; then
        echo "NDK toolchain not found: $NDK_TOOLCHAIN" >&2
        echo "Set ANDROID_HOME / NDK_VERSION to match your install." >&2
        exit 1
    fi
    cd "$MLN_DIR"
    cmake -B build-android -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK_TOOLCHAIN" \
        -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-26 \
        -DCMAKE_BUILD_TYPE=Release \
        -DMLN_WITH_OPENGL=ON \
        "${NO_CCACHE[@]}"
    # Only the pieces cyclomp links; the Java SDK targets are skipped.
    ninja -C build-android mbgl-core mbgl-vendor-icu mbgl-vendor-sqlite \
        mbgl-vendor-csscolorparser mbgl-vendor-parsedate
}

clone
case "$target" in
    macos)   build_macos ;;
    android) build_android ;;
    all)     build_macos; build_android ;;
    *)       echo "usage: $0 [macos|android|all]" >&2; exit 2 ;;
esac
echo "==> done: $MLN_DIR"
