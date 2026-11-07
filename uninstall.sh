#!/usr/bin/env bash
rm -f "${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d/VkLayer_vram_fallback.json"
rm -rf "${XDG_DATA_HOME:-$HOME/.local/share}/vram-fallback"
echo "Removed."
