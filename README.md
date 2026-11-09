# VK_LAYER_VRAM_fallback

A Vulkan layer that does what Windows does for free: when a game runs out of
VRAM, put the overflow in system RAM instead of crashing.

On Linux, NVIDIA's driver returns `VK_ERROR_OUT_OF_DEVICE_MEMORY` once VRAM is
full. Many games treat that as fatal. This layer sits between the game (or
DXVK/VKD3D-Proton) and the driver. When a device-local allocation fails, it
quietly retries in system memory that the GPU can still read and write, and
hands that back as if nothing happened. Expect lower frame rates and stutter
once it kicks in, because the GPU then reads that data over PCIe. Windows has
the same cost.

**Status: prototype.** It has been tested successfully on a real NVIDIA card
with 4 GB of VRAM, and on a simulated 256 MiB NVIDIA card (see
[Testing](#testing)).

## Install and use

On **Arch Linux**, use the PKGBUILD. See [Arch Linux](#arch-linux) below.

On **Debian, Ubuntu, Mint and Pop!_OS**, build a `.deb`. See
[Debian and Ubuntu](#debian-and-ubuntu) below.

On other distros:

```sh
sudo apt install cmake g++ libvulkan-dev      # or your distro's equivalent
./install.sh                                   # installs for your user only
```

`install.sh` only builds the 64-bit layer. That covers native Linux games and
64-bit Windows games under Proton. 32-bit Windows games load 32-bit Vulkan,
and they need the 32-bit build that the Arch and Debian packages provide.

The layer does nothing until a program is started with `VRAM_FALLBACK=1`. In
Steam, set a game's launch options to:

```
VRAM_FALLBACK=1 %command%
```

Run `./uninstall.sh` to remove it.

| Variable | Default | Meaning |
|---|---|---|
| `VRAM_FALLBACK=1` | off | Turn the layer on for this process. |
| `DISABLE_VRAM_FALLBACK=1` | | Force it off. |
| `VRAM_FALLBACK_HEADROOM_MB` | `0` | Keep this much VRAM free. Once the driver's budget says less than this is left, new VRAM allocations go straight to system RAM. Try `256` to `512` if games still crash with the layer on (see below). |
| `VRAM_FALLBACK_LOG` | `1` | `0` is quiet, `1` logs each fallback to stderr, `2` is verbose. |

With logging on, you'll see lines like this in the game's log (for Proton,
use `PROTON_LOG=1`):

```
[vram-fallback]   type 1 (heap 0, flags 0x1) -> falls back to type 0 (heap 1, flags 0x0)
[vram-fallback] VRAM full: 64 MiB type 1 -> system type 0 (now 64 MiB in system RAM)
```

If no `VRAM full` lines show up before a crash, the crash didn't come from a
failed `vkAllocateMemory`, and only the headroom setting can help.

Steam's Linux runtime container normally picks up layers from
`~/.local/share/vulkan/implicit_layer.d`, and `/home` is visible inside it, so
the user install should work there. Flatpak Steam can't see that folder. For
Flatpak you'd need to install the layer inside the Flatpak's data directory.
That setup is untested.

## Arch Linux

### Install

The PKGBUILD in `packaging/arch/` builds from the clone it sits in, using
the latest commit there. It makes two packages:

- `vram-fallback-layer-git` installs `/usr/lib/libVkLayer_vram_fallback.so`
  and the manifest
  `/usr/share/vulkan/implicit_layer.d/VkLayer_vram_fallback.json`.
- `lib32-vram-fallback-layer-git` installs
  `/usr/lib32/libVkLayer_vram_fallback.so` for 32-bit games.

The manifest names the library without a path, so the Vulkan loader picks
`/usr/lib` or `/usr/lib32` to match the game.

Enable the `[multilib]` repository in `/etc/pacman.conf` first (uncomment the
`[multilib]` section, then run `sudo pacman -Syu`). It provides the 32-bit
packages. Then:

```sh
sudo pacman -S --needed base-devel git cmake vulkan-headers \
    vulkan-icd-loader lib32-vulkan-icd-loader lib32-gcc-libs lib32-glibc
git clone https://github.com/jartificer/vram-fallback-layer
cd vram-fallback-layer/packaging/arch
makepkg -si
```

`makepkg -si` builds both packages and asks pacman to install them. To
update, run `git pull` and then `makepkg -si` again in that folder.
Uncommitted changes aren't included in the package. To remove both packages:

```sh
sudo pacman -R lib32-vram-fallback-layer-git vram-fallback-layer-git
```

If the 32-bit build fails with errors about `-m32`, `bits/c++config.h` or
`cannot find -lstdc++`, install the multilib toolchain with
`sudo pacman -S multilib-devel` and try again.

For your NVIDIA card you also need `nvidia-utils` (64-bit games) and
`lib32-nvidia-utils` (32-bit games). These come with the NVIDIA driver
packages (`nvidia-open` or `nvidia-open-dkms`).

Don't install with `install.sh` as well. You'd end up with two manifests for
the same layer.

### Use with Steam and Proton

The `steam` package from `[multilib]` runs Proton games inside the Steam Linux
Runtime container. The container picks up Vulkan layers installed in `/usr`.
Set the game's launch options (right-click the game, then Properties, then
General, then Launch Options):

```
VRAM_FALLBACK=1 %command%
```

To see whether the layer kicked in, add `PROTON_LOG=1`. Proton then writes
`~/steam-<appid>.log`, where you can search for `[vram-fallback]`:

```
VRAM_FALLBACK=1 VRAM_FALLBACK_LOG=2 PROTON_LOG=1 %command%
```

For native Linux games, `VRAM_FALLBACK_LOG` output goes to the game's stderr.
Launch Steam from a terminal to see it.

To confirm Vulkan sees the layer outside Steam, run:

```sh
VRAM_FALLBACK=1 vulkaninfo --summary 2>&1 | grep -i vram
```

That needs `vulkan-tools`. It lists `VK_LAYER_VRAM_fallback`, and the layer prints an `active` line plus the memory
types it picked, for each GPU.

Flatpak Steam (`com.valvesoftware.Steam`) can't see `/usr/share/vulkan` on the
host, so the package won't work there. Use the `steam` package from
`[multilib]`.

## Debian and Ubuntu

### Build the package

`packaging/debian/build-deb.sh` builds `.deb` files from the latest commit of
the clone it sits in. Like the PKGBUILD, it doesn't download anything and
leaves out uncommitted changes. The Debian packaging files themselves are in
`packaging/debian/debian/`.

```sh
sudo apt install build-essential debhelper cmake libvulkan-dev git
sudo apt install crossbuild-essential-i386     # only for 32-bit games
cd vram-fallback-layer
packaging/debian/build-deb.sh --i386           # leave out --i386 for 64-bit only
```

The packages land in `packaging/debian/out/`, named after the commit, for
example `vram-fallback-layer_0~r8.491ceba_amd64.deb` and the matching
`_i386.deb`. Each one installs:

- the library in `/usr/lib/x86_64-linux-gnu/` or `/usr/lib/i386-linux-gnu/`
- the manifest
  `/usr/share/vulkan/implicit_layer.d/VkLayer_vram_fallback.json`, which is
  the same file in both packages.

As on Arch, the manifest names the library without a path, so the Vulkan
loader picks the 64-bit or 32-bit copy to match the game.

### Install

```sh
sudo dpkg --add-architecture i386               # Steam has usually done this already
sudo apt install ./packaging/debian/out/vram-fallback-layer_*_amd64.deb \
                 ./packaging/debian/out/vram-fallback-layer_*_i386.deb
```

To update, run `git pull`, build again and install the new files. Remove the
old ones from `out/` first, or the wildcards above match several versions. To
remove the layer:

```sh
sudo apt remove vram-fallback-layer:amd64 vram-fallback-layer:i386
```

For your NVIDIA card, the 32-bit package needs the driver's 32-bit Vulkan
libraries too (`libnvidia-gl-<version>:i386` on Ubuntu). The Steam setup
normally installs them.

Don't install with `install.sh` as well. You'd end up with two manifests for
the same layer. Steam and Proton are set up the same way as on Arch, see
[Use with Steam and Proton](#use-with-steam-and-proton).

## How it works

1. **Retry in system memory.** When `vkAllocateMemory` asks for a
   `DEVICE_LOCAL` type and gets `VK_ERROR_OUT_OF_DEVICE_MEMORY`, the layer
   retries with a memory type on a non-device-local heap. Imports
   (dma-buf, host pointers) are never redirected.
2. **Keep the game's assumptions true.** The fallback type must keep any
   `HOST_VISIBLE` and `HOST_COHERENT` flags the original had, so ReBAR/upload
   memory that the game maps stays mappable. Among the valid types, the layer
   prefers plain GPU-only system memory, which NVIDIA exposes as a type with no
   flags. It avoids `HOST_CACHED` unless the original type had it.
3. **Respect `memoryTypeBits`.** This is the hard part. `vkAllocateMemory`
   doesn't say which buffers or images will live in the memory. DXVK,
   VKD3D-Proton and VMA allocate large blocks per memory type and then pack any
   resource that accepts that type into them. NVIDIA only lets optimal-tiling
   images (textures, render targets) live in two types: VRAM, and the plain
   system type. Picking the "upload" type for a texture block would be invalid.
   So the layer:
   - creates a few typical probe buffers and images at `vkCreateDevice`,
   - watches every `vkGet*MemoryRequirements*` call,
   - for each type, remembers which other types every resource that accepts
     it also accepts (a running AND of the masks),
   - only falls back to a type in that set. For dedicated allocations
     (`VkMemoryDedicatedAllocateInfo`), it uses that exact resource's mask.
4. **Safety net at bind time.** If a resource with an unusually narrow mask
   later gets bound into fallback memory it can't use, `vkBind*Memory`
   returns `VK_ERROR_OUT_OF_DEVICE_MEMORY` instead of letting the GPU corrupt
   memory. That's a legal result, and allocators handle it by trying
   elsewhere.
5. **Headroom mode** (`VRAM_FALLBACK_HEADROOM_MB`). The layer turns on
   `VK_EXT_memory_budget` when the driver supports it, and reads real VRAM
   usage, including other processes such as the compositor. When free VRAM drops
   below the headroom, new allocations go to system RAM *before* VRAM is
   exhausted.

## Trade-offs and limits

- **The crash may not be a failed allocation.** The NVIDIA driver also needs
  VRAM for its own use (shaders, command buffers, page tables), and those
  allocations don't go through `vkAllocateMemory`. If the game fills VRAM to
  the last byte, the driver itself can fail. That shows up as a device-lost
  error or an Xid in `dmesg`, which no layer can catch. Headroom mode is the
  mitigation: it leaves the driver some room.
- **DXVK already does part of this.** DXVK falls back to system memory for
  many resources on its own, and VKD3D-Proton does for some. So this layer
  helps most with native Vulkan games, with VKD3D's dedicated render-target
  allocations, and through headroom mode. Gains on DXVK titles may be small.
- **Nothing moves back.** Once a block lands in system RAM, it stays there
  until the game frees it. Windows can migrate memory back into VRAM. This
  layer can't, because the GPU addresses are fixed once resources are bound.
- **Budget numbers are passed through unchanged.** Games that read
  `VK_EXT_memory_budget` still see the real VRAM budget. They also count
  fallback blocks as VRAM in their own bookkeeping, so they tend to
  self-limit (lower texture streaming) a bit earlier. That's harmless. The
  layer deliberately does *not* inflate the VRAM heap size, because that just
  makes engines allocate more aggressively.
- **System memory is pinned.** The fallback memory is locked in RAM while
  allocated. On a 16 GB machine, a few GB of spill is fine. A game that wants
  10 GB more than the card has won't be.
- **The type-mask learning is optimistic early on.** It's built from probes
  and from what the game has asked so far. The bind-time check makes a later
  surprise fail cleanly rather than corrupt anything.
- **Driver 595 may change this.** The 595 beta release notes mention
  "improved support for falling back to system memory when available video
  memory is low". Try a current driver first. This layer is still useful if
  the driver keeps returning out-of-memory errors.

## Testing

### Real hardware

The layer has been tested successfully on a real NVIDIA card with 4 GB of
VRAM.

### Simulated card

The automated tests run without an NVIDIA GPU, on Mesa's software Vulkan driver
(lavapipe). A test-only layer, `VK_LAYER_TEST_nvsim` in
`test/nvsim_layer.cpp`, makes lavapipe look like a 256 MiB NVIDIA card. It
gives lavapipe NVIDIA's memory-type layout, NVIDIA's image `memoryTypeBits`
(types 0 and 1 only), a working memory budget, and an out-of-memory error once
the fake VRAM is full.

```sh
cmake -S . -B build && cmake --build build -j
test/run_tests.sh       # needs mesa-vulkan-drivers and vulkan-validationlayers
```

The test app (`test/vram_test.cpp`) acts like an engine. It allocates
8 × 64 MiB VRAM blocks on the 256 MiB card, puts a buffer and an
optimal-tiling texture into an overflow block, and checks GPU-written data
byte for byte. It then maps a ReBAR allocation while VRAM is full, makes a
dedicated 64 MiB render-target allocation, and checks that VRAM gets used
again after everything is freed. The Khronos validation layer sits between the
fallback layer and the simulator, so it checks every bind against the real
type masks.

| Run | Result |
|---|---|
| No fallback layer | 4 of 8 blocks, the ReBAR allocation, and the dedicated allocation fail with OUT_OF_DEVICE_MEMORY (expected) |
| Fallback layer + validation | All pass, 0 validation errors |
| Fallback layer, 64 MiB headroom | All pass. VRAM use stops at 192 MiB, as intended |
| Fallback layer without validation | All pass |
| Implicit layer, `VRAM_FALLBACK=1` set / unset | Active / inactive |
| Same suite built as 32-bit (`-m32`) | All pass. The 32-bit validation layer wasn't available, so these runs relied on the app's own data checks |
| One manifest with a bare library name, `.so` in both lib dirs | The 64-bit and 32-bit test apps each load the right one |

The PKGBUILD's build and package steps were run by hand on Ubuntu and produce
the file layout listed above. `makepkg` itself hasn't been run on a real Arch
system.

`build-deb.sh --i386` was run on Ubuntu 24.04 and builds both `.deb` files
with no `lintian` complaints. With the amd64 package installed,
`vulkaninfo` loads the layer when `VRAM_FALLBACK=1` is set and not otherwise.
The i386 package was only inspected (32-bit library in
`/usr/lib/i386-linux-gnu/`, dependencies on the i386 libraries), not
installed.

These runs also confirmed that the tests catch mistakes. With the type-mask
logic deliberately broken, the bind-time check refused both bad image binds.
With that check disabled too, validation reported
`VUID-vkBindImageMemory-memory-01047` both times.

## Checking it on your own card

1. Run `vulkaninfo` and check that the NVIDIA memory types look like the
   simulator's. The layer logs the mapping it picked at startup.
2. Try a game that crashes today, with `VRAM_FALLBACK=1 VRAM_FALLBACK_LOG=2`.
   Check whether `VRAM full` lines appear.
3. If it still crashes with no such lines, try `VRAM_FALLBACK_HEADROOM_MB=384`.

## License

Copyright (C) 2026 Radu

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE) for the full text.
