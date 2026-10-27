#!/bin/sh
#
# sqload, the loading screen: copies a cartridge behind a progress bar
# drawn with SDL, on a virtual X display (Xvfb) instead of the console's
# screen. Checks the copy, the save import and the screenshots it takes at
# 50% and 100% (SQ_LOADING_SNAPSHOT), and that the copy also completes with
# no display at all. Needs xvfb-run, rsvg-convert and the DejaVu fonts.
# Run by tests/run-all.sh (SQLOAD_BIN: sqload built for the host).
#
. "$(dirname "$0")/../scripts/sandbox.sh"

HOST_PATH=/usr/local/bin:/usr/bin:/bin

# Background and fonts where sqload looks for them. Also: no boot
# complete, so mounting the cartridge doesn't start a session that would
# copy it alongside the sqload under test.
SetupScreen(){
	rm -f "$R/tmp/boot_complete"
	mkdir -p "$R/etc" "$R/usr/share/fonts/dejavu"
	sed 's/@VERSION_LABEL@/v0.0.0 test/' "$SRC/splash/retronsq-loading.svg" > "$R/loading.svg"
	PATH=$HOST_PATH rsvg-convert -w 1280 -h 720 -o "$R/etc/retronsq-loading.png" "$R/loading.svg"
	cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
		/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf "$R/usr/share/fonts/dejavu/"
}

# BmpSize FILE: "WIDTHxHEIGHT" from a BMP header
BmpSize(){
	PATH=$HOST_PATH od -A n -t d4 -j 18 -N 8 "$1" | awk '{ print $1 "x" ($2 < 0 ? -$2 : $2) }'
}

echo "loading screen"
NewSandbox
SetupScreen
LoadMedium Pokemon_LeafGreen.gba 3145728 131072
"$LIB_DIR/cart-event" reconcile
PATH=$HOST_PATH SQ_LOADING_SNAPSHOT="$R/shot" xvfb-run -a -s "-screen 0 1280x720x24" \
	"$SQLOAD_BIN" > "$R/sqload.log" 2>&1
Check "exits 0" Equal $? 0
Check "ROM copied intact" cmp -s "$READER_DIR/Pokemon_LeafGreen.gba" "$ROM_PATH/Pokemon_LeafGreen.gba"
Check "verified copy marked with its .crc" cmp -s "$READER_DIR/Pokemon_LeafGreen.crc" "$ROM_PATH/Pokemon_LeafGreen.crc"
Check "save imported" cmp -s "$READER_DIR/Pokemon_LeafGreen.sav" "$ROM_PATH/Pokemon_LeafGreen.srm"
Check "screenshot at 50%" Equal "$(BmpSize "$R/shot-50.bmp")" 1280x720
Check "screenshot at 100%" Equal "$(BmpSize "$R/shot-100.bmp")" 1280x720
Check "the bar moved between them" eval '! cmp -s "$R/shot-50.bmp" "$R/shot-100.bmp"'
Check "drawn on the background, not a blank screen" eval \
	'[ $(PATH=$HOST_PATH od -A n -t u1 -j 54 -N 3000000 "$R/shot-100.bmp" | tr -s " " "\n" | sort -u | wc -l) -gt 50 ]'
[ -n "$SQTOOLS_SHOTS" ] && cp "$R/shot-50.bmp" "$R/shot-100.bmp" "$SQTOOLS_SHOTS/" 2> /dev/null

Check "timings and checksum logged" Contains "$R/sqload.log" "checksum verified"
Check "screen setup logged" Contains "$R/sqload.log" "loading screen up after"

echo "a cartridge that stops answering"
NewSandbox
SetupScreen
LoadMedium Pokemon_Ruby.gba 2097152
"$LIB_DIR/cart-event" reconcile
START=$(date +%s)
PATH=$HOST_PATH SQ_TEST_STALL=1 xvfb-run -a -s "-screen 0 1280x720x24" \
	"$SQLOAD_BIN" > "$R/sqload.log" 2>&1
RC=$?
Check "gives up with a failure" eval '[ $RC -ne 0 ]'
Check "after the stall limit, not never" eval '[ $(($(date +%s) - START)) -lt 15 ]'
Check "says why" Contains "$R/sqload.log" "no data from the cartridge"
Check "no half-copied ROM under the real name" test ! -e "$ROM_PATH/Pokemon_Ruby.gba"

echo "no display"
NewSandbox
SetupScreen
LoadMedium Tetris.gb 65536
"$LIB_DIR/cart-event" reconcile
PATH=$HOST_PATH DISPLAY= SDL_VIDEODRIVER=x11 "$SQLOAD_BIN" > "$R/sqload.log" 2>&1
Check "exits 0" Equal $? 0
Check "the copy still happens" cmp -s "$READER_DIR/Tetris.gb" "$ROM_PATH/Tetris.gb"

echo "no cartridge"
NewSandbox
rm -f "$R/tmp/boot_complete"
PATH=$HOST_PATH DISPLAY= "$SQLOAD_BIN" > "$R/sqload.log" 2>&1
RC=$?
Check "exits non-zero" eval '[ $RC -ne 0 ]'

Done
