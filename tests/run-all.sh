#!/bin/bash
#
# Run every firmware test. Normally through the build container, which has
# all the tools (compilers, SDL, Xvfb, sgdisk):
#
#   ./build-in-container.sh test           all of them
#   ./build-in-container.sh test session   only the groups named
#
# Groups:
#   syntax    every shell script parses with the console's busybox sh
#   patches   RetroArch's 000N-*.patch files are applied in its src/
#   build     the retronsq programs build for the host without warnings
#   lib       libsqcart unit tests, also with AddressSanitizer + UBSan
#   tools     Cartridge Tools driven frame by frame (also with sanitizers)
#   config    settings, core choice, playlists (scripts)
#   cartevent hot-plug handler (scripts)
#   session   sq-session end to end (scripts)
#   grow      first-boot partition grow on a GPT image (needs sgdisk)
#   load      loading screen under Xvfb (needs xvfb-run, rsvg-convert)
#
# The scripts run under busybox built from buildroot/dl with the console's
# own config, so they meet the same shell and tools as on the console.
# Build output goes to tests/.build (TEST_BUILD to change it).
# SQTOOLS_SHOTS=DIR keeps the screenshots the tests take.
#
set -u

TOP=$(cd "$(dirname "$0")/.." && pwd)
TESTS="$TOP/tests"
BUILD="${TEST_BUILD:-$TESTS/.build}"
PKG="$TOP/buildroot/package/retronsq"
BB_VERSION=1.27.2
BB_CONFIG="$TOP/buildroot/board/rockchip/common/base/busybox.config"
JOBS=$(nproc 2> /dev/null || echo 2)

ALL="syntax patches build lib tools config cartevent session grow load"
GROUPS_TO_RUN="${*:-$ALL}"
PASSED=""
FAILED=""
SKIPPED=""

Want(){
	[[ " $GROUPS_TO_RUN " == *" $1 "* ]]
}

Result(){
	case $2 in
		0) PASSED="$PASSED $1" ;;
		skip) SKIPPED="$SKIPPED $1" ;;
		*) FAILED="$FAILED $1" ;;
	esac
}

Header(){
	echo
	echo "=== $1"
}

mkdir -p "$BUILD"

# --- busybox with the console's config ----------------------------------

BUSYBOX="$BUILD/busybox/busybox"
BuildBusybox(){
	local stamp
	stamp=$(cat "$BB_CONFIG" | cksum)
	if [ -x "$BUSYBOX" ] && [ "$(cat "$BUILD/busybox/.config-stamp" 2> /dev/null)" = "$stamp" ]; then
		return 0
	fi
	echo "building busybox $BB_VERSION with the console's config..."
	rm -rf "$BUILD/busybox"
	mkdir -p "$BUILD/busybox"
	tar -xjf "$TOP/buildroot/dl/busybox-$BB_VERSION.tar.bz2" -C "$BUILD/busybox" --strip-components=1 || return 1
	cp "$BB_CONFIG" "$BUILD/busybox/.config"
	# Host build: no cross prefix, no sysroot
	sed -i -e 's/^CONFIG_CROSS_COMPILER_PREFIX=.*/CONFIG_CROSS_COMPILER_PREFIX=""/' \
		-e 's/^CONFIG_SYSROOT=.*/CONFIG_SYSROOT=""/' \
		-e 's/^CONFIG_EXTRA_CFLAGS=.*/CONFIG_EXTRA_CFLAGS=""/' "$BUILD/busybox/.config"
	(cd "$BUILD/busybox" && yes "" | make oldconfig > /dev/null && \
		make -j"$JOBS" busybox > build.log 2>&1) || { tail -n 30 "$BUILD/busybox/build.log"; return 1; }
	echo "$stamp" > "$BUILD/busybox/.config-stamp"
}

if ! BuildBusybox; then
	echo "busybox build failed" >&2
	exit 1
fi
mkdir -p "$BUILD/bin"
for applet in $("$BUSYBOX" --list); do
	ln -sf "$BUSYBOX" "$BUILD/bin/$applet"
done
# Console tools first; the host's for what the console doesn't have
export BUSYBOX BASE_PATH="$BUILD/bin:$PATH" SRC="$PKG"

# --- groups -------------------------------------------------------------

if Want syntax; then
	Header "syntax: shell scripts parse under busybox sh"
	rc=0
	for f in "$PKG"/scripts/* "$TOP"/buildroot/board/rockchip/rk3128/fs-overlay/usr/bin/drm-hotplug.sh \
			"$TOP"/buildroot/package/rockchip/partinit/S21mountall.sh "$TESTS"/*/*.sh "$TESTS"/scripts/stubs/*; do
		if "$BUSYBOX" sh -n "$f"; then
			echo "  ok   ${f#$TOP/}"
		else
			echo "  FAIL ${f#$TOP/}"
			rc=1
		fi
	done
	Result syntax $rc
fi

if Want patches; then
	Header "patches: RetroArch's patches are applied in its src/"
	rc=0
	if command -v patch > /dev/null; then
		for p in "$TOP"/buildroot/package/retroarch/0*.patch; do
			if (cd "$TOP/buildroot/package/retroarch/src" && patch -p1 -R --dry-run -s -f < "$p" > /dev/null); then
				echo "  ok   ${p##*/}"
			else
				echo "  FAIL ${p##*/} is not applied in src/"
				rc=1
			fi
		done
		Result patches $rc
	else
		echo "  skipped: no patch command"
		Result patches skip
	fi
