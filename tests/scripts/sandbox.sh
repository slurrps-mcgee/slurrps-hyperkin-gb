# Shared helpers for the script tests (tests/scripts/test_*.sh).
#
# Each test builds a sandbox: a directory standing in for "/" on the
# console, with the firmware scripts installed where they are on the device,
# a fake cartridge reader in a fake sysfs, a fake /proc/mounts, and stubs
# for the commands that need real hardware (stubs/: mount, umount, hdparm,
# amixer, retroarch, sqload). The firmware scripts find it through
# $SQ_ROOT, exactly as on the console with an empty $SQ_ROOT.
#
# Expected environment (set by tests/run-all.sh):
#   BUSYBOX   busybox built with the console's config
#   SQCART    sqcart built for the host
#   SRC       buildroot/package/retronsq
#   BASE_PATH busybox applets (console tools) first, then the host's
#
# Sourced by the tests, which run under that busybox's ash.

# Every test lives one level under tests/
STUBS="$(cd "$(dirname "$0")/.." && pwd)/scripts/stubs"
FAILS=0
CHECKS=0

# --- Assertions ----------------------------------------------------------------

# Check DESCRIPTION COMMAND...: run COMMAND, count a failure if it fails
Check(){
	local what="$1"
	shift
	CHECKS=$((CHECKS + 1))
	if "$@"; then
		echo "  ok   $what"
	else
		FAILS=$((FAILS + 1))
		echo "  FAIL $what"
	fi
}

# Equal A B
Equal(){
	[ "$1" = "$2" ] && return 0
	echo "       expected '$2', got '$1'"
	return 1
}

# Contains FILE TEXT: FILE has a line containing TEXT
Contains(){
	grep -qF -- "$2" "$1" 2> /dev/null && return 0
	echo "       '$2' not in $1:"
	sed 's/^/         | /' "$1" 2> /dev/null | tail -n 15
	return 1
}

# WaitFor SECONDS COMMAND...: poll COMMAND until it succeeds or time is up
WaitFor(){
	local tenths=$(($1 * 10))
	shift
	while [ $tenths -gt 0 ]; do
		"$@" > /dev/null 2>&1 && return 0
		usleep 100000
		tenths=$((tenths - 1))
	done
	"$@"
}

# Done: print the result and exit with it
Done(){
	Cleanup
	echo "$(basename "$0"): $((CHECKS - FAILS))/$CHECKS passed"
	[ $FAILS = 0 ]
}

# --- The sandbox -----------------------------------------------------------------

# NewSandbox: fresh $SQ_ROOT with the firmware scripts and an SD card
NewSandbox(){
	[ -n "$SQ_ROOT" ] && Cleanup
	SQ_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/sqtest.XXXXXX")
	export SQ_ROOT
	export SQ_CONSOLE="$SQ_ROOT/console.log"
	export SQCART SQLOAD="$SQ_ROOT/bin/sqload" RETROARCH_BIN="$SQ_ROOT/bin/retroarch"
	local d
	for d in bin proc tmp run/retronsq media/cart usr/lib/retronsq \
			usr/lib/libretro usr/share/retronsq userdata/rom userdata/roms \
			userdata/retroarch/cores fakedev sys/bus/usb/devices sys/class/block; do
		mkdir -p "$SQ_ROOT/$d"
	done
	: > "$SQ_ROOT/proc/mounts"

	# The scripts, installed as on the console, run by the console's shell
	for f in common.sh sq-session sq-scan-roms cart-event reset-btn; do
		sed "1s|^#!/bin/sh|#!$BUSYBOX sh|" "$SRC/scripts/$f" > "$SQ_ROOT/usr/lib/retronsq/$f"
		chmod +x "$SQ_ROOT/usr/lib/retronsq/$f"
	done
	# The stubs come first on PATH, then the console's busybox applets
	for f in "$STUBS"/*; do
		sed "1s|^#!/bin/sh|#!$BUSYBOX sh|" "$f" > "$SQ_ROOT/bin/${f##*/}"
		chmod +x "$SQ_ROOT/bin/${f##*/}"
	done
	export PATH="$SQ_ROOT/bin:$BASE_PATH"
	cp "$SRC/config/retroarch-cfw.cfg" "$SQ_ROOT/usr/share/retronsq/"
	touch "$SQ_ROOT/usr/share/retronsq/cartridge-tools.sqt"
	for core in gpsp mgba vbam sqtools; do
		touch "$SQ_ROOT/usr/lib/libretro/${core}_libretro.so"
	done
	cp "$SRC/../../../device/rockchip/userdata/userdata_brk01/retronsq.cfg" "$SQ_ROOT/userdata/"
	touch "$SQ_ROOT/tmp/boot_complete"
	. "$SQ_ROOT/usr/lib/retronsq/common.sh"
}

