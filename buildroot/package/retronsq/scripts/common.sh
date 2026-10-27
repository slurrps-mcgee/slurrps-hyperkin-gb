# Retron SQ firmware: shared shell functions.
#
# Installed as /usr/lib/retronsq/common.sh and sourced (". common.sh") by
# sq-session, sq-scan-roms, cart-event, reset-btn, S50ui and drm-hotplug.sh.
# It only defines variables and functions; nothing runs when it is sourced.
#
# How a game gets on screen:
#   - The cartridge slot is a small USB reader. cart-event mounts a
#     cartridge read-only at /media/cart (ROM, .sav save, .crc checksum).
#   - The ROM is copied to the SD card (/userdata/rom) once, by sqload, and
#     RetroArch runs that copy with a libretro emulator core.
#   - With no cartridge, RetroArch starts in its menu, so games stored on
#     the SD card (/userdata/roms) can be picked.
#   - The cartridge is only read: its block device is set read-only and it
#     is mounted read-only. Saves live on the SD card. (The one exception is
#     Cartridge Tools' "Write a save to the cartridge", see cart-event.)
#
# Cartridge work (copying, CRC, saves, backups) is done by the sqcart command
# (libsqcart, the same code as sqload and Cartridge Tools); this file is the
# glue around it.
#
# Only sq-session starts RetroArch. Everything else (cartridge in/out, reset
# button, HDMI hotplug) only *stops* it, and sq-session's loop decides what
# runs next. That keeps two games from ever starting at once.
#
# Every path below starts with $SQ_ROOT, which is empty on the console and
# points at a sandbox in tests (tests/session).

R="${SQ_ROOT:-}"

# --- Paths -------------------------------------------------------------------

READER_DIR="$R/media/cart"                     # the cartridge, read-only
ROM_PATH="$R/userdata/rom/"                    # cartridge copies + live saves (.srm)
SD_ROM_PATH="$R/userdata/roms/"                # the user's own ROMs (gb/gbc/gba)
LOG_DIR="$R/userdata/logs"                     # logs readable on a PC
RETROARCH_BIN="${RETROARCH_BIN:-/usr/bin/retroarch}"
RETROARCH_CON="$R/userdata/retroarch/retroarch.cfg"
# Settings the firmware needs, loaded on top of RETROARCH_CON
RETROARCH_CFW_CON="${SQ_SHARE:-$R/usr/share/retronsq}/retroarch-cfw.cfg"
DEFAULTS_CFG="${SQ_SHARE:-$R/usr/share/retronsq}/defaults.cfg"  # every setting's default
BUILTIN_CORE_DIR="$R/usr/lib/libretro"         # cores built into the firmware
USER_CORE_DIR="$R/userdata/retroarch/cores"    # cores on the SD card (+ copies of the above)
DEFAULTS_DIR="$R/usr/share/retronsq/userdata"  # factory copy of the SD card's files
LIB_DIR="${SQ_LIB:-$R/usr/lib/retronsq}"       # these scripts
SQ_CFG="$R/userdata/retronsq.cfg"              # user settings (see that file)
CART_OVERRIDES="$R/userdata/cart-overrides.txt" # per-cartridge core choice
MGBA_LIST="$R/userdata/gb.txt"                 # Hyperkin's list of games that need mGBA
SQCART="${SQCART:-/usr/bin/sqcart}"
SQLOAD="${SQLOAD:-/usr/bin/sqload}"
SQHOME="${SQHOME:-/usr/bin/sqhome}"

# --- Runtime state (tmpfs, gone after a reboot) ------------------------------

RUN_DIR="$R/run/retronsq"
SESSION_PID="$RUN_DIR/session.pid"            # pid of the running sq-session
SESSION_LOCK="$RUN_DIR/session.lock"          # held by sq-session for its whole life
MODE_FILE="$RUN_DIR/mode"                     # "cart" or "menu": what RetroArch runs now
REDUMP_FLAG="$RUN_DIR/redump"                 # reset: copy the cartridge again
CART_FAILED="$RUN_DIR/cart-failed"            # cartridge failed to load: show the menu
PLAY_FLAG="$RUN_DIR/play-cart"                # Cartridge Tools / home: play the cartridge now
CART_PLAYED="$RUN_DIR/cart-played"            # this insert already auto-started
HOME_REQUEST="$RUN_DIR/home-request"          # what sqhome asked for (see RunHomeRequest)
REPLAY_FLAG="$RUN_DIR/replay"                 # reset button: start the same game again
WRITE_CHECK="$RUN_DIR/write-check.txt"        # verdict on a save written to the cartridge
RA_LOG="$RUN_DIR/retroarch.log"               # RetroArch's log for the current run
HOME="$R/userdata"                            # RetroArch expands "~" in its config to this

