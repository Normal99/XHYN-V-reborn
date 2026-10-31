# SPDX-License-Identifier: MIT
# Copyright (c) 2026 XHYN-PH
#
# Android (produces libgtav_bcn.so, the replacement for the BCn layer slot):
#   cmake -S . -B build/android -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static
#   cmake --build build/android
# Host tests (software Vulkan driver + validation layer): see test/run_host_tests.sh
cmake_minimum_required(VERSION 3.16)
project(xhyn_upscaler CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

option(XHYN_BUILD_TESTS "Build the host test harness (not for Android)" OFF)

add_library(xhyn_shim SHARED
  src/shim.cpp src/layer.cpp src/sgsr.cpp src/framegen.cpp src/config.cpp src/log.cpp)
target_compile_options(xhyn_shim PRIVATE -Wall -Wextra -Wno-missing-field-initializers -fno-exceptions -fno-rtti)
target_include_directories(xhyn_shim PRIVATE src)

if(ANDROID)
  set_target_properties(xhyn_shim PROPERTIES OUTPUT_NAME gtav_bcn)
  # The loader looks the library up by this name, and the APK keeps native libraries
  # uncompressed and 16 KiB aligned.
  target_link_options(xhyn_shim PRIVATE -Wl,-soname,libgtav_bcn.so -Wl,-z,max-page-size=16384
    -Wl,--build-id=sha1 -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,defs)
  target_link_libraries(xhyn_shim PRIVATE log dl)
else()
  set_target_properties(xhyn_shim PROPERTIES OUTPUT_NAME xhyn_shim)
  target_link_libraries(xhyn_shim PRIVATE dl)
endif()

if(XHYN_BUILD_TESTS AND NOT ANDROID)
  add_library(xhyn_inner_stub SHARED test/inner_stub.cpp)
  set_target_properties(xhyn_inner_stub PROPERTIES OUTPUT_NAME xhyn_bcn)
  target_link_libraries(xhyn_inner_stub PRIVATE dl)
  add_executable(xhyn_harness test/harness.cpp)
  target_compile_options(xhyn_harness PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
  target_link_libraries(xhyn_harness PRIVATE dl)
endif()
