# Retron SQ custom firmware

Based on Hyperkin's [hyperkin-gb](https://github.com/hyperkin-game/hyperkin-gb)
firmware source, with the RetroArch menu idea from
[SiirRandall's fork](https://github.com/SiirRandall/sirrandall-hyperkin-gb).

## Features

- **RetroArch menu on Start + Select**: save states, shaders, settings, and
  loading other games.
- **Play without a cartridge.** With no cartridge inserted the console boots
  into the RetroArch menu. Games in `roms/gb`, `roms/gbc` and `roms/gba` on the
  SD card are listed under Playlists. Inserting a cartridge always takes over.
- **FAT32 SD card.** The userdata partition is FAT32 (label `RETRONSQ`) and
  grows to fill the card on first boot, so ROMs, saves, cores and shaders can
  be managed from any PC.
- **Shaders.** A set of light GLSL presets that run on the Mali-400 is
  included under `retroarch/shaders`. You can add more.
- **Extra cores.** armhf libretro cores copied into `retroarch/cores` show up
  in Load Core and can be set as the default in `retronsq.cfg`.
- **Faster cartridge loading.**
  - A cartridge is read once. After that it starts from its verified copy
    whenever the reader's CRC matches, with no USB copy and no re-check.
  - The first read pipelines USB reads and SD writes on separate threads with
    256 KB buffers, instead of 1 KB synchronous copies. The CRC is computed
    during the copy rather than in a second pass.
  - The copy starts before the splash screen's GPU and SDL setup. The old
    forced 3 second minimum splash is gone, and the boot scripts no longer
    wait for the copy.
  - Copies are written to `.part` and renamed only when complete, so a
    cartridge pulled mid-read can't leave a half-copied "valid" ROM.
- **Reproduction carts.** A CRC mismatch is retried once. If it still
  mismatches, the game launches anyway and the cartridge is read again on the
  next boot instead of failing. `cart-overrides.txt` can pick a core per
  cartridge (mGBA handles repro save chips best). A cartridge that fails to
  load twice falls back to the menu instead of looping on the loading screen.
- **Previously played cartridges** are kept in `rom/` with their saves and
  are listed as "Cartridge Dumps", so they can be played without the
  cartridge.

### Flash cartridges

Menu-based flash cartridges (EverDrive, EZ-Flash, and similar) can't work on
this hardware. The console never runs the cartridge itself: the reader dumps
the ROM to a file and RetroArch emulates that file, so a flash cart's menu
would run without the flash cart's FPGA, SD slot and mapper behind it. Put the
games on the SD card instead. Single-game flash carts and reproduction carts
dump like any other cartridge.

## How it works

| Piece | Where |
| --- | --- |
| Launcher loop: keeps one RetroArch running (cartridge, else menu) | `buildroot/board/rockchip/rk3128/fs-overlay/usr/bin/sq-launcher` |
| Shared helpers: core choice, cache check, kill/reset, hooks | `buildroot/board/rockchip/rk3128/fs-overlay/etc/check_dumper.sh` |
| Playlist generator | `buildroot/board/rockchip/rk3128/fs-overlay/usr/bin/sq-scan-roms` |
| Cartridge copier and splash (`hyperkin-loading`), CRC tool | `buildroot/package/hyperkin-loading/src/` |
| SD card factory contents (also installed to `/usr/share/retronsq/userdata` to restore missing files) | `device/rockchip/userdata/userdata_brk01/` |
| FAT userdata: fstab, image build, first-boot grow | `buildroot/package/rockchip/partinit/`, `device/rockchip/common/mk-userdata.sh` |

The usbmount hooks, reset button and HDMI hotplug only stop RetroArch.
`sq-launcher` then decides what runs next, so two scripts can never start
games at the same time.

## Building

Works on any Linux with **podman** or **docker** (Fedora, Ubuntu, Arch...):

```
./build-in-container.sh
```

The 2018 Rockchip SDK (Buildroot 2018.02, kernel 4.4) can't be built with a
current distro's compiler and Python, and its scripts break on paths with
spaces. The script runs the stock `./build.sh` inside an Ubuntu 18.04
container (`docker/Containerfile`) with this checkout mounted at `/work`, so
neither matters. No root is needed.

- The first run builds the container image (a few minutes). A full build
  takes 1-2 hours, and later builds are incremental.
- Any `build.sh` target works: `./build-in-container.sh kernel`,
  `./build-in-container.sh uboot`, ... `./build-in-container.sh shell` opens a
  shell in the build environment.
- Only one build can run per checkout at a time. Two builds sharing
  `buildroot/output` corrupt each other.
- macOS and Windows hosts need a Linux VM: Buildroot doesn't work on their
  case-insensitive filesystems.

Output in `rockdev/`:

| File | Use |
| --- | --- |
| `update.img` | Flash with Rockchip's RKDevTool / upgrade_tool |
| `<date>-rk3128-brk01-sdcard-128MB.img` (and `.zip`) | Write to a microSD card with `dd`, balenaEtcher, etc. On first boot the `userdata` partition grows to fill the card. |

Upgrading from stock firmware needs the userdata partition reflashed. The
partition changed from ext2 to FAT32, and an old ext2 one won't mount.