mkdir -p "$RUN_DIR" 2> /dev/null

# Seconds since boot, for log lines
Uptime(){
	cut -d ' ' -f 1 /proc/uptime
}

# Whole seconds since power-on (tests start the clock at $SQ_BOOT_T0)
BootSeconds(){
	local up
	up=$(Uptime)
	echo $((${up%.*} - ${SQ_BOOT_T0:-0}))
}

# Write a line to the serial console (and stderr in tests)
Log(){
	echo "sq: $*" > "${SQ_CONSOLE:-/dev/console}" 2> /dev/null
}

# Also keep the line in logs/launcher.log on the SD card, so problems can be
# read on a PC. The file is trimmed to its last 256 KB when it passes 512 KB.
# Synced at once: the console is switched off by pulling the power, and the
# last lines before a problem are the ones that matter. (Few lines per game,
# so the cost is small; tests skip it.)
LogSD(){
	Log "$*"
	mountpoint -q "$R/userdata" 2> /dev/null || [ -n "$SQ_ROOT" ] || return 0
	mkdir -p "$LOG_DIR"
	echo "[$(Uptime)] $*" >> "$LOG_DIR/launcher.log"
	if [ "$(stat -c %s "$LOG_DIR/launcher.log" 2> /dev/null || echo 0)" -gt 524288 ]; then
		tail -c 262144 "$LOG_DIR/launcher.log" > "$LOG_DIR/launcher.log.tmp" && \
			mv "$LOG_DIR/launcher.log.tmp" "$LOG_DIR/launcher.log"
	fi
	[ -n "$SQ_ROOT" ] || sync
}

# --- Settings ----------------------------------------------------------------

# CfgGet KEY: value of KEY from retronsq.cfg, or its default below. The file
# is edited on PCs, so tolerate Windows (CRLF) line endings, spaces around
# "=" and quotes. If a key appears twice the last one wins.
CfgGet(){
	local val
	val=$(tr -d '\r' 2> /dev/null < "$SQ_CFG" | sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" | tail -n 1 | tr -d "\"' \t")
	echo "${val:-$(CfgDefault "$1")}"
}

# A setting's default, from defaults.cfg: the one place defaults are
# defined (libsqcart reads the same file; retronsq.cfg documents them)
CfgDefault(){
	sed -n "s/^$1=//p" "$DEFAULTS_CFG" 2> /dev/null | tail -n 1
}

# CfgNum KEY MAX: a numeric setting, clamped to MAX; the default if the
# value isn't a whole number
CfgNum(){
	local val
	val=$(CfgGet "$1")
	case "$val" in
		''|*[!0-9]*) val=$(CfgDefault "$1") ;;
	esac
	[ "$val" -gt "$2" ] && val=$2
	echo "$val"
}

