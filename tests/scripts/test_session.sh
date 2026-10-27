#!/bin/sh
#
# sq-session end to end: boot with and without a cartridge, the home screen
# and what it hands over, hot-plugging
# while a game or the menu runs, CART_AUTOLOAD, Cartridge Tools' "Play
# cartridge", the reset button, a RetroArch that won't quit, a cartridge
# that can't be read. RetroArch and sqload are stubs (stubs/); everything
# else is the firmware's own scripts and sqcart.
# Run by tests/run-all.sh under the console's busybox.
#
. "$(dirname "$0")/sandbox.sh"

# Start sq-session as S50ui would, in the background, as if the console was
# just switched on (SQ_BOOT_T0), waiting at most 5 s for a cartridge. The
# first scenarios use RetroArch's menu as the start screen (UI=retroarch);
# the home screen ones set TEST_UI=home.
StartTestSession(){
	local up
	up=$(cut -d ' ' -f 1 /proc/uptime)
	export SQ_BOOT_T0=${up%.*}
	SetCfg CART_WAIT 5
	SetCfg UI "${TEST_UI:-retroarch}"
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
Check "a copy of the cartridge's save kept" cmp -s "$READER_DIR/Pokemon_LeafGreen.sav" "$R/userdata/saves/Pokemon_LeafGreen/cartridge.sav"
Check "and nothing else (backups only when asked)" Equal "$(ls "$R/userdata/saves/Pokemon_LeafGreen/")" "cartridge.sav"
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
Check "the live save was kept, not replaced by the cartridge's" eval \
	'! cmp -s "$READER_DIR/Pokemon_LeafGreen.sav" "$ROM_PATH/Pokemon_LeafGreen.srm"'
Check "the copy of the cartridge's save follows the cartridge" \
	cmp -s "$READER_DIR/Pokemon_LeafGreen.sav" "$R/userdata/saves/Pokemon_LeafGreen/cartridge.sav"
Check "still one file: no backup piling up" Equal "$(ls "$R/userdata/saves/Pokemon_LeafGreen/" | wc -l)" 1

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

# --- the home screen (UI=home, the default) ------------------------------------

export TEST_UI=home
GAME=/userdata/roms/gba/Golden_Sun.gba

echo "home screen: boot without a cartridge"
NewSandbox
mkdir -p "$R/userdata/roms/gba" "$R/sys/class/graphics/fb0" "$R/dev"
echo x > "$R$GAME"
# A tiny console framebuffer still showing "the splash"
echo 4,3 > "$R/sys/class/graphics/fb0/virtual_size"
echo 32 > "$R/sys/class/graphics/fb0/bits_per_pixel"
head -c 48 /dev/urandom | tr '\0' x > "$R/dev/fb0"
StartTestSession
Check "the home screen starts" WaitFor 10 HomeStartedAtLeast 1
Check "mode is home" Equal "$(cat "$MODE_FILE")" home
Check "RetroArch isn't started" Equal "$(RaRuns)" 0
Check "the splash is cleared once the home screen is up" WaitFor 8 eval \
	'[ "$(stat -c %s "$R/dev/fb0")" = 48 ] && [ -z "$(tr -d "\\000" < "$R/dev/fb0")" ]'

echo "home screen: play a game from the SD card"
HomeAsks "ACTION=game|ROM=$GAME|LABEL=Golden Sun"
Check "RetroArch plays it" WaitFor 5 RaLastHas "$R$GAME"
Check "with the GBA core" RaLastHas "gpsp_libretro.so"
Check "Close Content returns home (RETRONSQ_CLOSE_QUITS=1)" Equal "$(tail -n 1 "$R/ra-env")" "close=1"
Check "mode is game" Equal "$(cat "$MODE_FILE")" game
Check "the request is used up" test ! -f "$RUN_DIR/home-request"

echo "home screen: the reset button restarts the same game"
RUNS=$(RaRuns)
"$LIB_DIR/reset-btn"
Check "the same game again" WaitFor 6 RaStartedAtLeast $((RUNS + 1))
Check "not the home screen" Equal "$(HomeRuns)" 1
Check "same game" RaLastHas "$R$GAME"

echo "home screen: leaving the game"
kill $(cat "$R/fake-ra.pid")
Check "back to the home screen" WaitFor 5 HomeStartedAtLeast 2

echo "home screen: refuses what isn't a game"
for bad in /etc/passwd /userdata/../etc/passwd /userdata/retronsq.cfg; do
	HomeAsks "ACTION=game|ROM=$bad|LABEL=x"
done
Check "three refused requests, back home each time" WaitFor 8 HomeStartedAtLeast 5
Check "RetroArch never ran them" eval '! grep -q "passwd\|retronsq.cfg" "$R/ra-calls"'
Check "logged" Contains "$LOG_DIR/launcher.log" "home screen: not a game file: /etc/passwd"

echo "home screen: RetroArch's settings"
HomeAsks "ACTION=retroarch"
Check "RetroArch's menu" WaitFor 5 RaLastHas "--menu"
Check "where Close Content behaves as usual" Equal "$(tail -n 1 "$R/ra-env")" "close="
kill $(cat "$R/fake-ra.pid")
Check "then home again" WaitFor 5 HomeStartedAtLeast 6

echo "home screen: a cartridge inserted (auto-load on)"
LoadMedium Pokemon_Emerald.gba 262144 131072
CartEvent add
Check "the home screen is stopped" WaitFor 5 Contains "$R/home-events" "TERM"
Check "the cartridge plays" WaitFor 8 RaLastHas "Pokemon_Emerald.gba"
Check "and is in Recently played" Contains "$R/userdata/home/history.txt" "/userdata/rom/Pokemon_Emerald.gba"
HOMES=$(HomeRuns)
kill $(cat "$R/fake-ra.pid")
Check "leaving its game goes home, not into the game again" WaitFor 5 HomeStartedAtLeast $((HOMES + 1))
usleep 1500000
Check "still home" eval 'HomeRunning && ! RaLastHas "Pokemon_Emerald.gba"'

echo "home screen: Play cartridge"
HomeAsks "ACTION=cart"
Check "the cartridge plays again" WaitFor 8 RaLastHas "Pokemon_Emerald.gba"
kill $(cat "$R/fake-ra.pid")
Check "home again" WaitFor 5 HomeRunning

echo "home screen: restart and power off"
HomeAsks "ACTION=reboot"
Check "restart" WaitFor 5 Contains "$R/calls.log" "reboot"
HomeAsks "ACTION=poweroff"
Check "power off" WaitFor 5 Contains "$R/calls.log" "poweroff"

echo "home screen: auto-load off"
NewSandbox
SetCfg CART_AUTOLOAD 0
StartTestSession
Check "the home screen starts" WaitFor 10 HomeStartedAtLeast 1
LoadMedium Tetris.gb 32768
CartEvent add
usleep 1500000
Check "an inserted cartridge leaves it alone" eval 'HomeRunning && [ ! -s "$R/home-events" ]'

Done