fi

# The programs, built out of tree (the package source stays clean)
BuildPrograms(){
	rm -rf "$BUILD/src"
	cp -r "$PKG/src" "$BUILD/src"
	make -C "$BUILD/src" -s clean > /dev/null
	make -C "$BUILD/src" -s CC=gcc AR=ar CFLAGS="-O2 -Werror" "$@"
}

if Want build || Want config || Want cartevent || Want session || Want grow || Want load; then
	Header "build: retronsq programs for the host"
	if BuildPrograms all 2>&1; then
		echo "  ok   sqcart, sqload, sqtools_libretro.so"
		Result build 0
	elif BuildPrograms sqcart 2>&1; then
		# No SDL here: the script tests only need sqcart
		echo "  sqload/sqtools need SDL2 dev packages; built sqcart only"
		Result build skip
	else
		Result build 1
	fi
	export SQCART="$BUILD/src/sqcart" SQLOAD_BIN="$BUILD/src/sqload"
fi

# CTest NAME SOURCE [CFLAGS...]: build a C test with libsqcart's sources
# and run it in its own sandbox, plain and with sanitizers
CTest(){
	local name=$1 src=$2 rc=0 variant flags
	shift 2
	for variant in plain asan; do
		flags=""
		[ $variant = asan ] && flags="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
		if ! gcc -std=gnu99 -Wall -Wno-format-truncation -O1 $flags "$@" \
				-I "$TESTS/lib" -I "$PKG/src/lib" -I "$PKG/src/tools" \
				-o "$BUILD/$name-$variant" "$src" "$PKG"/src/lib/*.c -lpthread 2>&1; then
			if [ $variant = asan ]; then
				echo "  (no sanitizer runtime, skipped the sanitizer run)"
				continue
			fi
			return 1
		fi
		rm -rf "$BUILD/$name-root"
		echo "  $variant:"
		SQ_ROOT="$BUILD/$name-root" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
			"$BUILD/$name-$variant" 2>&1 | grep -v "^sq: " | sed 's/^/    /'
		[ "${PIPESTATUS[0]}" = 0 ] || rc=1
	done
	return $rc
}

if Want lib; then
	Header "lib: libsqcart unit tests"
	CTest test_sqcart "$TESTS/lib/test_sqcart.c"
	Result lib $?
fi

if Want tools; then
	Header "tools: Cartridge Tools"
	CTest test_sqtools "$TESTS/tools/test_sqtools.c" -DSQ_TEST_READ_DELAY_US=20000
	Result tools $?
fi

# ScriptTest GROUP FILE [REQUIRED_COMMAND]: run a test script under busybox
ScriptTest(){
	Want "$1" || return
	Header "$1: ${2#$TESTS/}"
	if [ -n "${3:-}" ] && ! command -v "$3" > /dev/null; then
		echo "  skipped: needs $3"
		Result "$1" skip
		return
	fi
	PATH="$BASE_PATH" timeout 600 "$BUSYBOX" sh "$2"
	Result "$1" $?
}

ScriptTest config "$TESTS/scripts/test_config.sh"
ScriptTest cartevent "$TESTS/scripts/test_cart_event.sh"
ScriptTest session "$TESTS/scripts/test_session.sh"
ScriptTest grow "$TESTS/grow/test_grow.sh" sgdisk
# The loading screen test wants a copy slow enough to see the bar move, as
# from the real slot: its own sqload, with libsqcart's read delay hook
if Want load && [ -x "${SQLOAD_BIN:-}" ]; then
	SQLOAD_BIN="$BUILD/sqload-slow"
	gcc -std=gnu99 -O2 -Wall -Wno-format-truncation -DSQ_TEST_READ_DELAY_US=100000 -DSTALL_MS=3000 \
		-o "$SQLOAD_BIN" "$PKG/src/load/sqload.c" "$PKG"/src/lib/*.c \
		-lSDL2 -lSDL2_image -lSDL2_ttf -lm -lpthread
	ScriptTest load "$TESTS/load/test_sqload.sh" xvfb-run
elif Want load; then
	Header "load: skipped, sqload wasn't built"
	Result load skip
fi

echo
echo "passed: ${PASSED:- none}"
[ -n "$SKIPPED" ] && echo "skipped:$SKIPPED"
if [ -n "$FAILED" ]; then
	echo "FAILED:$FAILED"
	exit 1
fi
echo "all tests passed"
