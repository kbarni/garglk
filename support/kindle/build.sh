#!/bin/sh
# Builds Gargoyle for Kindle firmware 5.16.3+ (kindlehf) into build/dist.
# Requires koxtoolchain's kindlehf toolchain with kindle-sdk installed on top of it.

set -e

TOP=$(CDPATH='' cd "$(dirname "$0")/../.." && pwd -P)
BUILD="${BUILD_DIR:-${TOP}/build/kindlehf}"
DEPS="${BUILD}/deps"
TOOLCHAIN="${TOP}/support/kindle/kindlehf.cmake"
JPEG_VERSION=3.2.0
# The firmware ships FreeType and fontconfig without headers, and kindle-sdk
# provides those of newer versions, so the ones of the firmware versions are used.
FREETYPE_VERSION=2.9.1
FONTCONFIG_VERSION=2.8.0

# The firmware libstdc++ (GCC 10) is older than the toolchain one.
STATIC_RUNTIME_FLAGS="-static-libstdc++ -static-libgcc"

mkdir -p "${BUILD}/src"

# The firmware ships libjpeg without headers and its soname is not stable across
# releases, so a static PIC libjpeg-turbo is linked into libgarglk instead.
if [ ! -f "${DEPS}/lib/libjpeg.a" ]; then
    curl -fsSL "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/${JPEG_VERSION}/libjpeg-turbo-${JPEG_VERSION}.tar.gz" |
        tar -xz -C "${BUILD}/src"
    cmake -S "${BUILD}/src/libjpeg-turbo-${JPEG_VERSION}" -B "${BUILD}/jpeg" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${DEPS}" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DENABLE_SHARED=OFF \
        -DWITH_JPEG8=ON \
        -DWITH_TURBOJPEG=OFF \
        -DWITH_TOOLS=OFF \
        -DWITH_TESTS=OFF
    cmake --build "${BUILD}/jpeg"
    cmake --install "${BUILD}/jpeg"
fi

if [ ! -f "${DEPS}/include/freetype2/ft2build.h" ]; then
    curl -fsSL "https://downloads.sourceforge.net/project/freetype/freetype2/${FREETYPE_VERSION}/freetype-${FREETYPE_VERSION}.tar.gz" |
        tar -xz -C "${BUILD}/src"
    mkdir -p "${DEPS}/include"
    cp -R "${BUILD}/src/freetype-${FREETYPE_VERSION}/include" "${DEPS}/include/freetype2"
fi

if [ ! -f "${DEPS}/include/fontconfig/fontconfig.h" ]; then
    curl -fsSL "https://www.freedesktop.org/software/fontconfig/release/fontconfig-${FONTCONFIG_VERSION}.tar.gz" |
        tar -xz -C "${BUILD}/src"
    mkdir -p "${DEPS}/include/fontconfig"
    cp "${BUILD}/src/fontconfig-${FONTCONFIG_VERSION}/fontconfig/"*.h "${DEPS}/include/fontconfig/"
fi

cmake -S "${TOP}" -B "${BUILD}/garglk" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
    -DKINDLE_DEPS_PREFIX="${DEPS}" \
    -DCMAKE_C_FLAGS="-I${DEPS}/include" \
    -DCMAKE_CXX_FLAGS="-I${DEPS}/include" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--as-needed ${STATIC_RUNTIME_FLAGS}" \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--as-needed ${STATIC_RUNTIME_FLAGS}" \
    -DCMAKE_INSTALL_RPATH='$ORIGIN' \
    -DFREETYPE_INCLUDE_DIR_ft2build="${DEPS}/include/freetype2" \
    -DFREETYPE_INCLUDE_DIR_freetype2="${DEPS}/include/freetype2" \
    -DJPEGLIB=IJG \
    -DJPEG_INCLUDE_DIR="${DEPS}/include" \
    -DJPEG_LIBRARY="${DEPS}/lib/libjpeg.a" \
    -DCMAKE_BUILD_TYPE=Release \
    -DINTERFACE=KINDLE \
    -DDIST_INSTALL=ON \
    -DWITH_BUNDLED_FMT=ON
cmake --build "${BUILD}/garglk"
cmake --install "${BUILD}/garglk"

# The game downloader of the game list.
if command -v go >/dev/null 2>&1 && [ -f "${TOP}/ifdb-dl/main.go" ]; then
    (cd "${TOP}/ifdb-dl" && CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 \
        go build -trimpath -ldflags="-s -w" -o "${TOP}/build/dist/ifdb-dl" .)
fi
