#!/bin/sh
#
# sq-session end to end: boot with and without a cartridge, hot-plugging
# while a game or the menu runs, CART_AUTOLOAD, Cartridge Tools' "Play
# cartridge", the reset button, a RetroArch that won't quit, a cartridge
# that can't be read. RetroArch and sqload are stubs (stubs/); everything
# else is the firmware's own scripts and sqcart.
# Run by tests/run-all.sh under the console's busybox.
#
. "$(dirname "$0")/sandbox.sh"

# Start sq-session as S50ui would, in the background, as if the console was
# just switched on (SQ_BOOT_T0), waiting at most 5 s for a cartridge
StartTestSession(){
	local up
	up=$(cut -d ' ' -f 1 /proc/uptime)
	export SQ_BOOT_T0=${up%.*}
	SetCfg CART_WAIT 5
	"$LIB_DIR/sq-session" > "$R/session.out" 2>&1 &
}

# True while the session runs
SessionAlive(){
	SessionRunning
}

# What happened to the fake RetroArch (TERM, HANG...), on one line
RaEvents(){
	cat "$R/ra-events" 2> /dev/null | tr '\n' ' '
}

# Count WORD: how many calls.log lines (stub calls) contain WORD
Count(){
	grep -c "$1" "$R/calls.log" 2> /dev/null || echo 0
}

echo "boot without a reader"
NewSandbox
StartTestSession
usleep 3500000
Check "the splash stays up while a cartridge could still appear" Equal "$(RaRuns)" 0
Check "menu starts" WaitFor 6 RaLastHas "--menu"
Check "RetroArch runs (didn't freeze reading its stdin)" eval '! grep -q HANG "$R/ra-events" 2> /dev/null'
Check "with the SD card's config" RaLastHas "-c $R/userdata/retroarch/retroarch.cfg"
Check "plus the firmware's required settings" RaLastHas "--appendconfig=$R/usr/share/retronsq/retroarch-cfw.cfg"
Check "boot wait logged" Contains "$LOG_DIR/launcher.log" "boot: cartridge reader none"
Check "mode is menu" Equal "$(cat "$MODE_FILE")" menu
Check "built-in cores copied to the SD card" Equal "$(ls "$USER_CORE_DIR" | grep -c _libretro.so)" 4
Check "playlists written" Equal "$(ls "$R/userdata/retroarch/playlists" | grep -c lpl)" 1

echo "insert while the menu runs (auto-load on)"
LoadMedium Pokemon_LeafGreen.gba 524288 131072
CartEvent add
Check "cartridge game starts" WaitFor 8 RaLastHas "Pokemon_LeafGreen.gba"
Check "menu was quit with exactly one SIGTERM" Equal "$(RaEvents)" "TERM "
Check "with the GBA core" RaLastHas "-L $USER_CORE_DIR/gpsp_libretro.so"
Check "ROM copied to the SD card intact" cmp -s "$READER_DIR/Pokemon_LeafGreen.gba" "$ROM_PATH/Pokemon_LeafGreen.gba"
Check "cartridge save imported" cmp -s "$READER_DIR/Pokemon_LeafGreen.sav" "$ROM_PATH/Pokemon_LeafGreen.srm"
Check "cartridge save backed up" Equal "$(ls "$R/userdata/saves/Pokemon_LeafGreen/")" "001-cartridge.sav"
Check "saves logged before the run" Contains "$LOG_DIR/launcher.log" "before cartridge save: 131072 bytes"
Check "mode is cart" Equal "$(cat "$MODE_FILE")" cart
Check "the cartridge was never written" Equal "$(grep -c 'rw' "$R/calls.log")" 0

echo "pull it while playing"
RUNS=$(RaRuns)
EjectMedium
CartEvent change 1
Check "back to the menu" WaitFor 6 RaLastHas "--menu"
Check "the game was quit with one SIGTERM (it saves)" Equal "$(RaEvents)" "TERM TERM "
Check "save state logged after the run" Contains "$LOG_DIR/launcher.log" "after live save: 131072 bytes"
Check "RetroArch's log kept on the SD card" test -f "$LOG_DIR/retroarch.log"

echo "same cartridge again"
LoadMedium Pokemon_LeafGreen.gba 524288 131072
# same ROM as before: put back the bytes the cache was made from
cp "$ROM_PATH/Pokemon_LeafGreen.gba" "$R/fakedev/sda/"
CrcFile "$R/fakedev/sda/Pokemon_LeafGreen.gba" "$R/fakedev/sda/Pokemon_LeafGreen.crc"
CartEvent change 1
Check "starts again" WaitFor 8 RaLastHas "Pokemon_LeafGreen.gba"
Check "without copying it again" Equal "$(Count sqload)" 1
Check "the live save was kept, not replaced by the cartridge's" Equal "$(ls "$R/userdata/saves/Pokemon_LeafGreen/" | wc -l)" 2

