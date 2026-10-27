#!/bin/sh
#
# First boot: S21mountall.sh's grow_userdata_part stretches the userdata
# partition (the last one) to the end of the SD card. This runs the real
# function under the console's busybox, with the real sgdisk, on a card
# image with the firmware's partition layout, through a fake /dev and /sys
# (paths rewritten into a sandbox). Needs sgdisk (gdisk package).
# Run by tests/run-all.sh.
#
. "$(dirname "$0")/../scripts/sandbox.sh"

MOUNTALL="$SRC/../rockchip/partinit/S21mountall.sh"
MiB=2048	# sectors

# MakeCard SIZE_MIB [EXTRA]: a GPT card image like the firmware's SD image
# (loader area, uboot, trust, misc, boot, rootfs, then userdata last), as
# fake device nodes and sysfs entries. EXTRA=1 adds a partition after
# userdata, which must stop the grow.
MakeCard(){
	G="$SQ_ROOT"
	mkdir -p "$G/dev/block/by-name" "$G/sys/class/block" "$G/usr/sbin" "$G/sys/devices/mmc/mmcblk0"
	truncate -s 128M "$G/card.img"
	sgdisk -o -a 1 \
		-n 1:16384:24575 -c 1:uboot \
		-n 2:24576:32767 -c 2:trust \
		-n 3:32768:40959 -c 3:misc \
		-n 4:40960:106495 -c 4:boot \
		-n 5:106496:131071 -c 5:rootfs \
		-n 6:131072:0 -c 6:userdata -t 6:EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 \
		"$G/card.img" > /dev/null
	if [ "$2" = 1 ]; then
		sgdisk -a 1 -d 6 -n 6:131072:+48M -c 6:userdata -n 7:0:0 -c 7:extra "$G/card.img" > /dev/null
	fi
	truncate -s "$1M" "$G/card.img"
	FakeSysfs
	ln -sf ../card.img "$G/dev/mmcblk0"
	ln -sf ../../mmcblk0p6 "$G/dev/block/by-name/userdata"
	# partx: tell "the kernel" (sysfs) about the new table
	cat > "$G/usr/sbin/partx" <<-EOF
	#!$BUSYBOX sh
	echo "partx \$*" >> "$G/calls.log"
	. "$G/fake-sysfs.sh"
	EOF
	ln -sf "$(command -v sgdisk)" "$G/usr/sbin/sgdisk"
	chmod +x "$G/usr/sbin/partx"
}

# FakeSysfs: mmcblk0 and its partitions in sysfs, read from the image
FakeSysfs(){
	cat > "$G/fake-sysfs.sh" <<-EOF
	echo \$((\$(stat -c %s "$G/card.img") / 512)) > "$G/sys/devices/mmc/mmcblk0/size"
	ln -sfn ../../devices/mmc/mmcblk0 "$G/sys/class/block/mmcblk0"
	sgdisk -p "$G/card.img" | awk '/^ +[0-9]+ +[0-9]+ +[0-9]+/ { print \$1, \$2, \$3 - \$2 + 1 }' |
	while read -r n start size; do
		d="$G/sys/devices/mmc/mmcblk0/mmcblk0p\$n"
		mkdir -p "\$d"
		echo \$start > "\$d/start"; echo \$size > "\$d/size"; echo \$n > "\$d/partition"
		ln -sfn ../../devices/mmc/mmcblk0/mmcblk0p\$n "$G/sys/class/block/mmcblk0p\$n"
		touch "$G/dev/mmcblk0p\$n"
	done
	EOF
	PATH="$BASE_PATH" . "$G/fake-sysfs.sh"
}

# RunGrow: grow_userdata_part from S21mountall.sh, its paths moved into the
# sandbox ("-b", block device, becomes "-e": the fake nodes are files)
RunGrow(){
	sed -n '/^grow_userdata_part()/,/^}/p' "$MOUNTALL" | sed \
		-e "s|/usr/sbin/|$G/usr/sbin/|g" \
		-e "s|/dev/|$G/dev/|g" -e "s|/sys/|$G/sys/|g" \
		-e 's|\[ ! -b |[ ! -e |' > "$G/grow.sh"
	echo grow_userdata_part >> "$G/grow.sh"
	PATH="$BASE_PATH" "$BUSYBOX" sh "$G/grow.sh" > "$G/grow.log" 2>&1
}

Part6(){
	sgdisk -i 6 "$G/card.img" | sed -n "s/^$1: \([^ ]*\).*/\1/p"
}

