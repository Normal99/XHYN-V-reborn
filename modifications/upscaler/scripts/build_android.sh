#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 XHYN-PH
#
# Builds libgtav_bcn.so (the shim) for arm64-v8a with the Android NDK and checks it.
# Needs: ANDROID_NDK_HOME (NDK r26 or newer), cmake, ninja, python3.
# Output: build/android/libgtav_bcn.so
set -euo pipefail
cd "$(dirname "$0")/.."
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
  echo "Set ANDROID_NDK_HOME to an Android NDK directory (r26 or newer)." >&2
  exit 1
fi
cmake -S . -B build/android -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static
cmake --build build/android
LIB=build/android/libgtav_bcn.so
STRIP=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -n 1)
"$STRIP" --strip-unneeded "$LIB"
python3 scripts/install_shim.py --shim "$LIB" --check-only
ls -l "$LIB"
