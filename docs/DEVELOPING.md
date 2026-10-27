# Developing the Retron SQ firmware

How to build, test and extend the firmware. For how it works, see
[DOCUMENTATION.md](DOCUMENTATION.md); for using it, the
[README](../README.md).

- [Where things are](#where-things-are)
- [Building](#building)
- [Testing](#testing)
- [Rules of the code](#rules-of-the-code)
- [Recipes](#recipes)
  - [Add a setting](#add-a-setting)
  - [Change the home screen](#change-the-home-screen)
  - [Add a sqcart command](#add-a-sqcart-command)
  - [Add a Cartridge Tools action](#add-a-cartridge-tools-action)
  - [React to a new event](#react-to-a-new-event)
  - [Add an emulator core](#add-an-emulator-core)
  - [Add shaders, controller profiles, factory files](#add-shaders-controller-profiles-factory-files)
  - [Change the boot splash or loading screen](#change-the-boot-splash-or-loading-screen)
  - [Change RetroArch](#change-retroarch)
  - [Change the kernel command line](#change-the-kernel-command-line)
  - [Make a release](#make-a-release)
- [Debugging on the console](#debugging-on-the-console)

## Where things are

```
buildroot/package/retronsq/      everything the firmware adds (one package)
  src/lib/                       libsqcart: all cartridge and save logic (C)
  src/cli/  src/load/  src/tools/  sqcart, sqload, Cartridge Tools
  src/home/                      sqhome, the home screen
  scripts/                       common.sh, sq-session, cart-event, ...
  udev/  config/  splash/        udev rule, defaults + RetroArch settings, screens
  retronsq.mk  Config.in         how Buildroot builds and installs it
buildroot/package/retroarch/     RetroArch 1.7.4: src/ + a .patch per change
buildroot/package/rockchip/partinit/S21mountall.sh   storage at boot
buildroot/configs/rockchip_rk3128_brk01_defconfig    Buildroot packages
buildroot/board/rockchip/rk3128/fs-overlay/          stock files we changed
device/rockchip/userdata/userdata_brk01/             SD card factory files
kernel/arch/arm/boot/dts/rk3128-brk01.dts            device tree, cmdline
tests/                           tests (run-all.sh)
docker/Containerfile             build and test environment
build-in-container.sh            entry point for builds and tests
.github/workflows/               tests on push; releases by hand
VERSION                          the firmware version
```

The rest of the tree is Rockchip's 2018 SDK (U-Boot, kernel, Buildroot,
tools) and is changed only where needed.

## Building

Everything builds in an Ubuntu 18.04 container (podman or docker), because
the 2018 SDK can't build with current compilers, and its scripts break on
paths with spaces:

```bash
./build-in-container.sh
```

The first build takes 1–3 hours and about 15 GB; later ones only rebuild
what changed. Results land in `rockdev/`:
`RetronSQ-CFW-v<version>-sdcard.img` (and `.zip`) to write to a microSD
card, and `update.img` for Rockchip's flashing tools.

Other targets:

```bash
./build-in-container.sh kernel
```

```bash
./build-in-container.sh shell
```

`shell` opens a shell in the build environment. From there,
Buildroot can be driven by hand:

```bash
source buildroot/build/envsetup.sh rockchip_rk3128_brk01 && make retronsq-rebuild && cd /work && ./build.sh
```

- **Local packages refresh themselves.** Buildroot copies a package whose
  sources live in the tree (`retronsq`, `retroarch`, `partinit`) only once.
  `device/rockchip/common/mk-buildroot.sh` rebuilds each one whose files
  are newer than its last copy, so an edit always reaches the image.
- **Finalize hooks** (`retronsq.mk`) install the version, the rendered
  splash screens and the factory SD card files on every build, without a
  package rebuild.
- **A package removed from the defconfig** leaves its files in
  `buildroot/output/.../target/` (Buildroot never deletes them). Delete them
  by hand, using `build/packages-file-list.txt` to see which package owned
  what, or rebuild from scratch.
- **Changing a Buildroot option** of an already-built package needs that
  package rebuilt (`make <pkg>-dirclean`).
- One build at a time: `build-in-container.sh` holds a lock.

## Testing

```bash
./build-in-container.sh test
```

That runs `tests/run-all.sh` in the container's `test` stage (the build
image plus SDL2, Xvfb and Mesa). It takes a few minutes, needs no console
and no firmware build, and runs on every push to GitHub
(`.github/workflows/tests.yml`; releases run it before building). Run
some groups only with:

```bash
./build-in-container.sh test session cartevent
```

| Group | What |
| --- | --- |
| `syntax` | every shell script parses under the console's busybox `sh` |
| `patches` | each `buildroot/package/retroarch/000N-*.patch` is applied in `src/` |
| `build` | sqcart, sqload and the tools core build for the host with `-Werror` |
| `lib` | libsqcart unit tests (`tests/lib/test_sqcart.c`), also under AddressSanitizer + UBSan |
| `tools` | Cartridge Tools driven frame by frame (`tests/tools/test_sqtools.c`), sanitizers too |
| `config` | settings, core choice and validation, playlists, audio route |
| `cartevent` | hot-plug: insert, swap with and without reconnecting, stale mounts, partitions, unreadable media, racing events |
| `session` | sq-session end to end: boot with/without cartridge, insert/pull while running, auto-load off, Play cartridge, reset, a stuck RetroArch, a v0.2.0 SD card, a cartridge reported late, failed copies, `NO_CART=none` |
| `grow` | first-boot partition grow on GPT images (512 MiB and 128 GiB cards, already full, blocked) |
| `load` | the loading screen under Xvfb: copy, save import, screenshots, no display, a cartridge that stops answering |
| `home` | the home screen, headless (SDL's dummy video driver), driven by scripted buttons: playing, history, favorites, box art, settings, restart/power off, the cartridge page, SIGTERM |

How they work:

- **Sandboxes.** Every path in the firmware starts with `$SQ_ROOT`
  (`sq_path()` in C, `$R` in the scripts). Tests point it at a temporary
  directory with a fake reader in a fake sysfs, a fake `/proc/mounts`, an SD
  card tree and stubs for what needs hardware (`tests/scripts/stubs/`:
  `mount`, `umount`, `hdparm`, `amixer`, `retroarch`, `sqload`). On the
  console `$SQ_ROOT` is empty.
- **The console's shell.** The scripts run under BusyBox 1.27.2 built from
  `buildroot/dl` with the console's own `busybox.config`, with its applets
  first on `PATH`, so a bash-ism or a missing applet fails the test as it
  would on the console.
- **Stubs behave like the real thing where it matters.** The fake
  RetroArch freezes like the real one when given a stdin that never ends
  with `stdin_cmd_enable` on, quits on one SIGTERM and exits without saving
  on a second. A stub that only does what the firmware expects can't catch
  a wrong expectation (that is how v0.2.0's frozen RetroArch got through).
- **Helpers.** `tests/scripts/sandbox.sh` (shell: `NewSandbox`,
  `LoadMedium`, `EjectMedium`, `CartEvent`, `SetCfg`, `Check`, `WaitFor`,
  ...) and `tests/lib/sandbox.h` (C: `insert_cart`, `remove_cart`, `CHECK`,
  ...).
- `SQTOOLS_SHOTS=DIR` keeps the screenshots the tools and loading screen
  tests take.

A new feature should come with a test in the matching group. Most need a
few lines in an existing file.

## Rules of the code

- **Never write to a cartridge.** Open reader files read-only; the mount and
  the block device are read-only too. Saves live on the SD card. The only
  exception is `src/lib/cartwrite.c`, behind Cartridge Tools' explicit,
  double-confirmed *Write a save to the cartridge*; keep it that way.
- **Write files atomically**: `sq_write_atomic()` in C (temporary file,
  fsync, rename); in shell, write `file.tmp` then `mv`. The console is
  switched off by pulling the power.
- **All cartridge logic is in libsqcart.** Scripts call `sqcart`, programs
  link the library. Don't re-implement a CRC or a copy anywhere else.
- **Only sq-session starts RetroArch.** Everything else stops it
  (`KillGame`) and lets the session decide what runs next. The home screen
  too: it writes a request and exits.
- **Keep the home screen light.** Draw only when something changed, no
  per-frame work, cache text, load images lazily. The console is a
  Cortex-A7 with a Mali-400.
- **Paths through `$SQ_ROOT`**: `sq_path()` in C, `$R`/the variables in
  `common.sh` in scripts. That's what makes the tests possible.
- **Shell is BusyBox ash.** No arrays, no `[[ ]]`, no `${var//}`; `local`
  is fine. Check an applet is enabled in
  `buildroot/board/rockchip/common/base/busybox.config` before using it.
  Mind redirections: `cmd 2> /dev/null < "$file"` (ash applies them left to
  right).
- **Validate what comes from the SD card.** Files are edited on PCs:
  tolerate CRLF and quotes (`CfgGet`), and never put a value into a path or
  command without checking it (`ValidCoreName`, `CfgNum`).
- **Log what matters for bug reports** with `LogSD` (goes to
  `logs/launcher.log`); `Log` and `sq_log()` go to the serial console.
- **Comment the why**: every file starts with what it is for, every
  function with what it does. Match the surrounding style.

## Recipes

### Add a setting

1. Pick a name and default; add it to `config/defaults.cfg` (the only
   place defaults live; the scripts and libsqcart both read it).
2. Read it where needed: `CfgGet NAME` for words, `CfgNum NAME MAX` for
   numbers (non-numbers fall back to the default).
3. Document it in `device/rockchip/userdata/userdata_brk01/retronsq.cfg`
   (what users see) and the settings table in `DOCUMENTATION.md`. To make
   it changeable on the console, add a row to the home screen's Settings
   (next recipe).
4. Test it in `tests/scripts/test_config.sh` or `test_session.sh`
   (`SetCfg NAME value`).

Existing SD cards don't get new lines in their `retronsq.cfg` (user files
are never overwritten), which is why the default must be the right
behaviour.

### Change the home screen

`src/home/home.c`:

- **A setting row:** add to `enum setting`, `setting_label[]` and
  `setting_key[]` (the `retronsq.cfg` key), then how it looks
  (`setting_text`) and how Left/Right/A change it (`change_setting`).
  Values are saved with `sq_cfg_set`, which only takes plain values.
- **A page:** add to `enum page` and `page_name[]`, fill `games` in
  `load_page` for a game list (or draw it yourself in `render`, like the
  Cartridge and Settings pages).
- **Starting something new:** add an `ACTION=` to `request()` callers and
  handle it in `RunHomeRequest` (`common.sh`). Validate anything that comes
  from SD card files before using it in a path or command.
- **Look:** colours are the `COL_*` values in `home.h`, sizes the layout
  `#define`s at the top of `home.c`, all in 1280×720 coordinates.
- **Test it** in `tests/home/test_home.sh` (`RunHome "down a ..."`, then
  check the request, `retronsq.cfg` or `home/`), and look at it:
  `SQTOOLS_SHOTS=dir` saves every `shot:NAME` as a BMP.

### Add a sqcart command

1. Put the logic in libsqcart (`src/lib/*.c`, declared and documented in
   `sqcart.h`), with an `struct sq_error*` for failures.
2. Add the command to `src/cli/sqcart.c`: the comment at the top, `usage()`
   and `main()`. Print `KEY=value` lines; the exit status says whether it
   worked.
3. Call it from `common.sh` as `"$SQCART" <command>`.
4. Unit-test the library function in `tests/lib/test_sqcart.c`.

### Add a Cartridge Tools action

Put the action itself in `src/lib/actions.c` (`sq_do_*`: the steps and the
lines shown to the user), so the home screen's Cartridge page and the
RetroArch tools core both get it. Then add it to both front ends: in
`src/home/home.c`, `enum cart_item`, `cart_label[]`, `cart_help[]`,
`cart_item_enabled`, `activate` and a `TASK_*`; and in `src/tools/sqtools.c`:

1. Add an `ITEM_*` to `enum item` and its label to `item_label[]`.
2. Say when it's available in `ItemEnabled()` and what it does in
   `ItemHelp()`.
3. In `HandleInput()`, `case ITEM_...:` either act at once, or run a
   task: a `static void* TaskSomething(void* arg)` that sets
   `work_title`/`work_total`, calls the `sq_do_*` action, shows its result
   with `SetResult()` and clears `worker_busy`; then
   `StartTask(TaskSomething)` (which first checks the cartridge wasn't
   swapped).
4. Test it in `tests/tools/test_sqtools.c` (`choose(ITEM_...)`,
   `wait_task()`, `message_is()`) and `tests/home/test_home.sh`.

Result lines are at most 50 characters (`SQ_RESULT_COLS`): the tools core's
screen is 320×240.

### React to a new event

1. Get the event to a script: a udev rule (`udev/`, installed by
   `retronsq.mk`), an `input-event-daemon.conf` line for a button, or an
   init script.
2. The script sources `common.sh` and calls a handler there. Handlers only
   change state (files in `/run/retronsq`) and call `KillGame`; sq-session's
   loop then picks what runs.
3. Keep udev handlers short: start slow work with `setsid ... &`.
4. Add a case to `tests/scripts/test_session.sh`.

### Add an emulator core

1. Add a Buildroot package for it (see `buildroot/package/libretro-gpsp/`)
   installing `<name>_libretro.so` into `/usr/lib/libretro/`, and enable it
   in the defconfig.
2. Add its `<name>_libretro.info` to
   `device/rockchip/userdata/userdata_brk01/retroarch/cores/`, so RetroArch
   shows its name.
3. Users can then name it in `retronsq.cfg` or `cart-overrides.txt`.
   `SyncUserdata` copies built-in cores to the SD card on the next boot.

Users can also just copy an armhf `.so` into `retroarch/cores` on the card.

### Add shaders, controller profiles, factory files

Anything under `device/rockchip/userdata/userdata_brk01/` is on new SD
cards, and is put back on existing cards when missing (`SyncUserdata`). The
Mali-400 has no `highp` in fragment shaders and little power: test shaders
at 720p.

### Change the boot splash or loading screen

Edit `buildroot/package/retronsq/splash/retronsq-splash.svg` or
`retronsq-loading.svg` (keep the font "DejaVu Sans"; `@VERSION_LABEL@` is
replaced by the version). For the loading screen, keep the progress track
and text areas where the comment at the top of the SVG says, or move the
`LOADING_*` values at the top of `src/load/sqload.c` with them. To see the
result without a console, run the `load` tests with `SQTOOLS_SHOTS` set.

### Change RetroArch

Buildroot doesn't apply patches to a local-source package, so RetroArch's
`src/` holds the patched code, and each change is also kept as a patch:

1. Copy the original file, edit `buildroot/package/retroarch/src/...`.
2. Write the patch: `diff -u a/path b/path` with a header saying why, saved
   as the next `000N-*.patch` in `buildroot/package/retroarch/`. List it in
   the comment in `retroarch.mk`.
3. The `patches` test checks every patch is applied in `src/`.

To look at RetroArch's options, `retroarch.cfg` on the SD card is the
user's; settings the firmware depends on go in
`buildroot/package/retronsq/config/retroarch-cfw.cfg`, which is loaded on
top with `--appendconfig`.

### Change the kernel command line

`bootargs` in `kernel/arch/arm/boot/dts/rk3128-brk01.dts`. Check the
result with `strings kernel/resource.img | grep console=` after a build.

### Make a release

1. Set the version in `VERSION` (0.x versions are shown as "beta").
2. Commit and push to `main`.
3. GitHub → Actions → **Build firmware release** → Run workflow, with the
   same version. It runs the tests, builds (2–3 hours), and publishes
   `RetronSQ-CFW-v<version>-sdcard.zip`, `...-update.img` and
   `SHA256SUMS.txt` as release `v<version>`.

## Debugging on the console

- **Logs without opening the console:** `logs/launcher.log`,
  `logs/retroarch.log` and `logs/boot.log` on the SD card (see
  DOCUMENTATION.md, *Logs and troubleshooting*).
- **Serial console** (115200 8N1 on the board's UART pads) gives a root
  shell and every `sq:` log line.
- On the console, `sqcart info`, `sqcart reader-state` and
  `sqcart check-save /userdata/rom/<game>.srm <CODE>` show what the
  firmware sees; `cat /run/retronsq/*` shows the session's state.
- `kill $(pidof retroarch)` (one SIGTERM) quits RetroArch the way the
  firmware does, saving the game.