echo "reset button"
RUNS=$(RaRuns)
"$LIB_DIR/reset-btn"
Check "the game restarts" WaitFor 6 RaStartedAtLeast $((RUNS + 1))
Check "same game" RaLastHas "Pokemon_LeafGreen.gba"

echo "a stuck RetroArch"
RUNS=$(RaRuns)
touch "$R/ra-stuck"
"$LIB_DIR/reset-btn"
rm "$R/ra-stuck"
Check "killed after 6 s" Contains "$LOG_DIR/launcher.log" "RetroArch didn't quit within 6 s, killing it"
Check "never sent a second SIGTERM (it would quit without saving)" eval '! grep -q TERM2 "$R/ra-events"'
Check "and restarted" WaitFor 6 RaStartedAtLeast $((RUNS + 1))

echo "shutdown"
StopSession
Check "session stopped" WaitFor 3 eval '! SessionAlive'
Check "RetroArch stopped" WaitFor 3 eval '! RaRunning'

echo "auto-load off"
NewSandbox
SetCfg CART_AUTOLOAD 0
StartTestSession
Check "menu starts" WaitFor 10 RaLastHas "--menu"
LoadMedium Golden_Sun.gba 65536
CartEvent add
Check "insert logged" WaitFor 3 Contains "$LOG_DIR/launcher.log" "cartridge inserted: Golden_Sun.gba"
usleep 1500000
Check "the menu keeps running" Equal "$(RaRuns)/$(RaEvents)" "1/"
# What Cartridge Tools' "Play cartridge" does
touch "$PLAY_FLAG"
kill $(cat "$R/fake-ra.pid")
Check "Play cartridge starts it" WaitFor 8 RaLastHas "Golden_Sun.gba"
Check "the request is used up" test ! -f "$PLAY_FLAG"
LoadMedium Advance_Wars.gba 65536
CartEvent change 1
Check "a swap stops the old cartridge's game even with auto-load off" WaitFor 6 RaLastHas "--menu"

echo "an SD card set up by v0.2.0 (stdin_cmd_enable = true)"
NewSandbox
mkdir -p "$R/userdata/retroarch"
echo 'stdin_cmd_enable = "true"' > "$R/userdata/retroarch/retroarch.cfg"
StartTestSession
Check "the menu starts and runs" WaitFor 10 RaLastHas "--menu"
usleep 500000
Check "RetroArch didn't freeze" eval '! grep -q HANG "$R/ra-events" 2> /dev/null'

echo "boot with a cartridge (reader still settling)"
NewSandbox
LoadMedium Pokemon_Emerald.gba 262144
StartTestSession
Check "the cartridge starts" WaitFor 8 RaLastHas "Pokemon_Emerald.gba"
Check "the menu never showed" Equal "$(RaRuns)" 1
Check "mounted by the boot wait" Contains "$LOG_DIR/launcher.log" "boot: cartridge reader ready"

echo "boot with a cartridge the reader reports late (empty at first)"
NewSandbox
PlugReader
StartTestSession
usleep 3000000
LoadMedium Pokemon_FireRed.gba 131072
Check "the cartridge starts" WaitFor 8 RaLastHas "Pokemon_FireRed.gba"
Check "the menu never showed" Equal "$(RaRuns)" 1
Check "state changes logged" Contains "$LOG_DIR/launcher.log" "boot: cartridge reader empty at"

echo "boot with a cartridge (reader shows up late)"
NewSandbox
StartTestSession
usleep 800000
LoadMedium Pokemon_Ruby.gba 131072
Check "the cartridge starts" WaitFor 8 RaLastHas "Pokemon_Ruby.gba"
Check "the menu never showed" Equal "$(RaRuns)" 1

echo "a cartridge that can't be read"
NewSandbox
LoadMedium Bad_Repro.gba 65536
"$LIB_DIR/cart-event" reconcile
chmod 000 "$READER_DIR/Bad_Repro.gba"
StartTestSession
Check "the menu shows after two failed copies" WaitFor 10 RaLastHas "--menu"
Check "failure logged" Contains "$LOG_DIR/launcher.log" "cartridge failed to load twice"
Check "tried twice" Equal "$(Count sqload)" 2

echo "NO_CART=none"
NewSandbox
SetCfg NO_CART none
StartTestSession
usleep 3000000
Check "nothing starts without a cartridge" Equal "$(RaRuns)" 0
LoadMedium Tetris.gb 32768
CartEvent add
Check "a cartridge still starts" WaitFor 8 RaLastHas "Tetris.gb"
Check "with the GB core" RaLastHas "vbam_libretro.so"

Done