echo "128 MiB image on a 512 MiB card"
NewSandbox
MakeCard 512
UUID=$(Part6 "Partition unique GUID")
RunGrow
Check "grown" Contains "$G/grow.log" "grow: done, userdata is now 447 MiB"
Check "same start" Equal "$(Part6 "First sector")" 131072
Check "ends at the end of the card" Equal "$(Part6 "Last sector")" $((512 * MiB - 34))
Check "same GUID (the rootfs finds userdata by it)" Equal "$(Part6 "Partition unique GUID")" "$UUID"
Check "same name" eval 'sgdisk -i 6 "$G/card.img" | grep -q "^Partition name: .userdata.$"'
Check "same type" Equal "$(Part6 "Partition GUID code")" EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
Check "valid GPT" eval 'sgdisk -v "$G/card.img" | grep -q "No problems found"'
Check "the kernel was told" Contains "$G/calls.log" "partx -u -n 6"
RunGrow
Check "second boot: nothing to do" Contains "$G/grow.log" "grow: userdata already fills"

echo "128 GiB card"
NewSandbox
MakeCard $((128 * 1024))
RunGrow
Check "grown to the end" Equal "$(Part6 "Last sector")" $((128 * 1024 * MiB - 34))

echo "card the same size as the image"
NewSandbox
MakeCard 128
RunGrow
Check "nothing to do" Contains "$G/grow.log" "grow: userdata already fills"

echo "a partition after userdata"
NewSandbox
MakeCard 512 1
RunGrow
Check "left alone" Contains "$G/grow.log" "comes after userdata, not growing"

# RunResizeFat IMAGE: S21mountall.sh's fat_fs_size and resize_fat on an
# image, as the console's busybox runs them. expand_fat and mkfs.fat are
# replaced by stubs that say what would happen (formatting needs root).
RunResizeFat(){
	{
		sed -n '/^le_uint()/,/^}/p; /^fat_fs_size()/,/^}/p; /^resize_fat()/,/^}/p' "$MOUNTALL"
		echo 'expand_fat(){ echo "EXPAND $1 $2"; }'
		echo 'echo "SIZE=$(fat_fs_size $1)"'
		echo 'resize_fat $1 /userdata userdata $(stat -c %s $1)'
	} > "$G/resize.sh"
	mkdir -p "$G/stubbin"
	printf '#!%s sh\necho "MKFS $*"\n' "$BUSYBOX" > "$G/stubbin/mkfs.fat"
	chmod +x "$G/stubbin/mkfs.fat"
	PATH="$G/stubbin:$BASE_PATH" "$BUSYBOX" sh "$G/resize.sh" "$1" > "$G/resize.log" 2>&1
}

echo "FAT32 expand (resize_fat)"
NewSandbox
G="$SQ_ROOT"
truncate -s 64M "$G/fat.img"
PATH=/sbin:/usr/sbin:$PATH mkfs.fat -F 32 -n RETRONSQ "$G/fat.img" > /dev/null
RunResizeFat "$G/fat.img"
Check "boot sector read with the console's tools" Contains "$G/resize.log" "SIZE=67108864"
Check "same size as its partition: left alone" eval '! grep -q "EXPAND\|MKFS" "$G/resize.log"'
truncate -s 512M "$G/fat.img"
RunResizeFat "$G/fat.img"
Check "partition grown: the filesystem is expanded" Contains "$G/resize.log" "EXPAND $G/fat.img RETRONSQ"
Check "sizes logged" Contains "$G/resize.log" "fat: $G/fat.img filesystem 67108864 bytes, partition 536870912 bytes"
truncate -s 0 "$G/blank.img"; truncate -s 64M "$G/blank.img"
RunResizeFat "$G/blank.img"
Check "a blank partition is formatted" Contains "$G/resize.log" "MKFS -F 32 -n RETRONSQ $G/blank.img"
head -c 65536 /dev/urandom > "$G/junk.img"; truncate -s 64M "$G/junk.img"
RunResizeFat "$G/junk.img"
Check "an unreadable partition is never formatted" eval '! grep -q "EXPAND\|MKFS" "$G/resize.log"'
truncate -s 0 "$G/big.img"; truncate -s 300M "$G/big.img"
PATH=/sbin:/usr/sbin:$PATH mkfs.fat -F 32 -n MYCARD "$G/big.img" > /dev/null
truncate -s 1G "$G/big.img"
RunResizeFat "$G/big.img"
Check "a big FAT (made on a PC) is never rebuilt" eval '! grep -q "EXPAND\|MKFS" "$G/resize.log"'
Check "and that is logged" Contains "$G/resize.log" "bigger than the image's filesystem"

# --- The expansion itself, for real ------------------------------------------
#
# expand_fat as the console runs it (its busybox tar, du, find, df; the real
# mkfs.fat and fsck.fat), on FAT images. Mounting needs root, so mount and
# umount are stubs that move the files in and out of the image with mtools.

# mtools checks disk geometry, which plain image files don't have
export MTOOLS_SKIP_CHECK=1