# ListLookup NAME FILE: look up a ROM file name in a list file whose lines
# are either "name" or "name=value" ("#" lines are comments). Prints the
# value ("1" for a bare name), or nothing when NAME isn't listed. Used for
# cart-overrides.txt and gb.txt.
ListLookup(){
	tr -d '\r' 2> /dev/null < "$2" | awk -v n="$1" '
		/^[ \t]*#/ { next }
		{
			k = $0; v = "1"
			eq = index($0, "=")
			if (eq) { k = substr($0, 1, eq - 1); v = substr($0, eq + 1) }
			gsub(/^[ \t]+|[ \t]+$/, "", k); gsub(/[ \t"]/, "", v)
			if (k == n) found = v
		}
		END { if (found != "") print found }'
}

# --- Hardware state ----------------------------------------------------------

# CartInfo KEY: one field of "sqcart info" (ROM, BASE, SYSDIR, CODE...)
CartInfo(){
	"$SQCART" info 2> /dev/null | sed -n "s/^$1=//p"
}

# Name of the cartridge's ROM file ("Pokemon_Emerald.gba"), or nothing.
# Only a mounted cartridge whose device is still there counts (see libsqcart:
# a hot-swap can leave a stale mount behind).
CheckROM(){
	CartInfo ROM
}

# True when a cartridge is mounted and readable (sqcart info succeeds)
CartPresent(){
	"$SQCART" info > /dev/null 2>&1
}

# True when a screen is plugged in. RetroArch can't start without one.
DisplayConnected(){
	[ -n "$SQ_ROOT" ] && [ ! -e "$R/sys/class/drm" ] && return 0
	grep -qx connected "$R"/sys/class/drm/card0-*/status 2>/dev/null
}

# Boot: keep the splash up until we know whether a cartridge is in, so an
# inserted cartridge starts straight away and the menu never shows first.
# The Retron SQ's reader only appears on USB once it has a cartridge and has
# read it, which can be well after this script starts. So the clock is the
# time since power-on, not since this started:
#   ready      a cartridge is mounted: done at once
#   pending    the reader has a cartridge that isn't mounted yet: wait (up
#              to 15 s past the deadline), mounting it ourselves once a
#              second in case its udev event came before anything could
#              handle it
#   none/empty no cartridge *yet*: only final once CART_WAIT seconds have
#              passed since power-on (default 15)
# Each change of state is logged with its time since power-on, so
# launcher.log shows how long the reader takes, to tune CART_WAIT.
WaitForReader(){
	local tenths=0 deadline now state last=""
	deadline=$(CfgNum CART_WAIT 120)
	while :; do
		state=$("$SQCART" reader-state 2> /dev/null)
		if [ "$state" != "$last" ]; then
			LogSD "boot: cartridge reader $state at $(Uptime) s"
			last=$state
		fi
		now=$(BootSeconds)
		case "$state" in
			ready) break ;;
			pending)
				[ $((tenths % 10)) = 5 ] && "$LIB_DIR/cart-event" reconcile 2> /dev/null
				[ $now -ge $((deadline + 15)) ] && break
				;;
			*)
				[ $now -ge $deadline ] && break
				;;
		esac
		usleep 100000
		tenths=$((tenths + 1))
	done
	LogSD "boot: cartridge reader $state after $((tenths / 10)).$((tenths % 10)) s of waiting"
}

# AnyAlive PID...: true while at least one of the given processes exists
AnyAlive(){
	local pid
	for pid; do
		kill -0 $pid 2> /dev/null && return 0
	done
	return 1
}

# Stop RetroArch (or the home screen) if it is running and wait for it to
# exit. One SIGTERM is
# a normal quit in RetroArch 1.7.4: its video driver sees the signal, the
# run loop ends, and the game's save is written. Exactly one: a second
# SIGTERM makes RetroArch exit(1) on the spot, without saving. SIGKILL
# only if it is still running after 6 s. Only these pids are waited on:
# sq-session may already be starting the next RetroArch.
#
# (Not RetroArch's stdin commands: this build has no networking, and
# without it 1.7.4 neither parses them nor makes stdin non-blocking, so a
# stdin that never ends freezes RetroArch. RunRetroArch gives it /dev/null.)
KillGame(){
	local i=0 pids
	pids=$(pidof retroarch sqhome)
	if [ -n "$pids" ]; then
		kill $pids 2> /dev/null
		while AnyAlive $pids && [ $i -lt 60 ]; do
			usleep 100000
			i=$((i + 1))
		done
		if AnyAlive $pids; then
			LogSD "RetroArch didn't quit within 6 s, killing it"
			kill -9 $pids 2> /dev/null
		fi
	fi
	sync
}

# --- Choosing an emulator core -----------------------------------------------

