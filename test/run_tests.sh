#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the test app on Mesa lavapipe, made to look like a 256 MiB NVIDIA card
# by the nvsim layer, in four configurations. Usage: test/run_tests.sh [build-dir]
set -u
BUILD="${1:-$(dirname "$0")/../build}"
BUILD="$(cd "$BUILD" && pwd)"
export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/lvp_icd.json}"
export VK_ADD_LAYER_PATH="$BUILD"
export NVSIM_VRAM_MB=256
VAL=VK_LAYER_KHRONOS_validation
pass=0; fail=0

run() {  # name, expected exit code, layers, extra env...
  local name="$1" expect="$2" layers="$3"; shift 3
  echo "=== $name"
  env VK_INSTANCE_LAYERS="$layers" "$@" "$BUILD/vram_test" 2>&1 | sed 's/^/    /'
  local rc=${PIPESTATUS[0]}
  if [ "$rc" = "$expect" ]; then echo "--- ok (exit $rc)"; pass=$((pass+1));
  else echo "--- UNEXPECTED exit $rc (wanted $expect)"; fail=$((fail+1)); fi
}

# Without the layer the app must hit OUT_OF_DEVICE_MEMORY, like on a real NVIDIA card.
run "baseline: no fallback layer (should fail)" 1 "$VAL:VK_LAYER_TEST_nvsim"
run "fallback layer + validation" 0 "VK_LAYER_VRAM_fallback:$VAL:VK_LAYER_TEST_nvsim" VRAM_FALLBACK_LOG=2
run "fallback layer, 64 MiB headroom" 0 "VK_LAYER_VRAM_fallback:$VAL:VK_LAYER_TEST_nvsim" VRAM_FALLBACK_HEADROOM_MB=64
run "fallback layer, no validation" 0 "VK_LAYER_VRAM_fallback:VK_LAYER_TEST_nvsim"

echo "passed $pass, failed $fail"
[ "$fail" = 0 ]
