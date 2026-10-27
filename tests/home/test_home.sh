#!/bin/sh
#
# sqhome, the home screen, run for real without a display (SDL's dummy
# video driver) and driven by scripted button presses (SQ_HOME_SCRIPT, see
# run_script in home.c). Checks what it hands to sq-session and what it
# changes on the SD card: history, favorites, settings, cartridge actions.
# SQTOOLS_SHOTS=DIR keeps its screenshots. Run by tests/run-all.sh
# (SQHOME_BIN: sqhome built for the host).
#
. "$(dirname "$0")/../scripts/sandbox.sh"

HOST_PATH=/usr/local/bin:/usr/bin:/bin

# A sandbox with fonts, a version and some games
NewHomeSandbox(){
	NewSandbox
	REQ="$RUN_DIR/home-request"
	rm -f "$R/tmp/boot_complete"
	mkdir -p "$R/usr/share/fonts/dejavu" "$R/etc" "$R/userdata/roms/gba" "$R/userdata/roms/gb"
	cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
		/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf "$R/usr/share/fonts/dejavu/"
	echo "Retron SQ CFW v0.0.0 test" > "$R/etc/retronsq-version"
	for g in "Advance Wars" "Golden Sun" "Metroid Fusion"; do
		echo x > "$R/userdata/roms/gba/$g.gba"
	done
	echo x > "$R/userdata/roms/gb/Tetris.gb"
}

# RunHome SCRIPT: run the home screen until the script ends or it hands
# something over. The sandbox's stubs (mount, hdparm) come first on its
# PATH, for cart-event write-enable.
RunHome(){
	rm -f "$REQ"
	PATH=$R/bin:$HOST_PATH SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
		SQ_HOME_SCRIPT="$1" SQ_HOME_SHOTS="${SQTOOLS_SHOTS:-}" \
		SQ_HOME_TEST_PAD="${SQ_HOME_TEST_PAD:-}" \
		timeout 30 "$SQHOME_BIN" > "$R/home.log" 2>&1
	RC=$?
	# Sanitizer reports (sqhome is built with ASan/UBSan by run-all.sh)
	if grep -q "ERROR: AddressSanitizer\|runtime error:" "$R/home.log"; then
		{ echo "== $1"; cat "$R/home.log"; } >> "$SAN_LOG"
	fi
}
SAN_LOG=$(mktemp)

Req(){
	sed -n "s/^$1=//p" "$REQ" 2> /dev/null
}

echo "first start: the games"
NewHomeSandbox
RunHome "shot:home-games a down sleep:400 shot:home-list a"
Check "exits normally" Equal "$RC" 0
Check "hands over a game" Equal "$(Req ACTION)" game
Check "the one chosen" Equal "$(Req ROM)" "/userdata/roms/gba/Golden Sun.gba"
Check "with its name" Equal "$(Req LABEL)" "Golden Sun"
Check "now recently played" Contains "$R/userdata/home/history.txt" "/userdata/roms/gba/Golden Sun.gba"

echo "the Hyperkin SQ pad (its RetroArch autoconfig), quick taps"
mkdir -p "$R/userdata/retroarch/autoconfig"
cp "$SRC/../../../device/rockchip/userdata/userdata_brk01/retroarch/autoconfig/Hyperkin_SQ.cfg" \
	"$R/userdata/retroarch/autoconfig/"
# Opens on Recently played: d-pad down (axis 1) twice = Game Boy Advance,
# A (button 7) in, down, A. Each press and its release arrive together.
SQ_HOME_TEST_PAD=11812:18538 RunHome "joy:a1=32767,a1=0 joy:a1=32767,a1=0 joy:b7=1,b7=0 joy:a1=32767,a1=0 joy:b7=1,b7=0"
Check "every tap counts, mapped like RetroArch" Equal "$(Req ROM)" "/userdata/roms/gba/Golden Sun.gba"
SQ_HOME_TEST_PAD=11812:18538 RunHome "joy:a1=32767 sleep:600 joy:a1=0 shot:home-held quit"
Check "a held direction repeats (no crash)" Equal "$RC" 0

echo "favorites"
# Opens on Recently played now: down down = Game Boy Advance
RunHome "down down a y b up a shot:home-favorites quit"
Check "Y marks a favorite" Contains "$R/userdata/home/favorites.txt" "/userdata/roms/gba/Advance Wars.gba"
RunHome "down down a y quit"
Check "Y again unmarks it" eval '! grep -q "Advance Wars" "$R/userdata/home/favorites.txt"'

