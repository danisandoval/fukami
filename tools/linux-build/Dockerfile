# Linux x86-64 build box for the Steam Deck port (Gate 5).
# Ubuntu 24.04 -> glibc 2.39; SteamOS 3.7+ ships a newer glibc, so binaries
# built here run there. PCSX2's own Linux deps (minus Qt/ffmpeg) are built from
# pinned, SHA-256-checked source by build-deps.sh, same versions as the pinned
# PCSX2 2.8.2 CI script (.github/workflows/scripts/linux/build-dependencies-qt.sh).
FROM --platform=linux/amd64 ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential clang lld llvm cmake ninja-build git python3 python3-venv \
    pkg-config curl ca-certificates patch autoconf automake libtool nasm file patchelf \
    libfreetype-dev libfontconfig-dev libx11-dev libx11-xcb-dev libxrandr-dev libxi-dev \
    libxext-dev libxcursor-dev libxinerama-dev libxss-dev libxfixes-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev \
    libdbus-1-dev libudev-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
    libcurl4-openssl-dev libpcap-dev zlib1g-dev extra-cmake-modules libssl-dev \
    mesa-vulkan-drivers vulkan-tools libvulkan-dev xvfb xauth \
    && rm -rf /var/lib/apt/lists/*
COPY build-deps.sh shaderc-changes.patch /opt/rrv-deps-src/
RUN bash /opt/rrv-deps-src/build-deps.sh /opt/rrv-deps
ENV PKG_CONFIG_PATH=/opt/rrv-deps/lib/pkgconfig \
    CMAKE_PREFIX_PATH=/opt/rrv-deps \
    LD_LIBRARY_PATH=/opt/rrv-deps/lib \
    CC=clang CXX=clang++
