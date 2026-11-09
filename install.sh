#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the layer and installs it for the current user only.
# The layer stays off until a program is started with VRAM_FALLBACK=1.
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -DVRAM_FALLBACK_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

LIBDIR="${XDG_DATA_HOME:-$HOME/.local/share}/vram-fallback"
MANIFEST_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d"
mkdir -p "$LIBDIR" "$MANIFEST_DIR"
install -m 755 build/libVkLayer_vram_fallback.so "$LIBDIR/"
sed "s|\"libVkLayer_vram_fallback.so\"|\"$LIBDIR/libVkLayer_vram_fallback.so\"|" \
  layer/VkLayer_vram_fallback_implicit.json > "$MANIFEST_DIR/VkLayer_vram_fallback.json"
echo "Installed. Steam launch options:  VRAM_FALLBACK=1 %command%"
