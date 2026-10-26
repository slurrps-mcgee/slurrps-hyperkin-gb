# Shared helpers for the cartridge reader and the game launcher.
#
# Sourced by S50ui, sq-launcher, sq-scan-roms, the usbmount hooks,
# reset-btn.sh and drm-hotplug.sh. Only sq-launcher ever starts RetroArch;
# everything else just stops it and lets the launcher decide what runs next.

DUMPER_PATH="/media/usb0/"
ROM_PATH="/userdata/rom/"
SD_ROM_PATH="/userdata/roms/"
RETROARCH_BIN="/usr/bin/retroarch"
RETROARCH_CON="/userdata/retroarch/retroarch.cfg"
BUILTIN_CORE_DIR="/usr/lib/libretro"
USER_CORE_DIR="/userdata/retroarch/cores"
DEFAULTS_DIR="/usr/share/retronsq/userdata"
SQ_CFG="/userdata/retronsq.cfg"
CART_OVERRIDES="/userdata/cart-overrides.txt"
MGBA_LIST="/userdata/gb.txt"
LAUNCHER_PID="/var/run/sq-launcher.pid"
LAUNCHER_LOCK="/var/run/sq-launcher.lock"
MODE_FILE="/tmp/sq-mode"
REDUMP_FLAG="/tmp/sq-redump"
CART_FAILED="/tmp/sq-cart-failed"
HOME="/userdata"

Log(){
	echo "sq: $*" > /dev/console
}

# Value of KEY from retronsq.cfg, or $2. The file is edited on PCs, so
# tolerate CRLF line endings, spaces around "=" and quotes.
CfgGet(){
	local val
	val=$(tr -d '\r' < $SQ_CFG 2>/dev/null | sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" | tail -n 1 | tr -d "\"' \t")
	echo "${val:-$2}"
}

# Look up a ROM file name in a "name" or "name=value" list file and print
# the value (or "1" for a bare name). Prints nothing when it isn't listed.
ListLookup(){
	tr -d '\r' < "$2" 2>/dev/null | awk -v n="$1" '
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

# Name of the ROM file the cartridge reader exposes, if any
CheckROM(){
	local file
	for file in ${DUMPER_PATH}*.gb*
	do
		if [ -f "${file}" ]; then
			echo "${file##*/}"
			break;
		fi
	done
}

CartPresent(){
	mountpoint -q ${DUMPER_PATH%/} && [ -n "$(CheckROM)" ]
}

DisplayConnected(){
	grep -qx connected /sys/class/drm/card0-*/status 2>/dev/null
}

AnyAlive(){
	local pid
	for pid; do
		kill -0 $pid 2> /dev/null && return 0
	done
	return 1
}

KillGame(){
	local i=0 pids
	pids=$(pidof retroarch)
	if [ -n "$pids" ]; then
		# SIGTERM lets RetroArch flush the save file before it exits. Only
		# wait on these pids: sq-launcher may already be starting the next
		# RetroArch, and that one must not be killed.
		kill $pids
		while AnyAlive $pids && [ $i -lt 30 ]; do
			usleep 100000
			i=$((i + 1))
		done
		AnyAlive $pids && kill -9 $pids 2> /dev/null
	fi
	/bin/sync
}

# Full path of a core given its short name ("mgba") or a path. Cores dropped
# into retroarch/cores on the SD card win over the built-in ones.
ResolveCore(){
	local dir
	case "$1" in
		/*) [ -f "$1" ] && echo "$1"; return ;;
	esac
	for dir in $USER_CORE_DIR $BUILTIN_CORE_DIR; do
		if [ -f "$dir/$1_libretro.so" ]; then
			echo "$dir/$1_libretro.so"
			return
		fi
	done
}

# Default core for a system (gb, gbc or gba) from retronsq.cfg
SystemCore(){
	local core fallback=vbam
	[ "$1" = gba ] && fallback=gpsp
	case "$1" in
		gba) core=$(CfgGet GBA_CORE gpsp) ;;
		gbc) core=$(CfgGet GBC_CORE vbam) ;;
		*)   core=$(CfgGet GB_CORE vbam) ;;
	esac
	core=$(ResolveCore "$core")
	if [ -z "$core" ]; then
		Log "core for $1 not found, using $fallback"
		core=$(ResolveCore $fallback)
	fi
	echo "$core"
}

RomSystem(){
	case "$1" in
		*.gba|*.GBA) echo gba ;;
		*.gbc|*.GBC) echo gbc ;;
		*) echo gb ;;
	esac
}

# Core for a cartridge ROM: cart-overrides.txt first, then gb.txt (games
# known to need mGBA), then the system default from retronsq.cfg
CoreForRom(){
	local core
	core=$(ListLookup "$1" $CART_OVERRIDES)
	[ -z "$core" ] && [ -n "$(ListLookup "$1" $MGBA_LIST)" ] && core=mgba
	[ -n "$core" ] && core=$(ResolveCore "$core")
	[ -n "$core" ] || core=$(SystemCore $(RomSystem "$1"))
	echo "$core"
}

# Saves on the cartridge only seed the cache; after that the .srm in
# /userdata/rom is the live save (same rule as hyperkin-loading).
ImportCartSave(){
	if [ -f "${DUMPER_PATH}$1.sav" ] && [ ! -f "${ROM_PATH}$1.srm" ]; then
		cp "${DUMPER_PATH}$1.sav" "${ROM_PATH}$1.srm.tmp" && \
			mv "${ROM_PATH}$1.srm.tmp" "${ROM_PATH}$1.srm"
	fi
}

# The cached copy is only kept with a .crc after hyperkin-loading verified
# it, so a matching CRC and size means the cartridge did not change.
CartCacheValid(){
	local name=$1 base=${1%.*}
	[ -f "${ROM_PATH}${name}" ] || return 1
	[ -f "${ROM_PATH}${base}.crc" ] && [ -f "${DUMPER_PATH}${base}.crc" ] || return 1
	cmp -s "${DUMPER_PATH}${base}.crc" "${ROM_PATH}${base}.crc" || return 1
	[ "$(stat -c %s "${DUMPER_PATH}${name}")" = "$(stat -c %s "${ROM_PATH}${name}")" ] || return 1
	if [ "$(CfgGet VERIFY_CACHE 0)" = 1 ]; then
		/usr/bin/hyperkin-crc "${ROM_PATH}${name}" > /dev/console || return 1
	fi
	return 0
}

# Make sure /userdata/rom holds a good copy of the inserted cartridge
PrepareCart(){
	local name base
	name=$(CheckROM)
	[ -n "$name" ] || return 1
	base=${name%.*}
	mkdir -p ${ROM_PATH}

	if [ -f $REDUMP_FLAG ]; then
		rm -f $REDUMP_FLAG "${ROM_PATH}${name}" "${ROM_PATH}${base}.crc"
	fi

	if CartCacheValid "$name"; then
		Log "${name} unchanged, skipping cartridge copy"
		ImportCartSave "$base"
	else
		Log "loading:${ROM_PATH}${name}"
		/usr/bin/hyperkin-loading > /dev/console 2>&1 || return 1
	fi
	/bin/sync
	[ -f "${ROM_PATH}${name}" ]
}

RunRetroArch(){
	if [ "$(CfgGet VERBOSE 1)" = 1 ]; then
		HOME=$HOME $RETROARCH_BIN -v -c $RETROARCH_CON "$@" > /dev/console 2>&1
	else
		HOME=$HOME $RETROARCH_BIN -c $RETROARCH_CON "$@" > /dev/null 2>&1
	fi
}

LaunchCart(){
	local name core
	name=$(CheckROM)
	core=$(CoreForRom "$name")
	Log "starting ${ROM_PATH}${name} with ${core##*/}"
	echo cart > $MODE_FILE
	RunRetroArch -L "$core" "${ROM_PATH}${name}"
}