# SetCfg KEY VALUE: change a setting in the sandbox's retronsq.cfg
SetCfg(){
	sed -i "/^$1=/d" "$SQ_ROOT/userdata/retronsq.cfg" 2> /dev/null
	echo "$1=$2" >> "$SQ_ROOT/userdata/retronsq.cfg"
}

# Cleanup: stop everything the sandbox started and delete it
Cleanup(){
	[ -n "$SQ_ROOT" ] || return
	local sig
	for sig in TERM KILL; do
		kill -$sig $(SandboxPids) 2> /dev/null
		usleep 200000
	done
	rm -rf "$SQ_ROOT"
	SQ_ROOT=
}

# SandboxPids: every process whose command line mentions the sandbox
SandboxPids(){
	local p
	for p in /proc/[0-9]*; do
		[ "${p#/proc/}" = $$ ] && continue
		case "$(tr '\0' ' ' 2> /dev/null < "$p/cmdline")" in
			*"$SQ_ROOT"*) echo "${p#/proc/}" ;;
		esac
	done
}

# --- The fake cartridge reader ------------------------------------------------------

PORT_BLOCK=sys/bus/usb/devices/3-1/3-1:1.1/host0/target0:0:0/0:0:0:0/block

# PlugReader: the reader shows up on USB with no cartridge (disk size 0)
PlugReader(){
	mkdir -p "$SQ_ROOT/$PORT_BLOCK/sda" "$SQ_ROOT/sys/class/block/sda"
	echo 0 > "$SQ_ROOT/sys/class/block/sda/size"
}

# UnplugReader: the reader disappears from USB
UnplugReader(){
	rm -rf "$SQ_ROOT/sys/bus/usb/devices/3-1" "$SQ_ROOT/sys/class/block/sda"
}

# LoadMedium NAME.EXT ROM_SIZE [SAVE_SIZE] [SEED]: put a cartridge in the
# reader (without telling anyone: send the udev event with CartEvent). The
# medium is the whole disk, like the Retron SQ's reader. Writes the .crc the
# reader would expose.
LoadMedium(){
	local dev="$SQ_ROOT/fakedev/sda" base=${1%.*}
	PlugReader
	rm -rf "$dev"
	mkdir -p "$dev"
	head -c "$2" /dev/urandom > "$dev/$1"
	[ "${3:-0}" -gt 0 ] && head -c "$3" /dev/urandom > "$dev/$base.sav"
	CrcFile "$dev/$1" "$dev/$base.crc"
	echo $((($2 + 511) / 512 + 64)) > "$SQ_ROOT/sys/class/block/sda/size"
}

# EjectMedium: the cartridge is pulled, the reader stays connected
EjectMedium(){
	rm -rf "$SQ_ROOT/fakedev/sda"
	echo 0 > "$SQ_ROOT/sys/class/block/sda/size"
}

# CrcFile ROM OUT: the reader's .crc file for ROM (big-endian CRC-32)
CrcFile(){
	local crc
	crc=$("$SQCART" crc "$1")
	printf "\\$(printf %o $((crc >> 24 & 255)))\\$(printf %o $((crc >> 16 & 255)))\\$(printf %o $((crc >> 8 & 255)))\\$(printf %o $((crc & 255)))" > "$2"
}

# CartEvent ACTION [MEDIA_CHANGE]: what udev runs for the reader's disk
CartEvent(){
	DISK_MEDIA_CHANGE=$2 "$SQ_ROOT/usr/lib/retronsq/cart-event" "$1" sda
}

# Mounted / NotMounted: is something mounted at /media/cart
Mounted(){
	grep -q " $SQ_ROOT/media/cart " "$SQ_ROOT/proc/mounts"
}

NotMounted(){
	! Mounted
}

# --- The fake RetroArch -----------------------------------------------------------------

# RaRuns: how many times RetroArch was started
RaRuns(){
	grep -c . "$SQ_ROOT/ra-calls" 2> /dev/null || echo 0
}

# RaLast: the arguments of the last RetroArch start
RaLast(){
	tail -n 1 "$SQ_ROOT/ra-calls" 2> /dev/null
}

# RaRunning: the fake RetroArch is running now
RaRunning(){
	[ -f "$SQ_ROOT/fake-ra.pid" ] && kill -0 $(cat "$SQ_ROOT/fake-ra.pid") 2> /dev/null
}

# RaStartedAtLeast N: RetroArch has been started N times or more, and runs
RaStartedAtLeast(){
	[ $(RaRuns) -ge $1 ] && RaRunning
}

# RaLastHas TEXT: the running RetroArch was started with TEXT in its args
RaLastHas(){
	RaRunning && RaLast | grep -qF -- "$1"
}