# ValidCoreName NAME: a short name ("mgba", "gambatte") or an absolute path
# inside one of the core folders. Settings come from files edited on PCs, so
# anything else is refused rather than used in paths and playlists.
ValidCoreName(){
	case "$1" in
		"") return 1 ;;
		*..*) return 1 ;;
		"$USER_CORE_DIR"/*_libretro.so|"$BUILTIN_CORE_DIR"/*_libretro.so) return 0 ;;
		*[!A-Za-z0-9_-]*) return 1 ;;
	esac
	return 0
}

# ResolveCore NAME: print the full path of a core given its short name
# ("mgba" -> .../mgba_libretro.so) or full path. Cores dropped into
# retroarch/cores on the SD card win over the built-in ones. Prints nothing
# when the core doesn't exist or the name isn't valid.
ResolveCore(){
	local dir
	if ! ValidCoreName "$1"; then
		Log "ignoring invalid core name '$1'"
		return
	fi
	case "$1" in
		/*) [ -f "$1" ] && echo "$1"; return ;;
	esac
	for dir in "$USER_CORE_DIR" "$BUILTIN_CORE_DIR"; do
		if [ -f "$dir/$1_libretro.so" ]; then
			echo "$dir/$1_libretro.so"
			return
		fi
	done
}

# SystemCore gb|gbc|gba: path of the default core for a system, from
# retronsq.cfg (GB_CORE/GBC_CORE/GBA_CORE). If the configured core doesn't
# exist, fall back to the stock choice (gpSP for GBA, VBA-M otherwise).
SystemCore(){
	local core key=GB_CORE
	[ "$1" = gba ] && key=GBA_CORE
	[ "$1" = gbc ] && key=GBC_CORE
	core=$(ResolveCore "$(CfgGet $key)")
	if [ -z "$core" ]; then
		Log "core for $1 not found, using $(CfgDefault $key)"
		core=$(ResolveCore "$(CfgDefault $key)")
	fi
	echo "$core"
}

# RomSystem FILE: which system a ROM file name is for (gb, gbc or gba)
RomSystem(){
	case "$1" in
		*.gba|*.GBA) echo gba ;;
		*.gbc|*.GBC) echo gbc ;;
		*) echo gb ;;
	esac
}

# CoreForRom FILE: path of the core to run a cartridge ROM with. In order:
# cart-overrides.txt, then gb.txt (games known to need mGBA), then the
# system default from retronsq.cfg.
CoreForRom(){
	local core
	core=$(ListLookup "$1" "$CART_OVERRIDES")
	[ -z "$core" ] && [ -n "$(ListLookup "$1" "$MGBA_LIST")" ] && core=mgba
	[ -n "$core" ] && core=$(ResolveCore "$core")
	[ -n "$core" ] || core=$(SystemCore $(RomSystem "$1"))
	echo "$core"
}

# --- Getting the cartridge onto the SD card ----------------------------------

# PrepareCart: make sure /userdata/rom holds a good copy of the inserted
# cartridge. Uses the copy from an earlier play when the cartridge didn't
# change (sqcart cache-valid), otherwise runs sqload (loading screen +
# copy). Then imports the cartridge's save on its first play and backs it
# up in saves/<game>/ when it differs from earlier backups. Returns
# non-zero when there is no usable copy (cartridge pulled, unreadable...).
PrepareCart(){
	local name verify="" rc line
	name=$(CheckROM)
	[ -n "$name" ] || return 1
	mkdir -p "$ROM_PATH"

	# Reset button with RESET_REDUMP=1: forget the copy so it is read again
	if [ -f "$REDUMP_FLAG" ]; then
		rm -f "$REDUMP_FLAG" "$ROM_PATH$name" "$ROM_PATH${name%.*}.crc"
	fi

	# A save written to this cartridge earlier (Cartridge Tools): now that
	# it was put back in, is it really on it? The home screen shows the
	# answer next time it opens.
	if "$SQCART" write-check > "$WRITE_CHECK" 2> /dev/null; then
		while read -r line; do
			LogSD "  write check: $line"
		done < "$WRITE_CHECK"
	else
		rm -f "$WRITE_CHECK"
	fi

	[ "$(CfgGet VERIFY_CACHE)" = 1 ] && verify=--verify
	if "$SQCART" cache-valid $verify; then
		Log "$name unchanged, skipping cartridge copy"
		"$SQCART" import-save
	else
		LogSD "copying cartridge $name"
		# sqload's messages (timings, checksum, screen) go to launcher.log
		"$SQLOAD" > "$RUN_DIR/sqload.log" 2>&1
		rc=$?
		while read -r line; do
			LogSD "  sqload: ${line#sq: }"
		done < "$RUN_DIR/sqload.log"
		if [ $rc != 0 ]; then
			LogSD "copying $name failed (sqload exit status $rc)"
			return 1
		fi
	fi
	"$SQCART" backup-save > /dev/null
	sync
	[ -f "$ROM_PATH$name" ]
}

# --- Starting RetroArch ------------------------------------------------------

# RunRetroArch ARGS...: run RetroArch in the foreground with the SD card's
# config plus the firmware's required settings (retroarch-cfw.cfg).
# Returns when RetroArch exits (quit from its menu, or stopped by KillGame).
# Its stdin is /dev/null (see KillGame). The log goes to $RA_LOG; VERBOSE=1
# makes it detailed.
RunRetroArch(){
	local verbose="" close=""
	[ "$(CfgGet VERBOSE)" = 1 ] && verbose=-v
	# With the home screen, Close Content in a game goes back to it (a
	# normal quit, see RetroArch patch 0004) instead of RetroArch's menu
	[ "$1" != --menu ] && [ "$(CfgGet UI)" != retroarch ] && close=1
	RETRONSQ_CLOSE_QUITS=$close HOME=$HOME "$RETROARCH_BIN" $verbose -c "$RETROARCH_CON" \
		--appendconfig="$RETROARCH_CFW_CON" "$@" \
		< /dev/null > "$RA_LOG" 2>&1
}

# Keep RetroArch's log of the last cartridge run on the SD card
SaveRetroArchLog(){
	mkdir -p "$LOG_DIR"
	tail -c 262144 "$RA_LOG" > "$LOG_DIR/retroarch.log" 2> /dev/null
}

# LogSaveState WHEN NAME CODE: record the cartridge save and the live save
# (sizes, checksums, and a Pokémon save check) in launcher.log, before and
# after a run. Comparing the two tells whether saves reach the SD card, and
# whether the cartridge's save was complete.
LogSaveState(){
	local base=${2%.*} f label
	for label in cartridge live; do
		f="$READER_DIR/$base.sav"
		[ $label = live ] && f="$ROM_PATH$base.srm"
		[ -f "$f" ] || { LogSD "  $1 $label save: none"; continue; }
		LogSD "  $1 $label save: $(stat -c %s "$f") bytes, md5 $(md5sum "$f" | cut -c 1-12)"
		"$SQCART" check-save "$f" "$3" | grep "^GEN3" | while read -r line; do
			LogSD "    $line"
		done
	done
	grep " $R/userdata " /proc/mounts 2> /dev/null | while read -r line; do
		LogSD "  userdata: $line"
	done
}

# Run the copied cartridge with its core. Call PrepareCart first.
LaunchCart(){
	local name core code
	name=$(CheckROM)
	code=$(CartInfo CODE)
	core=$(CoreForRom "$name")
	LogSD "starting $name (title '$(CartInfo TITLE)', code '$code') with ${core##*/}"
	LogSaveState before "$name" "$code"
	echo cart > "$MODE_FILE"
	touch "$CART_PLAYED"
	# Recently played on the home screen: the cartridge's copy, playable
	# without it
	"$SQCART" history-add "/userdata/rom/$name" "$(CartLabel)" > /dev/null 2>&1
	RunRetroArch -L "$core" "$ROM_PATH$name"
	LogSD "RetroArch exited ($name)"
	LogSaveState after "$name" "$code"
	SaveRetroArchLog
}

