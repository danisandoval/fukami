#!/usr/bin/env bash
# PCSX2 2.8.2's Linux dependency set without Qt/ffmpeg/KDDockWidgets (the GS
# bridge is built with ENABLE_QT_UI=OFF), plus SDL2 2.32.10 for the RRV host.
# Versions and SHA-256 sums copied from PCSX2 fd9d310c
# .github/workflows/scripts/linux/build-dependencies-qt.sh (GPL-3.0+).
set -euo pipefail
INSTALLDIR="$1"; SCRIPTDIR=$(cd "$(dirname "$0")" && pwd)
export CC=clang CXX=clang++
export PKG_CONFIG_PATH="$INSTALLDIR/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
LIBBACKTRACE=ad106d5fdd5d960bd33fae1c48a351af567fd075; LIBJPEGTURBO=3.2.0; LIBPNG=1.6.58
LIBWEBP=1.6.0; SDL=SDL3-3.4.12; LZ4=1.10.0; VULKAN=1.4.328.1; ZSTD=1.5.7
PLUTOVG=1.3.2; PLUTOSVG=0.0.7; RAPIDYAML=0.12.1; SHADERC=2026.2
SHADERC_GLSLANG=275822a6261ee689aadb1da5f09a0ec2f058685c
SHADERC_SPIRVHEADERS=58006c901d1d5c37dece6b6610e9af87fa951375
SHADERC_SPIRVTOOLS=6337eb62cadd7d124ac6789bf39c0f71148f0a73
SDL2=2.32.10
C="-DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$INSTALLDIR -DCMAKE_INSTALL_PREFIX=$INSTALLDIR -DCMAKE_INSTALL_LIBDIR=lib -G Ninja"
mkdir -p /tmp/deps-build && cd /tmp/deps-build
grep . > SHASUMS <<SUMS
96e5c2d7f2c482a60d5804da48a2eb9a0db0719b2c65dcc169fbfdcf37f3a45d  libbacktrace-$LIBBACKTRACE.tar.gz
6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e  libjpeg-turbo-$LIBJPEGTURBO.tar.gz
28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775  libpng-$LIBPNG.tar.xz
e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564  libwebp-$LIBWEBP.tar.gz
f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7  $SDL.tar.gz
eee7dea22ed502868017971c86c63c4ed1e6085de0baebfdcc3d3322f00f3eb0  libpng-$LIBPNG-apng.patch.gz
537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b  lz4-$LZ4.tar.gz
c465aa56757e7746ac707f582b6e2d51546569a4a2488c1172fb543aa5fdfc2c  vulkan-sdk-$VULKAN.tar.gz
eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3  zstd-$ZSTD.tar.gz
7bd4e79ce18b1d47517e7e91fbb7cf19d4f01942804a519bc7c0bf32b6325dd5  plutovg-$PLUTOVG.tar.gz
78561b571ac224030cdc450ca2986b4de915c2ba7616004a6d71a379bffd15f3  plutosvg-$PLUTOSVG.tar.gz
e9efcdd17f86287748793cf21d106e461fcad8d103a3e5a23632afe93828660d  rapidyaml-$RAPIDYAML-src.tgz
f924178e75e3293082481b25ed64d5e48a795b479dac3bd3c83d23070855df42  shaderc-$SHADERC.tar.gz
971848a1cc639ce8dc244e778b17efe0f690e32ac398a75e31d1c67ad06d3e0a  shaderc-glslang-$SHADERC_GLSLANG.tar.gz
a6cb1b300bb8171795e116457e858e555334749f9cacaed8068ae0ef8681110c  shaderc-spirv-headers-$SHADERC_SPIRVHEADERS.tar.gz
e156be0bd81c8812f1bff8e520422bfa9df61b3045587b9eb483185f1074a7b2  shaderc-spirv-tools-$SHADERC_SPIRVTOOLS.tar.gz
SUMS
curl -fsSL \
  -O "https://github.com/ianlancetaylor/libbacktrace/archive/$LIBBACKTRACE/libbacktrace-$LIBBACKTRACE.tar.gz" \
  -O "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LIBJPEGTURBO/libjpeg-turbo-$LIBJPEGTURBO.tar.gz" \
  -O "https://downloads.sourceforge.net/project/libpng/libpng16/$LIBPNG/libpng-$LIBPNG.tar.xz" \
  -O "https://download.sourceforge.net/libpng-apng/libpng-$LIBPNG-apng.patch.gz" \
  -O "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$LIBWEBP.tar.gz" \
  -O "https://github.com/lz4/lz4/releases/download/v$LZ4/lz4-$LZ4.tar.gz" \
  -O "https://libsdl.org/release/$SDL.tar.gz" \
  -O "https://github.com/facebook/zstd/releases/download/v$ZSTD/zstd-$ZSTD.tar.gz" \
  -O "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-$VULKAN.tar.gz" \
  -O "https://github.com/google/shaderc/archive/v$SHADERC/shaderc-$SHADERC.tar.gz" \
  -O "https://github.com/KhronosGroup/glslang/archive/$SHADERC_GLSLANG/shaderc-glslang-$SHADERC_GLSLANG.tar.gz" \
  -O "https://github.com/KhronosGroup/SPIRV-Headers/archive/$SHADERC_SPIRVHEADERS/shaderc-spirv-headers-$SHADERC_SPIRVHEADERS.tar.gz" \
  -O "https://github.com/KhronosGroup/SPIRV-Tools/archive/$SHADERC_SPIRVTOOLS/shaderc-spirv-tools-$SHADERC_SPIRVTOOLS.tar.gz" \
  -O "https://github.com/sammycage/plutovg/archive/v$PLUTOVG/plutovg-$PLUTOVG.tar.gz" \
  -O "https://github.com/sammycage/plutosvg/archive/v$PLUTOSVG/plutosvg-$PLUTOSVG.tar.gz" \
  -O "https://github.com/biojppm/rapidyaml/releases/download/v$RAPIDYAML/rapidyaml-$RAPIDYAML-src.tgz"
