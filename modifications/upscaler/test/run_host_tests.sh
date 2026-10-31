#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 XHYN-PH
#
# Host tests for the upscaler layer on a software Vulkan driver (lavapipe) with the
# Khronos validation layer. They cover the layer protocol, swapchain virtualisation,
# colour handling for all four 8-bit formats and cleanup. They do not cover Android,
# Adreno or the real game.
#
# Needs: cmake, ninja, a C++17 compiler, glslang-tools, libvulkan-dev, vulkan-validationlayers,
# mesa-vulkan-drivers, python3 with Pillow.
set -u
cd "$(dirname "$0")/.."
BUILD=${BUILD_DIR:-/tmp/xhyn-upscaler-build}
OUT=$BUILD/out
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}
# Mesa's device-select implicit layer crashes in the stub path on hosts with a real GPU.
export NODEVICE_SELECT=1
mkdir -p "$OUT"
pass=0; fail=0

step() { printf '\n== %s\n' "$1"; }
ok()   { pass=$((pass+1)); printf 'PASS %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf 'FAIL %s\n' "$1"; }

step "build"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DXHYN_BUILD_TESTS=ON >/dev/null && cmake --build "$BUILD" >/dev/null || { echo "build failed"; exit 1; }
SHIM=$BUILD/libxhyn_shim.so
STUB=$BUILD/libxhyn_bcn.so
HARNESS=$BUILD/xhyn_harness

step "generated shaders are up to date"
cp src/shaders_spv.h "$OUT/shaders_spv.committed.h"
python3 scripts/gen_shaders.py >/dev/null && cmp -s src/shaders_spv.h "$OUT/shaders_spv.committed.h" && ok "shaders_spv.h matches shaders/" || bad "shaders_spv.h is stale (run scripts/gen_shaders.py)"

# run NAME ENV... -- HARNESS_ARGS...
run() {
  local name=$1; shift
  local envs=()
  while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
  rm -f "$OUT"/*.ppm "$OUT"/*.png
  if env "${envs[@]}" XHYN_UPSCALE_DUMP_DIR="$OUT" XHYN_UPSCALE_LOG_INTERVAL=0 "$HARNESS" --shim "$SHIM" --validate "$@" >"$OUT/$name.log" 2>&1; then
    ok "$name"
  else
    bad "$name"; sed -n '1,25p' "$OUT/$name.log" | cut -c1-300
  fi
}
expect_log() { grep -q "$2" "$OUT/$1.log" && ok "$1: log has '$2'" || { bad "$1: log lacks '$2'"; }; }
check() { python3 -W ignore test/check_dump.py "$OUT" "$@" >"$OUT/check.log" 2>&1 && { ok "dumped $2 frame is correct"; sed 's/^/     /' "$OUT/check.log"; } || { bad "dumped $2 frame is wrong"; sed 's/^/     /' "$OUT/check.log"; }; }
INNER="XHYN_UPSCALE_INNER=$STUB"
VIA_INNER="XHYN_UPSCALE_BCN=inner"   # force the path through the original-layer stub
FMT01273=(--ignore VUID-VkSwapchainCreateInfoKHR-imageFormat-01273)  # lavapipe headless lists only UNORM formats

step "BC decoding policy (original BCn layer)"
run bcn_auto_hardware "$INNER" XHYN_UPSCALE_MODE=sgsr1 XHYN_UPSCALE_DUMP_FRAME=3 -- --pattern flat --diag-bypassed
expect_log bcn_auto_hardware "hardware BC, original BCn layer bypassed"
grep -q "STUB:" "$OUT/bcn_auto_hardware.log" && bad "bcn_auto_hardware: original layer was used" || ok "bcn_auto_hardware: original layer not in the call chain"
check 3 flat 640 360 960 540
run bcn_auto_no_hardware "$INNER" XHYN_UPSCALE_ASSUME_NO_HARDWARE_BC=1 XHYN_UPSCALE_MODE=sgsr1 XHYN_UPSCALE_DUMP_FRAME=3 -- --pattern flat
expect_log bcn_auto_no_hardware "a GPU lacks native BC"
[ "$(grep -c 'STUB: vkCreateInstance' "$OUT/bcn_auto_no_hardware.log")" = 1 ] && ok "bcn_auto_no_hardware: instance recreated through the original layer once" || bad "bcn_auto_no_hardware: wrong number of original-layer instance creations"
[ "$(grep -c 'STUB: vkCreateDevice' "$OUT/bcn_auto_no_hardware.log")" = 1 ] && ok "bcn_auto_no_hardware: device created through the original layer" || bad "bcn_auto_no_hardware: device not created through the original layer"
check 3 flat 640 360 960 540
run bcn_native "$INNER" XHYN_UPSCALE_BCN=native XHYN_UPSCALE_ASSUME_NO_HARDWARE_BC=1 XHYN_UPSCALE_MODE=probe --
grep -q "STUB:" "$OUT/bcn_native.log" && bad "bcn_native: original layer was used" || ok "bcn_native: policy overrides the hardware check"

step "pass-through (off) and probe modes"
run off     "$INNER" "$VIA_INNER" XHYN_UPSCALE_MODE=off   -- --diag
expect_log off "STUB: vkCreateInstance forwarded"
run probe   "$INNER" "$VIA_INNER" XHYN_UPSCALE_MODE=probe -- --diag --recreate
expect_log probe "swapchain create: 640x360"
expect_log probe "surface formats"

step "SGSR 1: colour preservation, all formats"
for combo in "bgra:" "rgba:--rgba"; do
  name=${combo%%:*}; flag=${combo#*:}
  run "sgsr1_flat_${name}_unorm" "$INNER" "$VIA_INNER" XHYN_UPSCALE_MODE=sgsr1 XHYN_UPSCALE_DUMP_FRAME=3 -- --pattern flat $flag
  expect_log "sgsr1_flat_${name}_unorm" "sgsr1 ready: 640x360 -> 960x540"
  check 3 flat 640 360 960 540
  run "sgsr1_flat_${name}_srgb" "$INNER" "$VIA_INNER" XHYN_UPSCALE_MODE=sgsr1 XHYN_UPSCALE_DUMP_FRAME=3 -- --pattern flat --srgb --mutable $flag "${FMT01273[@]}"
  expect_log "sgsr1_flat_${name}_srgb" "sRGB output encode"
  check 3 flat 640 360 960 540
done

step "SGSR 1: edges, options, resize, explicit size"
run sgsr1_edges "$INNER" "$VIA_INNER" XHYN_UPSCALE_MODE=sgsr1 XHYN_UPSCALE_DUMP_FRAME=3 -- --pattern edges
check 3 edges 640 360 960 540
run sgsr1_edge_direction "$INNER" "$VIA_INNER" XHYN_UPSCALE_MODE=sgsr1 XHYN_UPSCALE_EDGE_DIRECTION=1 XHYN_UPSCALE_EDGE_SHARPNESS=1.5 XHYN_UPSCALE_DUMP_FRAME=3 -- --pattern edges
check 3 edges 640 360 960 540
run sgsr1_recreate "$INNER" "$VIA_INNER" XHYN_UPSCALE_MODE=sgsr1 -- --pattern edges --recreate
[ "$(grep -c 'sgsr1 ready' "$OUT/sgsr1_recreate.log")" = 2 ] && ok "resize recreated the upscaler" || bad "resize did not recreate the upscaler"
run sgsr1_explicit_size "$INNER" "$VIA_INNER" XHYN_UPSCALE_MODE=sgsr1 XHYN_UPSCALE_OUT_WIDTH=1280 XHYN_UPSCALE_OUT_HEIGHT=720 XHYN_UPSCALE_DUMP_FRAME=3 -- --pattern edges
check 3 edges 640 360 1280 720

step "SGSR 1: setup failing after the real swapchain exists, on a resize"
# The enlarged swapchain must be replaced by one of the game's size using it as oldSwapchain;
# the validation layer flags reuse of a retired oldSwapchain.
run sgsr1_late_failure "$INNER" "$VIA_INNER" XHYN_UPSCALE_MODE=sgsr1 XHYN_UPSCALE_TEST_FAIL_ATTACH=1 -- --pattern edges --recreate
expect_log sgsr1_late_failure "sgsr1 setup failed: test hook"
[ "$(grep -c 'sgsr1 ready' "$OUT/sgsr1_late_failure.log")" = 0 ] && ok "sgsr1_late_failure: game ran unmodified at its own size" || bad "sgsr1_late_failure: upscaler unexpectedly active"

step "SGSR 1: cases where it must stay out of the way"
run sgsr1_scale1 "$INNER" "$VIA_INNER" XHYN_UPSCALE_MODE=sgsr1 XHYN_UPSCALE_SCALE=1.0 -- --pattern flat
expect_log sgsr1_scale1 "not applied"
run sgsr1_without_original_layer XHYN_UPSCALE_INNER=/nonexistent/libxhyn_bcn.so XHYN_UPSCALE_MODE=sgsr1 XHYN_UPSCALE_DUMP_FRAME=3 -- --pattern flat
expect_log sgsr1_without_original_layer "not present (pass-through)"
check 3 flat 640 360 960 540

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