# The inserted cartridge's name for lists: its header title, else its file
# name with spaces ("Pokemon_Emerald" -> "Pokemon Emerald")
CartLabel(){
	local title
	title=$(CartInfo TITLE)
	[ -n "$title" ] || title=$(CartInfo BASE | tr '_' ' ')
	echo "$title"
}

# The home screen (UI=home): runs until the user picks something, then
# that is started (RunHomeRequest). sq-session comes back here when it ends.
LaunchHome(){
	echo home > "$MODE_FILE"
	LogSD "home screen"
	rm -f "$HOME_REQUEST"
	"$SQHOME" > "${SQ_CONSOLE:-/dev/console}" 2>&1
	RunHomeRequest
}

# HomeRequest KEY: a value from sqhome's request file
HomeRequest(){
	sed -n "s/^$1=//p" "$HOME_REQUEST" 2> /dev/null | head -n 1
}

# Do what the home screen asked for:
#   game       play ROM= (a file under /userdata) with its system's core
#   cart       play the cartridge (sq-session's next pass)
#   retroarch  RetroArch's own menu (its settings)
#   reboot, poweroff
# Nothing (it was stopped, or crashed): sq-session decides what's next.
RunHomeRequest(){
	local action rom
	action=$(HomeRequest ACTION)
	rom=$(HomeRequest ROM)
	rm -f "$HOME_REQUEST"
	# For sq-session's back-off: the home screen did its job
	[ -n "$action" ] && HOME_ACTED=1
	case "$action" in
		game) PlayFromHome "$rom" ;;
		cart) touch "$PLAY_FLAG" ;;
		retroarch)
			echo menu > "$MODE_FILE"
			RunRetroArch --menu
			;;
		reboot|poweroff)
			LogSD "$action, from the home screen"
			sync
			$action
			;;
	esac
}