echo "box art"
mkdir -p "$R/userdata/retroarch/thumbnails/Nintendo - Game Boy Advance/Named_Boxarts"
cp "$SRC/../../../docs/images/boot-splash.png" \
	"$R/userdata/retroarch/thumbnails/Nintendo - Game Boy Advance/Named_Boxarts/Golden Sun.png"
RunHome "down down a down sleep:400 shot:home-boxart quit"
Check "a game with box art shows it" Equal "$RC" 0

echo "settings"
# up up = Settings (Recently played -> Cartridge -> Settings)
RunHome "up up a a down right down right down down down right shot:home-settings quit"
Check "auto-load switched off" Equal "$(sed -n 's/^CART_AUTOLOAD=//p' "$SQ_CFG")" 0
Check "cartridge wait 15 -> 20 s" Equal "$(sed -n 's/^CART_WAIT=//p' "$SQ_CFG")" 20
Check "GBA emulator gpsp -> mgba" Equal "$(sed -n 's/^GBA_CORE=//p' "$SQ_CFG")" mgba
Check "start screen -> RetroArch menu" Equal "$(sed -n 's/^UI=//p' "$SQ_CFG")" retroarch
Check "the rest of the settings file is kept" Contains "$SQ_CFG" "# Retron SQ settings."
RunHome "up up a down down down down down down down a quit"
Check "RetroArch settings" Equal "$(Req ACTION)" retroarch
RunHome "up up a down down down down down down down down down a b quit"
Check "restart asks first (B: no)" test ! -f "$REQ"
RunHome "up up a down down down down down down down down down a a quit"
Check "No is the default button" test ! -f "$REQ"
RunHome "up up a down down down down down down down down down a right a"
Check "restart (Yes)" Equal "$(Req ACTION)" reboot
RunHome "up up a down down down down down down down down down down a right shot:home-poweroff a"
Check "power off" Equal "$(Req ACTION)" poweroff
# Date and time: year up, then Set (a test can't set the clock: not root)
RunHome "up up a down down down down down down a right up shot:home-clock left up a shot:home-clock-set a quit"
Check "date and time: tries to set the clock" Contains "$R/home.log" "setting the clock"
Check "settings are saved before it exits" Equal "$(sed -n 's/^CART_AUTOLOAD=//p' "$SQ_CFG")" 0

echo "the cartridge page"
# With auto-load on, inserting a cartridge stops the home screen (so the
# cartridge starts): this page is for auto-load off
NewHomeSandbox
SetCfg CART_AUTOLOAD 0
LoadMedium Pokemon_LeafGreen.gba 262144 131072
"$LIB_DIR/cart-event" reconcile
WaitFor 3 Contains "$LOG_DIR/launcher.log" "cartridge inserted"
# Opens on the cartridge: A in, down down = Back up save, then Copy
RunHome "shot:home-cart a down down a wait shot:home-backup a up a wait shot:home-copied a quit"
Check "back up the save" test -f "$R/userdata/saves/Pokemon_LeafGreen/001-cartridge.sav"
Check "copy to the SD card" cmp -s "$READER_DIR/Pokemon_LeafGreen.gba" "$R/userdata/roms/gba/Pokemon_LeafGreen.gba"
RunHome "a a"
Check "Play cartridge" Equal "$(Req ACTION)" cart

echo "a save written to the cartridge, checked when it's put back"
mkdir -p "$R/userdata/home"
printf 'ROM=Pokemon_LeafGreen.gba\nROM_SIZE=262144\nSAVE_SIZE=131072\nSAVE_CRC=00000000\n' \
	> "$R/userdata/home/cart-write-check"
RunHome "shot:home-write-check quit"
Check "a slot that didn't store it: said so, and the check is done" \
	test ! -f "$R/userdata/home/cart-write-check"
Check "logged" Contains "$LOG_DIR/launcher.log" "does NOT have the written save"
mkdir -p "$RUN_DIR"
printf 'OK=1\nYour save is on the cartridge.\nThe save written to it earlier was read back\n' > "$RUN_DIR/write-check.txt"
RunHome "shot:home-write-ok quit"
Check "the session's verdict is shown once" test ! -f "$RUN_DIR/write-check.txt"

echo "a game written to the cartridge"
# Down x5 = Write a game to the cartridge: the first game that fits
# (Advance Wars), Yes, then "Write it"
RunHome "a down down down down down a shot:home-write-rom-list a right shot:home-write-rom-confirm a right a wait shot:home-write-rom a quit"
Check "written over the start of the cartridge's game" Equal "$(head -c 2 "$READER_DIR/Pokemon_LeafGreen.gba")" x
Check "the cartridge's file keeps its size" Equal "$(wc -c < "$READER_DIR/Pokemon_LeafGreen.gba" | tr -d ' ')" 262144
Check "read back and logged" Contains "$LOG_DIR/launcher.log" "game written to the cartridge and read back identical"
Check "locked again" Contains "$R/proc/mounts" " $READER_DIR vfat ro"
Check "remembered for the check when it's put back" Contains "$R/userdata/home/cart-write-check" "KIND=rom"
RunHome "shot:home-write-rom-check quit"
Check "checked when it's back: the game is on it" Contains "$LOG_DIR/launcher.log" \
	"the written game is on the cartridge"
