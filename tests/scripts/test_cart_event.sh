#!/bin/sh
#
# cart-event: the cartridge reader's udev handler. Hot-plug, hot-swap with
# and without the reader reconnecting, stale mounts, and racing events.
# Run by tests/run-all.sh under the console's busybox.
#
. "$(dirname "$0")/sandbox.sh"

# The handlers only start a session after boot; these tests want the
# mounting alone
NewCartSandbox(){
	NewSandbox
	rm -f "$R/tmp/boot_complete"
}

# The reader's change counter (/run/retronsq/cart-generation)
Generation(){
	cat "$RUN_DIR/cart-generation" 2> /dev/null || echo 0
}

# How many mounts there are at /media/cart
MountCount(){
	grep -c " $R/media/cart " "$R/proc/mounts"
}

echo "no reader, empty reader"
NewCartSandbox
"$LIB_DIR/cart-event" reconcile
Check "no reader: state none" Equal "$("$SQCART" reader-state)" none
Check "no reader: nothing mounted" NotMounted
PlugReader
CartEvent add
Check "reader without a cartridge: state empty" Equal "$("$SQCART" reader-state)" empty
Check "reader without a cartridge: nothing mounted" NotMounted
Check "nothing changed: generation untouched" Equal "$(Generation)" 0

echo "insert"
LoadMedium Pokemon_LeafGreen.gba 262144 131072
Check "cartridge in, no event yet: state pending" Equal "$("$SQCART" reader-state)" pending
CartEvent change 1
Check "mounted at /media/cart" Mounted
Check "mounted read-only" Contains "$R/proc/mounts" "vfat ro,noexec,nodev,noatime"
Check "reader set read-only before mounting" Contains "$R/calls.log" "hdparm -r1 /dev/sda"
Check "state ready" Equal "$("$SQCART" reader-state)" ready
Check "the ROM is found" Equal "$(CheckROM)" Pokemon_LeafGreen.gba
Check "generation bumped" Equal "$(Generation)" 1
Check "session told about the insert" WaitFor 3 Contains "$LOG_DIR/launcher.log" "cartridge inserted: Pokemon_LeafGreen.gba"

echo "repeated events"
CartEvent change
CartEvent add
Check "an event without a media change keeps the mount" Equal "$(grep -c '^mount' "$R/calls.log")" 1
Check "generation unchanged" Equal "$(Generation)" 1

echo "hot-swap, reader stays connected (media change)"
EjectMedium
LoadMedium Golden_Sun.gba 131072
CartEvent change 1
Check "new cartridge mounted" Equal "$(CheckROM)" Golden_Sun.gba
Check "old cartridge's files gone" Equal "$(ls "$READER_DIR" | grep -c LeafGreen)" 0
Check "exactly one mount" Equal "$(MountCount)" 1
Check "generation bumped" Equal "$(Generation)" 2
Check "session told about the swap" WaitFor 3 Contains "$LOG_DIR/launcher.log" "cartridge swapped"

echo "pulled, reader stays connected"
EjectMedium
CartEvent change 1
Check "unmounted" NotMounted
Check "state empty" Equal "$("$SQCART" reader-state)" empty
Check "session told about the removal" WaitFor 3 Contains "$LOG_DIR/launcher.log" "cartridge removed"

echo "hot-swap, reader reconnects (remove + add)"
LoadMedium Pokemon_Emerald.gba 262144 131072
CartEvent add
Check "mounted" Equal "$(CheckROM)" Pokemon_Emerald.gba
UnplugReader
CartEvent remove
Check "reader gone: unmounted" NotMounted
LoadMedium Tetris.gb 32768
CartEvent add
Check "next cartridge mounted" Equal "$(CheckROM)" Tetris.gb

echo "stale mount (an event was missed)"
UnplugReader
Check "libsqcart already ignores the stale mount" Equal "$("$SQCART" reader-state)/$(CheckROM)" none/
"$LIB_DIR/cart-event" reconcile
Check "the next reconcile drops it" NotMounted

echo "medium with a partition table"
NewCartSandbox
LoadMedium Metroid_Fusion.gba 65536
mkdir -p "$R/sys/class/block/sda/sda1"
echo 100 > "$R/sys/class/block/sda/sda1/size"
ln -s sda/sda1 "$R/sys/class/block/sda1"
mv "$R/fakedev/sda" "$R/fakedev/sda1"
CartEvent add
Check "first partition mounted" Contains "$R/proc/mounts" "/dev/sda1 $R/media/cart"
Check "the ROM is found" Equal "$(CheckROM)" Metroid_Fusion.gba

echo "unreadable cartridge"
NewCartSandbox
LoadMedium Broken.gba 65536
rm -rf "$R/fakedev/sda"
CartEvent add
Check "mount failure logged" Contains "$LOG_DIR/launcher.log" "couldn't mount /dev/sda"
Check "state stays pending" Equal "$("$SQCART" reader-state)" pending

echo "racing events"
NewCartSandbox
LoadMedium Pokemon_Ruby.gba 131072
for i in 1 2 3 4 5 6; do
	CartEvent add &
done
wait
Check "six events at once: one mount" Equal "$(MountCount)" 1
Check "one generation bump" Equal "$(Generation)" 1

Done
