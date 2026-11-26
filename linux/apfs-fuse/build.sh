#!/usr/bin/env bash
# Builds apfs-fuse (read-only APFS via FUSE) for sharing the iMac's macOS Data volume read-only.
# Pinned to the commit that was read before building (docs/hardware-validation.md, Other findings).
# Reviewed: on Linux the device is opened O_RDONLY (ApfsLib/DeviceLinux.cpp); only pread64 and the
# BLKGETSIZE64 ioctl touch it; the FUSE layer registers no write operations (apfsfuse/ApfsFuse.cpp).
set -euo pipefail
COMMIT=66b86bd525e8cb90f9012543be89b1f092b75cf3
SRC=/root/build/src/apfs-fuse
BUILD=/root/build/apfs-fuse
[ -d "$SRC" ] || git clone --recursive https://github.com/sgan81/apfs-fuse.git "$SRC"
git -C "$SRC" fetch -q origin && git -C "$SRC" checkout -q "$COMMIT" && git -C "$SRC" submodule update -q --init
# -include cstdint: upstream misses this include and does not build with GCC 15+ otherwise.
# CMAKE_POLICY_VERSION_MINIMUM: upstream's cmake_minimum_required is older than CMake 4 accepts.
cmake -S "$SRC" -B "$BUILD" -DUSE_FUSE3=ON -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS="-include cstdint"
make -C "$BUILD" -j"$(nproc)" apfs-fuse apfsutil
install -m755 "$BUILD/apfs-fuse" "$BUILD/apfsutil" /usr/local/bin/
