#!/bin/sh
#
# Settings and core choice (common.sh), and the playlists sq-scan-roms
# writes. Run by tests/run-all.sh under the console's busybox.
#
. "$(dirname "$0")/sandbox.sh"

echo "settings (retronsq.cfg)"
NewSandbox
Check "defaults from the factory file" Equal "$(CfgGet GBA_CORE)/$(CfgGet CART_AUTOLOAD)/$(CfgGet NO_CART)" "gpsp/1/menu"
printf 'GBA_CORE = "mgba"\r\nCART_AUTOLOAD=0\r\n' >> "$SQ_CFG"
Check "CRLF, spaces and quotes from PC editors" Equal "$(CfgGet GBA_CORE)" mgba
Check "last occurrence wins" Equal "$(CfgGet CART_AUTOLOAD)" 0
rm "$SQ_CFG"
Check "defaults without a settings file" Equal "$(CfgGet CART_AUTOLOAD)/$(CfgGet CART_WAIT)" "1/15"
SetCfg CART_WAIT abc
Check "non-numeric number falls back to its default" Equal "$(CfgNum CART_WAIT 120)" 15
SetCfg CART_WAIT 9999
Check "numbers are capped" Equal "$(CfgNum CART_WAIT 120)" 120

echo "core choice"
NewSandbox
Check "system defaults" Equal "$(CoreForRom a.gba) $(CoreForRom b.gbc)" \
	"$BUILTIN_CORE_DIR/gpsp_libretro.so $BUILTIN_CORE_DIR/vbam_libretro.so"
touch "$USER_CORE_DIR/gpsp_libretro.so"
Check "a core on the SD card wins over the built-in one" Equal "$(CoreForRom a.gba)" "$USER_CORE_DIR/gpsp_libretro.so"
rm "$USER_CORE_DIR/gpsp_libretro.so"
echo "Pokemon_LeafGreen.gba=mgba" > "$CART_OVERRIDES"
Check "cart-overrides.txt" Equal "$(CoreForRom Pokemon_LeafGreen.gba)" "$BUILTIN_CORE_DIR/mgba_libretro.so"
echo "Mirumodepon3.gba" > "$MGBA_LIST"
Check "gb.txt means mGBA" Equal "$(CoreForRom Mirumodepon3.gba)" "$BUILTIN_CORE_DIR/mgba_libretro.so"
for bad in '../../bin/sh' 'gp sp' 'x;reboot' '$(reboot)' "$R/tmp/evil_libretro.so"; do
	Check "invalid core name refused: $bad" Equal "$(ResolveCore "$bad")" ""
done
SetCfg GBA_CORE 'nope;reboot'
Check "an invalid configured core falls back to the default" Equal "$(SystemCore gba)" "$BUILTIN_CORE_DIR/gpsp_libretro.so"
SetCfg GBA_CORE missingcore
Check "a missing configured core falls back to the default" Equal "$(SystemCore gba)" "$BUILTIN_CORE_DIR/gpsp_libretro.so"

echo "playlists (sq-scan-roms)"
NewSandbox
mkdir -p "${SD_ROM_PATH}gba" "${SD_ROM_PATH}gb"
touch "${SD_ROM_PATH}gba/Zelda Minish Cap.gba" "${SD_ROM_PATH}gba/Advance Wars.gba" \
	"${SD_ROM_PATH}gb/Tetris.gb" "${SD_ROM_PATH}gba/$(printf 'Tab\tName').gba" \
	"$ROM_PATH/Pokemon_LeafGreen.gba"
"$LIB_DIR/sq-scan-roms" > /dev/null 2>&1
PL="$R/userdata/retroarch/playlists"
Check "a playlist per system" Equal "$(ls "$PL" | tr '\n' ' ')" \
	"Cartridge Dumps.lpl Cartridge Tools.lpl Nintendo - Game Boy Advance.lpl Nintendo - Game Boy.lpl "
Check "6-line entries, sorted by name" Equal "$(sed -n '2p;8p' "$PL/Nintendo - Game Boy Advance.lpl" | tr '\n' '|')" \
	"Advance Wars|Zelda Minish Cap|"
Check "names with a tab are skipped" Equal "$(grep -c . "$PL/Nintendo - Game Boy Advance.lpl")" 12
Check "paths with spaces kept whole" Contains "$PL/Nintendo - Game Boy Advance.lpl" "${SD_ROM_PATH}gba/Zelda Minish Cap.gba"
Check "cartridge dumps listed with their core" Contains "$PL/Cartridge Dumps.lpl" "$BUILTIN_CORE_DIR/gpsp_libretro.so"
Check "playlists hold device paths, not the sandbox's" Contains "$PL/Cartridge Tools.lpl" "/usr/share/retronsq/cartridge-tools.sqt"
BEFORE=$(stat -c %Y "$PL/Nintendo - Game Boy.lpl")
touch -d @1 "$PL/Nintendo - Game Boy.lpl"
"$LIB_DIR/sq-scan-roms" > /dev/null 2>&1
Check "unchanged playlists aren't rewritten" Equal "$(stat -c %Y "$PL/Nintendo - Game Boy.lpl")" 1
rm "${SD_ROM_PATH}gb/Tetris.gb"
"$LIB_DIR/sq-scan-roms" > /dev/null 2>&1
Check "an empty list removes its playlist" Equal "$(ls "$PL/Nintendo - Game Boy.lpl" 2> /dev/null)" ""

echo "audio route"
NewSandbox
mkdir -p "$R/sys/class/drm/card0-HDMI-A-1" "$R/sys/class/switch/h2w"
echo connected > "$R/sys/class/drm/card0-HDMI-A-1/status"
echo 0 > "$R/sys/class/switch/h2w/state"
AudioRoute
Check "HDMI: codec off" Contains "$R/calls.log" "Playback Path OFF"
echo 2 > "$R/sys/class/switch/h2w/state"
AudioRoute
Check "headphones: codec on" Contains "$R/calls.log" "Playback Path HP"

Done