Check "and the check is done" test ! -f "$R/userdata/home/cart-write-check"
echo 1 > "$RUN_DIR/reader-wp"
cp "$READER_DIR/Pokemon_LeafGreen.gba" "$R/before.gba"
RunHome "a down down down down down shot:home-write-protected a a right a right a quit"
Check "write protected: writing is greyed out" cmp -s "$READER_DIR/Pokemon_LeafGreen.gba" "$R/before.gba"
Check "(and it was the cartridge page)" eval '! grep -q "request: game" "$R/home.log"'
echo 0 > "$RUN_DIR/reader-wp"

echo "the slot reading a cartridge"
NewHomeSandbox
SetCfg CART_AUTOLOAD 0
LoadMedium Golden_Sun.gba 65536
RunHome "sleep:900 shot:home-reading quit"
Check "shown while it's read (no crash)" Equal "$RC" 0

echo "a cartridge that can't be read"
NewHomeSandbox
SetCfg CART_AUTOLOAD 0
LoadMedium notes.txt 4096
"$LIB_DIR/cart-event" reconcile
RunHome "sleep:2500 shot:home-unreadable quit"
Check "mounted but no game on it: said so" Contains "$LOG_DIR/launcher.log" "has no game on it"
mkdir -p "$RUN_DIR"
printf 'OK=0\nCouldn'"'"'t copy the cartridge.\nreading the cartridge: Input/output error\n' > "$RUN_DIR/cart-error.txt"
RunHome "shot:home-copy-failed quit"
Check "a failed copy is shown, once" test ! -f "$RUN_DIR/cart-error.txt"

echo "options: deleting a game"
NewHomeSandbox
# Opens on Game Boy Advance (nothing played): A in, Select, down down = Delete
RunHome "a select shot:home-options down down a a quit"
Check "asks first, No is the default" test -f "$R/userdata/roms/gba/Advance Wars.gba"
RunHome "a select down down a right shot:home-delete-game a shot:home-deleted a quit"
Check "the game is deleted" test ! -f "$R/userdata/roms/gba/Advance Wars.gba"
Check "the others are kept" test -f "$R/userdata/roms/gba/Golden Sun.gba"
RunHome "a select down a quit"
Check "options: add to favorites" Contains "$R/userdata/home/favorites.txt" "Golden Sun.gba"
RunHome "a select a"
Check "options: play" Equal "$(Req ROM)" "/userdata/roms/gba/Golden Sun.gba"

echo "the saves page"
echo s > "$R/userdata/roms/gba/Golden Sun.srm"
mkdir -p "$R/userdata/saves/Golden_Sun"
echo b > "$R/userdata/saves/Golden_Sun/001-cartridge.sav"
# Opens on Recently played now: down x5 = Saves. The game's save first,
# then its backup.
RunHome "down down down down down a shot:home-saves down a shot:home-save-options a right a a quit"
Check "a backup is deleted (asked once)" test ! -f "$R/userdata/saves/Golden_Sun/001-cartridge.sav"
Check "the game's save is kept" test -f "$R/userdata/roms/gba/Golden Sun.srm"
RunHome "down down down down down a a a right a quit"
Check "a game's save is asked about twice" test -f "$R/userdata/roms/gba/Golden Sun.srm"
RunHome "down down down down down a a a right a right shot:home-delete-save a a quit"
Check "then deleted" test ! -f "$R/userdata/roms/gba/Golden Sun.srm"

echo "memory checks (AddressSanitizer, UBSan)"
Check "no bad memory access or undefined behaviour in any run" eval '! [ -s "$SAN_LOG" ] || { cat "$SAN_LOG"; false; }'
rm -f "$SAN_LOG"

echo "stopped by sq-session"
NewHomeSandbox
PATH=$HOST_PATH SDL_VIDEODRIVER=dummy "$SQHOME_BIN" > "$R/home.log" 2>&1 &
PID=$!
usleep 800000
kill $PID
WAITED=0
while kill -0 $PID 2> /dev/null && [ $WAITED -lt 30 ]; do
	usleep 100000
	WAITED=$((WAITED + 1))
done
Check "one SIGTERM ends it" eval '! kill -0 $PID 2> /dev/null'
Check "without handing anything over" test ! -f "$REQ"

Done