# MakeUserdata PARTITION_MIB: the 64M FAT the SD card image is flashed with,
# holding 40 MB of files (more than v0.3.1's 32 MB limit), in a partition of
# PARTITION_MIB
MakeUserdata(){
	F="$G/files"
	rm -rf "$F" "$G/userdata.img"
	mkdir -p "$F/roms/gba" "$F/saves/Golden_Sun" "$F/retroarch/autoconfig"
	head -c 12582912 /dev/urandom > "$F/roms/gba/Golden Sun.gba"
	head -c 16777216 /dev/urandom > "$F/roms/gba/Pokemon - Emerald Version.gba"
	head -c 12582912 /dev/urandom > "$F/roms/gba/Metroid Fusion.gba"
	head -c 131072 /dev/urandom > "$F/saves/Golden_Sun/cartridge.sav"
	echo "CART_AUTOLOAD=0" > "$F/retronsq.cfg"
	echo 'input_driver = "udev"' > "$F/retroarch/autoconfig/Hyperkin_SQ.cfg"
	truncate -s 64M "$G/userdata.img"
	PATH=/sbin:/usr/sbin:$PATH mkfs.fat -F 32 -n RETRONSQ "$G/userdata.img" > /dev/null
	mcopy -s -i "$G/userdata.img" "$F"/* ::/
	truncate -s "$1M" "$G/userdata.img"
}

# Mount and umount stubs on mtools: mount copies the image's files into the
# folder; umount writes the folder back into a freshly formatted (empty)
# image, then empties the folder
FatStubs(){
	mkdir -p "$G/fatbin"
	cat > "$G/fatbin/mount" <<-EOF
	#!$BUSYBOX sh
	dev=\$3 dir=\$4
	echo "\$dir \$dev" >> "$G/mounted"
	mcopy -s -p -n -i "\$dev" "::*" "\$dir/" 2> /dev/null
	exit 0
	EOF
	cat > "$G/fatbin/umount" <<-EOF
	#!$BUSYBOX sh
	dir=\$1
	dev=\$(awk -v d="\$dir" '\$1 == d { x = \$2 } END { print x }' "$G/mounted")
	if [ -z "\$(mdir -b -i "\$dev" ::/ 2> /dev/null)" ] && [ -n "\$(ls -A "\$dir")" ]; then
		mcopy -s -p -i "\$dev" "\$dir"/* ::/
	fi
	rm -rf "\$dir"/*
	exit 0
	EOF
	if [ "$LOW_MEMORY" = 1 ]; then
		printf '#!%s sh\necho "Filesystem 1K-blocks Used Available Use%% Mounted on"\necho "tmpfs 20000 10000 10000 50%% /tmp"\n' "$BUSYBOX" > "$G/fatbin/df"
	else
		rm -f "$G/fatbin/df"
	fi
	chmod +x "$G/fatbin/"*
}

# RunExpand IMAGE: S21mountall.sh's resize_fat (and expand_fat) on IMAGE
RunExpand(){
	FatStubs
	{
		sed -n '/^le_uint()/,/^}/p; /^fat_fs_size()/,/^}/p; /^expand_fat()/,/^}/p; /^resize_fat()/,/^}/p' "$MOUNTALL"
		echo 'resize_fat $1 /userdata userdata $(stat -c %s $1)'
	} > "$G/expand.sh"
	PATH="$G/fatbin:/sbin:/usr/sbin:$BASE_PATH" "$BUSYBOX" sh "$G/expand.sh" "$1" > "$G/expand.log" 2>&1
}

# FsSize IMAGE: the FAT filesystem's size in MiB, from its boot sector
FsSize(){
	echo $(( $(hexdump -s 32 -n 4 -e '1/4 "%u"' "$1") * 512 / 1048576 ))
}

# SameFiles: the image holds exactly the files MakeUserdata put in
SameFiles(){
	rm -rf "$G/out"; mkdir -p "$G/out"
	mcopy -s -n -i "$G/userdata.img" "::*" "$G/out/" 2> /dev/null
	diff -r "$G/files" "$G/out" > /dev/null
}

for PART in 128 512; do
	echo "first boot: the 64M image with 40 MB of games, on a $PART MiB partition"
	NewSandbox
	G="$SQ_ROOT"
	MakeUserdata $PART
	RunExpand "$G/userdata.img"
	Check "expanded" Contains "$G/expand.log" "expand: done"
	Check "the filesystem fills the partition" eval '[ $(FsSize "$G/userdata.img") -ge $(($PART - 1)) ]'
	Check "every file kept, unchanged" SameFiles
	Check "named RETRONSQ" eval 'mlabel -s -i "$G/userdata.img" :: | grep -q RETRONSQ'
	Check "a valid filesystem" eval 'PATH=/sbin:/usr/sbin:$PATH fsck.fat -n "$G/userdata.img" > /dev/null'
	RunExpand "$G/userdata.img"
	Check "next boot: nothing to do" eval '! grep -q "Expanding" "$G/expand.log"'
done

echo "not enough memory to keep the files"
NewSandbox
G="$SQ_ROOT"
MakeUserdata 512
LOW_MEMORY=1 RunExpand "$G/userdata.img"
Check "left as it is" Contains "$G/expand.log" "not enough memory"
Check "files untouched" SameFiles
Check "still 64M" Equal "$(FsSize "$G/userdata.img")" 64

Done