LaunchMenu(){
	echo menu > $MODE_FILE
	/usr/bin/sq-scan-roms > /dev/console 2>&1
	RunRetroArch --menu
}

# Put back any default file missing from the SD card (never overwrites),
# and copy the built-in cores next to user cores so RetroArch's
# "Load Core" lists all of them.
SyncUserdata(){
	local stamp so
	# Never write into the read-only rootfs if userdata didn't mount
	mountpoint -q /userdata || return

	if [ -d $DEFAULTS_DIR ]; then
		(cd $DEFAULTS_DIR && find . -type d) | while read dir; do
			mkdir -p "/userdata/$dir"
		done
		(cd $DEFAULTS_DIR && find . -type f) | while read file; do
			[ -e "/userdata/$file" ] || cp "$DEFAULTS_DIR/$file" "/userdata/$file"
		done
	fi

	stamp=$(ls -l $BUILTIN_CORE_DIR | cksum)
	if [ "$(cat $USER_CORE_DIR/.builtin-stamp 2>/dev/null)" != "$stamp" ]; then
		Log "installing built-in cores to $USER_CORE_DIR"
		mkdir -p $USER_CORE_DIR
		for so in $BUILTIN_CORE_DIR/*_libretro.so; do
			cp "$so" "$USER_CORE_DIR/${so##*/}.tmp" && \
				mv "$USER_CORE_DIR/${so##*/}.tmp" "$USER_CORE_DIR/${so##*/}"
		done
		echo "$stamp" > $USER_CORE_DIR/.builtin-stamp
	fi
	/bin/sync
}

LauncherRunning(){
	[ -f $LAUNCHER_PID ] && kill -0 $(cat $LAUNCHER_PID) 2>/dev/null
}

StartLauncher(){
	LauncherRunning && return
	# flock keeps a second launcher from starting if two callers race
	setsid flock -n $LAUNCHER_LOCK /usr/bin/sq-launcher < /dev/null > /dev/console 2>&1 &
}

StopLauncher(){
	if LauncherRunning; then
		kill $(cat $LAUNCHER_PID)
	fi
	rm -f $LAUNCHER_PID
	KillGame
}

# usbmount: cartridge inserted. Stop whatever runs, the launcher loads it.
CartInserted(){
	rm -f $CART_FAILED
	KillGame
	[ -f /tmp/boot_complete ] && StartLauncher
}

# usbmount: cartridge removed. Only stop RetroArch if it runs the cartridge.
CartRemoved(){
	killall hyperkin-loading 2> /dev/null
	rm -f $CART_FAILED
	[ "$(cat $MODE_FILE 2>/dev/null)" = cart ] && KillGame
}

# Reset button: restart the game (or menu). Ignored while copying a cartridge.
ResetROM(){
	if pidof hyperkin-loading > /dev/null; then
		Log "reset ignored until hyperkin-loading finished"
		return
	fi
	if [ "$(CfgGet RESET_REDUMP 0)" = 1 ] && [ "$(cat $MODE_FILE 2>/dev/null)" = cart ]; then
		touch $REDUMP_FLAG
	fi
	rm -f $CART_FAILED
	KillGame
}

# Kept for older callers
RunGame(){
	StartLauncher
}