sha256sum --check --strict SHASUMS
# SDL2 for the RRV host: the same pinned tag the macOS build uses (config/dependencies.lock.toml).
git clone -q --depth 1 --branch release-$SDL2 https://github.com/libsdl-org/SDL.git SDL2-$SDL2
test "$(git -C SDL2-$SDL2 rev-parse HEAD)" = 5d249570393f7a37e037abf22cd6012a4cc56a71

tar xf "vulkan-sdk-$VULKAN.tar.gz"; cmake -S "Vulkan-Headers-vulkan-sdk-$VULKAN" -B vkh $C; ninja -C vkh install
tar xf "libbacktrace-$LIBBACKTRACE.tar.gz"; (cd "libbacktrace-$LIBBACKTRACE" && ./configure --prefix="$INSTALLDIR" --with-pic && make -j"$(nproc)" && make install)
tar xf "libpng-$LIBPNG.tar.xz"; gzip -kdf "libpng-$LIBPNG-apng.patch.gz"; (cd "libpng-$LIBPNG" && patch -p1 < "../libpng-$LIBPNG-apng.patch")
cmake -S "libpng-$LIBPNG" -B png $C -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF; ninja -C png install
tar xf "libjpeg-turbo-$LIBJPEGTURBO.tar.gz"; cmake -S "libjpeg-turbo-$LIBJPEGTURBO" -B jpg $C -DENABLE_STATIC=OFF -DENABLE_SHARED=ON -DCMAKE_INSTALL_DEFAULT_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=1; ninja -C jpg install
tar xf "lz4-$LZ4.tar.gz"; cmake -S "lz4-$LZ4/build/cmake" -B lz4 $C -DBUILD_SHARED_LIBS=ON -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF; ninja -C lz4 install
tar xf "zstd-$ZSTD.tar.gz"; cmake -S "zstd-$ZSTD/build/cmake" -B zstd $C -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_PROGRAMS=OFF; ninja -C zstd install
tar xf "libwebp-$LIBWEBP.tar.gz"; cmake -S "libwebp-$LIBWEBP" -B webp $C -DBUILD_SHARED_LIBS=ON -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF; ninja -C webp install
tar xf "$SDL.tar.gz"; cmake -S "$SDL" -B sdl3 $C -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_VIDEO=OFF -DSDL_POWER=OFF -DSDL_SENSOR=OFF -DSDL_DIALOG=OFF -DSDL_TRAY=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_UNIX_CONSOLE_BUILD=ON; ninja -C sdl3 install
cmake -S "SDL2-$SDL2" -B sdl2 $C -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_VULKAN=ON -DSDL_PIPEWIRE=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=ON -DSDL_HIDAPI=ON; ninja -C sdl2 install
tar xf "plutovg-$PLUTOVG.tar.gz"; cmake -S "plutovg-$PLUTOVG" -B pvg $C -DBUILD_SHARED_LIBS=ON -DPLUTOVG_BUILD_EXAMPLES=OFF; ninja -C pvg install
tar xf "plutosvg-$PLUTOSVG.tar.gz"; cmake -S "plutosvg-$PLUTOSVG" -B psvg $C -DBUILD_SHARED_LIBS=ON -DPLUTOSVG_ENABLE_FREETYPE=OFF -DPLUTOSVG_BUILD_EXAMPLES=OFF; ninja -C psvg install
tar xf "rapidyaml-$RAPIDYAML-src.tgz"; cmake -S "rapidyaml-$RAPIDYAML-src" -B ryml $C -DBUILD_SHARED_LIBS=ON; ninja -C ryml install
tar xf "shaderc-$SHADERC.tar.gz"
(cd "shaderc-$SHADERC/third_party" && tar xf "../../shaderc-glslang-$SHADERC_GLSLANG.tar.gz" && mv "glslang-$SHADERC_GLSLANG" glslang \
  && tar xf "../../shaderc-spirv-headers-$SHADERC_SPIRVHEADERS.tar.gz" && mv "SPIRV-Headers-$SHADERC_SPIRVHEADERS" spirv-headers \
  && tar xf "../../shaderc-spirv-tools-$SHADERC_SPIRVTOOLS.tar.gz" && mv "SPIRV-Tools-$SHADERC_SPIRVTOOLS" spirv-tools)
(cd "shaderc-$SHADERC" && patch -p1 < "$SCRIPTDIR/shaderc-changes.patch")
cmake -S "shaderc-$SHADERC" -B shaderc $C -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON; ninja -C shaderc install
cd / && rm -rf /tmp/deps-build