# PlayFromHome ROM: play the game the home screen asked for. The path
# comes from files on the SD card (history, favorites), so only a ROM file
# under /userdata is accepted. The core is picked like for the Collections:
# a cartridge copy (rom/) gets its cartridge's core, a library game its
# system's.
PlayFromHome(){
	local rom="$1" core
	case "$rom" in
		*..*|*"$(printf '\t')"*) rom="" ;;
		/userdata/*.gb|/userdata/*.gbc|/userdata/*.gba|/userdata/*.zip) ;;
		/userdata/*.GB|/userdata/*.GBC|/userdata/*.GBA|/userdata/*.ZIP) ;;
		*) rom="" ;;
	esac
	if [ -z "$rom" ] || [ ! -f "$R$rom" ]; then
		LogSD "home screen: not a game file: $1"
		return
	fi
	case "$rom" in
		/userdata/rom/*) core=$(CoreForRom "${rom##*/}") ;;
		*) core=$(SystemCore "$(RomSystem "$rom")") ;;
	esac
	echo game > "$MODE_FILE"
	LogSD "starting $rom from the home screen with ${core##*/}"
	while :; do
		rm -f "$REPLAY_FLAG"
		RunRetroArch -L "$core" "$R$rom"
		# The reset button asks for the same game again
		[ -f "$REPLAY_FLAG" ] || break
	done
	LogSD "RetroArch exited ($rom)"
}

# Open the RetroArch menu (no game loaded), after refreshing the game lists
# shown under Load Content > Collections
LaunchMenu(){
	echo menu > "$MODE_FILE"
	"$LIB_DIR/sq-scan-roms" > "${SQ_CONSOLE:-/dev/console}" 2>&1
	RunRetroArch --menu
}

# --- SD card upkeep ----------------------------------------------------------

# Run once per boot by sq-session:
#   1. Put back any factory file missing from the SD card. Files that exist
#      are never overwritten, so the user's edits are kept.
#   2. Copy the built-in cores into retroarch/cores, so RetroArch's
#      "Load Core" lists them next to the user's own cores. Only redone when
#      the firmware's cores change (tracked with a checksum stamp file).
SyncUserdata(){
	local stamp so
	# Never write into the read-only rootfs if userdata didn't mount
	[ -n "$SQ_ROOT" ] || mountpoint -q /userdata || return

	if [ -d "$DEFAULTS_DIR" ]; then
		(cd "$DEFAULTS_DIR" && find . -type d) | while read -r dir; do
			mkdir -p "$R/userdata/$dir"
		done
		(cd "$DEFAULTS_DIR" && find . -type f) | while read -r file; do
			[ -e "$R/userdata/$file" ] || cp "$DEFAULTS_DIR/$file" "$R/userdata/$file"
		done
	fi

	stamp=$(ls -l "$BUILTIN_CORE_DIR" | cksum)
	if [ "$(cat "$USER_CORE_DIR/.builtin-stamp" 2>/dev/null)" != "$stamp" ]; then
		Log "installing built-in cores to $USER_CORE_DIR"
		mkdir -p "$USER_CORE_DIR"
		for so in "$BUILTIN_CORE_DIR"/*_libretro.so; do
			cp "$so" "$USER_CORE_DIR/${so##*/}.tmp" && \
				mv "$USER_CORE_DIR/${so##*/}.tmp" "$USER_CORE_DIR/${so##*/}"
		done
		echo "$stamp" > "$USER_CORE_DIR/.builtin-stamp"
	fi
	sync
}

# --- The screen ------------------------------------------------------------------

