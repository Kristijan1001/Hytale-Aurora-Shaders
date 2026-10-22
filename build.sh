#!/usr/bin/env bash
# Native Ubuntu/Linux build. Dependencies: build-essential pkg-config libcairo2-dev
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ $(uname -s) == Darwin ]]; then
    # Apple Silicon: an SDL3 proxy that re-exports the game's original library. Dependency: brew install cairo
    if ! xcode-select -p >/dev/null 2>&1; then echo 'Install the compiler first: xcode-select --install' >&2; exit 1; fi
    if ! command -v pkg-config >/dev/null || ! pkg-config --exists cairo; then echo 'Install build dependencies: brew install cairo pkg-config' >&2; exit 1; fi
    mkdir -p "$root/build"
    stage=$(mktemp -d "$root/build/.aurora-build.XXXXXX")
    trap 'rm -rf -- "$stage"' EXIT
    read -r -a cairo_cflags <<< "$(pkg-config --cflags cairo)"
    # Link-time placeholder for the original SDL3, as on Linux; no game code is bundled.
    cc -arch arm64 -dynamiclib -x c /dev/null -install_name @loader_path/libSDL3.aurora-original.dylib -o "$stage/sdl-link.dylib"
    cc -arch arm64 -std=gnu11 -O2 -dynamiclib -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
        -Wno-missing-field-initializers -DAU_SDL_PROXY=1 -DAU_HAS_DLSS=0 "${cairo_cflags[@]}" \
        -I"$(pkg-config --variable=includedir cairo)" "$root/hook/hook.c" \
        -install_name @rpath/libSDL3.dylib -Wl,-reexport_library,"$stage/sdl-link.dylib" -L"$(pkg-config --variable=libdir cairo)" -Wl,-weak-lcairo \
        -o "$stage/libaurora-sdl.dylib"
    mv -f -- "$stage/libaurora-sdl.dylib" "$root/build/"
    echo "Built $root/build/libaurora-sdl.dylib"
    exit 0
fi
if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
    echo 'The native build currently targets x86_64 Linux (glibc).' >&2
    exit 1
fi
if ! command -v "${CC:-cc}" >/dev/null || ! command -v pkg-config >/dev/null || ! pkg-config --exists cairo; then
    echo 'Install build dependencies: sudo apt install build-essential pkg-config libcairo2-dev fonts-dejavu-core python3 python3-pil' >&2
    exit 1
fi
mkdir -p "$root/build"
stage=$(mktemp -d "$root/build/.aurora-build.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
read -r -a cairo_cflags <<< "$(pkg-config --cflags cairo)"
read -r -a cairo_libs <<< "$(pkg-config --libs cairo)"
common=(-std=gnu11 -O2 -fPIC -shared -Wl,-z,defs -Wl,-Bsymbolic-functions
    -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-missing-field-initializers)
dlss=()
sdk=${DLSS_SDK_DIR:-"$root/third_party/DLSS"}
vulkan=${VULKAN_HEADERS_DIR:-"$root/third_party/vulkan"}
if [[ ${AU_DLSS:-auto} != 0 && -f "$sdk/include/nvsdk_ngx_defs.h" && -f "$vulkan/include/vulkan/vulkan.h" ]]; then
    dlss=(-DAU_HAS_DLSS=1 "-I$sdk/include" "-I$vulkan/include" "$root/hook/aud_dlss.c")
    echo 'Building with native Linux DLSS support (NVIDIA RTX runtime required).'
elif [[ ${AU_DLSS:-auto} == 1 ]]; then
    echo 'DLSS SDK/Vulkan headers missing. Run python3 tools/prepare_dlss_linux.py first.' >&2
    exit 1
else
    dlss=(-DAU_HAS_DLSS=0)
    echo 'Building without DLSS. Run python3 tools/prepare_dlss_linux.py to enable it.'
fi
"${CC:-cc}" "${common[@]}" "${cairo_cflags[@]}" "$root/hook/hook.c" "${dlss[@]}" \
    -o "$stage/libaurora.so" "${cairo_libs[@]}" -ldl -lpthread -lm
# A link-time placeholder supplies the dependency name without bundling game code.
# At runtime this dependency is the player's unchanged original SDL3 library.
"${CC:-cc}" -shared -fPIC -x c /dev/null \
    -Wl,-soname,libSDL3.aurora-original.so -o "$stage/sdl-link.so"
"${CC:-cc}" "${common[@]}" -DAU_SDL_PROXY=1 "${cairo_cflags[@]}" "$root/hook/hook.c" "${dlss[@]}" \
    -o "$stage/libaurora-sdl.so" -Wl,-soname,libSDL3.so '-Wl,-rpath,$ORIGIN' \
    "-Wl,--version-script=$root/hook/sdl_proxy.map" \
    -Wl,--no-as-needed "$stage/sdl-link.so" -Wl,--as-needed \
    "${cairo_libs[@]}" -ldl -lpthread -lm
# Replace outputs atomically, including if an older library is still mapped by a process.
mv -f -- "$stage/libaurora.so" "$stage/libaurora-sdl.so" "$root/build/"
echo "Built $root/build/libaurora.so and libaurora-sdl.so"