# BlankFramebuffer: clear the console framebuffer (fb0) to black. The boot
# splash is drawn there (S50ui, fbv), and the display falls back to it
# whenever no program holds the screen: between the loading screen and the
# game, or a game and the home screen. Cleared once the first program is up,
# the splash only shows at power-on.
BlankFramebuffer(){
	local size bpp
	size=$(cat "$R/sys/class/graphics/fb0/virtual_size" 2> /dev/null) || return 0
	bpp=$(cat "$R/sys/class/graphics/fb0/bits_per_pixel" 2> /dev/null) || return 0
	dd if=/dev/zero of="$R/dev/fb0" bs=$((${size%,*} * bpp / 8)) count=${size#*,} \
		conv=notrunc 2> /dev/null
}

# --- Audio -------------------------------------------------------------------

# Send the internal codec's output to the headphone jack when headphones are
# plugged in, else turn it off while HDMI is connected (HDMI carries the
# audio). Used by S50ui at boot and drm-hotplug.sh on HDMI changes.
AudioRoute(){
	local path=HP
	if [ "$(cat "$R/sys/class/switch/h2w/state" 2> /dev/null)" != 2 ] && \
			[ "$(cat "$R/sys/class/drm/card0-HDMI-A-1/status" 2> /dev/null)" = connected ]; then
		path=OFF
	fi
	amixer cset name='Playback Path' "$path" > /dev/null 2>&1
}

# --- The session process -----------------------------------------------------

# True while sq-session runs (its pid file points at a live process)
SessionRunning(){
	[ -f "$SESSION_PID" ] && kill -0 $(cat "$SESSION_PID") 2>/dev/null
}

# Start sq-session in the background if it isn't running. Safe to call from
# anywhere, any number of times.
StartSession(){
	SessionRunning && return
	# flock keeps a second session from starting if two callers race;
	# setsid detaches it from the udev/init script that started it
	setsid flock -n "$SESSION_LOCK" "$LIB_DIR/sq-session" < /dev/null \
		> "${SQ_CONSOLE:-/dev/console}" 2>&1 &
}

# Stop sq-session and whatever game it is running (used at shutdown)
StopSession(){
	if SessionRunning; then
		kill $(cat "$SESSION_PID")
	fi
	rm -f "$SESSION_PID"
	KillGame
}

# --- Event handlers ----------------------------------------------------------

# cart-event, cartridge inserted. With CART_AUTOLOAD=1 (default) stop
# whatever runs (even a game from the SD card); sq-session then loads the
# cartridge. With CART_AUTOLOAD=0 nothing changes: the cartridge waits in
# Cartridge Tools (Play cartridge).
# "CartInserted swapped": one cartridge replaced another in a single event.
# Also does what CartRemoved does for the old one (a game from it stops even
# with auto-load off), stopping RetroArch only once: a second KillGame could
# hit the RetroArch sq-session starts for the new cartridge.
CartInserted(){
	local stop=0
	if [ "$1" = swapped ]; then
		LogSD "cartridge swapped"
		killall sqload 2> /dev/null
		rm -f "$PLAY_FLAG"
		[ "$(cat "$MODE_FILE" 2>/dev/null)" = cart ] && stop=1
	fi
	rm -f "$CART_FAILED" "$CART_PLAYED"
	LogSD "cartridge inserted: $(CheckROM)"
	[ "$(CfgGet CART_AUTOLOAD)" = 1 ] && stop=1
	[ $stop = 1 ] && KillGame
	[ -f "$R/tmp/boot_complete" ] && StartSession
}

# cart-event, cartridge removed: abort a copy in progress, and stop
# RetroArch only if it was running the cartridge (an SD card game keeps
# going). sq-session then returns to the menu.
CartRemoved(){
	LogSD "cartridge removed"
	killall sqload 2> /dev/null
	rm -f "$CART_FAILED" "$PLAY_FLAG" "$CART_PLAYED"
	[ "$(cat "$MODE_FILE" 2>/dev/null)" = cart ] && KillGame
}

# Reset button: restart the game (or menu). With RESET_REDUMP=1 a cartridge
# is copied again first. Ignored while a cartridge is being copied.
ResetROM(){
	if pidof sqload > /dev/null; then
		Log "reset ignored while the cartridge is being copied"
		return
	fi
	case "$(cat "$MODE_FILE" 2>/dev/null)" in
		cart)
			[ "$(CfgGet RESET_REDUMP)" = 1 ] && touch "$REDUMP_FLAG"
			touch "$PLAY_FLAG"	# the same cartridge again
			;;
		game)
			touch "$REPLAY_FLAG"	# the same SD card game again
			;;
	esac
	rm -f "$CART_FAILED"
	KillGame
}
