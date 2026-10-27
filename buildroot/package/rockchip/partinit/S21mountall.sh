#!/bin/sh
#
# Early boot (S21): get the partitions in /etc/fstab ready and mount them.
#   1. grow_userdata_part: on the first boot from the SD card image, stretch
#      the userdata partition to the end of the card
#   2. resizeall:         grow each filesystem to fill its partition
#   3. checkall:          fsck the filesystems
#   4. mountall:          mount everything
#   5. save_boot_log:     copy this boot's log to logs/boot.log on the card
# then touch /tmp/boot_complete, which the cartridge hooks and HDMI hotplug
# wait for before starting games.

# Uncomment below to see more logs
# set -x

# Grow an ext2/3/4 filesystem to its partition, once (the label marks it done)
resize_e2fs()
{
	DEV=$1
	MOUNT_POINT=$2
	PART_NAME=$3
	LABEL=$(dumpe2fs -h $DEV | grep "volume name:" | grep -o "[^ ]*$")

	[ $? -eq 0 ] || return

	# Not first boot, likely already resized
	[ "$LABEL" == "$PART_NAME" ] && return

	echo Resizing $DEV...

	mountpoint -q $MOUNT_POINT || e2fsck -fy $DEV

	# Force using online resize, see:
	# https://bugs.launchpad.net/ubuntu/+source/e2fsprogs/+bug/1796788.
	TEMP=$(mktemp -d)
	mount $DEV $TEMP
	resize2fs $DEV
	umount $TEMP

	# Use volume name to specify first boot
	tune2fs $DEV -L $PART_NAME
}

# le_uint DEV OFFSET BYTES: little-endian unsigned number at OFFSET in DEV.
# hexdump, because the console's BusyBox od has none of -A/-t/-j/-N
# (CONFIG_DESKTOP is off): v0.1/v0.2 used od here and never expanded the FAT.
le_uint()
{
	hexdump -s $2 -n $3 -e "1/$3 \"%u\"" $1 2>/dev/null
}

# fat_fs_size DEV: print the FAT filesystem size in bytes, or nothing when
# DEV has no FAT boot sector
fat_fs_size()
{
	# Filesystem size in bytes, read straight from the FAT boot sector
	DEV=$1

	SIG=$(hexdump -s 510 -n 2 -e '2/1 "%02x"' $DEV 2>/dev/null)
	[ "$SIG" == "55aa" ] || return

	BPS=$(le_uint $DEV 11 2)
	TOTAL=$(le_uint $DEV 19 2)
	[ "$TOTAL" == "0" ] && TOTAL=$(le_uint $DEV 32 4)
	[ -n "$BPS" ] && [ -n "$TOTAL" ] || return

	echo $(($BPS * $TOTAL))
}

# expand_fat DEV LABEL: make the FAT filesystem on DEV fill its partition
expand_fat()
{
	# There is no reliable in-place FAT resizer, so back up the (small)
	# freshly-flashed contents, reformat the whole partition and restore.
	DEV=$1
	LABEL=$2
	TEMP=$(mktemp -d)
	BACKUP=/tmp/fat-expand.tar

	mount -t vfat $DEV $TEMP || { rmdir $TEMP; return; }

	# Never touch a partition holding real user data (e.g. ROMs)
	USED_KB=$(du -sk $TEMP | cut -f 1)
	if [ $USED_KB -gt $((32 * 1024)) ];then
		echo "$DEV holds ${USED_KB}K, not expanding it"
		umount $TEMP; rmdir $TEMP
		return
	fi

	echo Expanding $DEV...
	if ! tar -cf $BACKUP -C $TEMP .;then
		umount $TEMP; rmdir $TEMP; rm -f $BACKUP
		return
	fi
	umount $TEMP

	if mkfs.fat -F 32 -n $LABEL $DEV;then
		mount -t vfat $DEV $TEMP && tar -xf $BACKUP -C $TEMP
		sync
		umount $TEMP
	fi
	rmdir $TEMP
	rm -f $BACKUP
}

# resize_fat DEV MOUNTPOINT PARTNAME MAXSIZE: format a blank FAT partition,
# or expand a FAT that is smaller than its partition (the freshly flashed
# 64M userdata image). Used for userdata, which is FAT32 so PCs can read it.
resize_fat()
{
	DEV=$1
	MAX_SIZE=$4
	LABEL=RETRONSQ

	SIZE=$(fat_fs_size $DEV)
	echo "fat: $DEV filesystem ${SIZE:-unreadable} bytes, partition $MAX_SIZE bytes"
	if [ -z "$SIZE" ];then
		# Only format a blank partition, never an unreadable one (the
		# first 64K also covers the superblock of an old ext2 userdata)
		if [ -z "$(head -c 65536 $DEV | tr -d '\000')" ];then
			echo Formatting $DEV...
			mkfs.fat -F 32 -n $LABEL $DEV
		fi
		return
	fi

	# Avoid resizing when it's large enough (delta less than 16M)
	[ $SIZE -gt $(($MAX_SIZE - 16 * 1024 * 1024)) ] && return

	expand_fat $DEV $LABEL
}

# Grow an NTFS filesystem to its partition (stock, unused here)
resize_ntfs()
{
	DEV=$1
	MAX_SIZE=$4
	SIZE=$(ntfsresize -if $DEV 2>/dev/null|grep -o "volume size: [0-9]\+" |grep -o "[0-9]\+")

	[ $? -eq 0 ] || return

	# Always attempt to fix it, since fsck might not work for ntfs
	ntfsfix $DEV

	# Avoid resizing when it's large enough (delta less than 4M)
	[ $SIZE -gt $(($MAX_SIZE - 4 * 1024 * 1024)) ] && return

	echo Resizing $DEV...
	echo y | ntfsresize -f $DEV
}

# do_resize: one /etc/fstab line (DEV MOUNTPOINT FSTYPE ...) -> resize_<type>
do_resize()
{
	# Not enough args
	[ $# -lt 3 ] && return

	# Ignore comments
	echo $1 |grep -q "^#" && return

	DEV=$1
	MOUNT_POINT=$2
	FSTYPE=$3

	# Find real dev for root dev
	if [ "$MOUNT_POINT" == '/' ];then
		DEV=$(mountpoint -n /|cut -d ' ' -f 1)
	fi

	# Unknown device
	[ -b "$DEV" ] || return

	DEV=$(realpath $DEV)
	SYS_PATH=/sys/class/block/${DEV##*/}
	MAX_SIZE=$(( $(cat ${SYS_PATH}/size) * 512))
	PART_NAME=$(grep PARTNAME ${SYS_PATH}/uevent | cut -d '=' -f 2)

	case $FSTYPE in
		ext*)
			resize_e2fs $DEV $MOUNT_POINT $PART_NAME $MAX_SIZE
			return
			;;
		vfat)
			resize_fat $DEV $MOUNT_POINT $PART_NAME $MAX_SIZE
			return
			;;
		ntfs)
			resize_ntfs $DEV $MOUNT_POINT $PART_NAME $MAX_SIZE
			return
			;;
	esac

	# Unsupported file system
}

grow_userdata_part()
{
	# The SD card image is only 128M. On first boot stretch userdata (the
	# last partition) to the end of the card, before its filesystem is
	# resized to match. Every step logs what it found, and the log ends up
	# in logs/boot.log on the SD card (save_boot_log).
	SGDISK=/usr/sbin/sgdisk
	PARTX=/usr/sbin/partx

	DEV=$(realpath /dev/block/by-name/userdata 2>/dev/null)
	if [ ! -b "$DEV" ];then
		echo "grow: no /dev/block/by-name/userdata, nothing to grow"
		return
	fi
	PART_SYS=/sys/class/block/${DEV##*/}
	PART=$(cat $PART_SYS/partition 2>/dev/null)
	DISK_NAME=$(basename $(dirname $(readlink -f $PART_SYS)))
	DISK=/dev/$DISK_NAME
	DISK_SIZE=$(cat /sys/class/block/$DISK_NAME/size 2>/dev/null)
	START=$(cat $PART_SYS/start 2>/dev/null)
	END=$(($START + $(cat $PART_SYS/size)))
	echo "grow: userdata is $DEV (partition $PART of $DISK)," \
		"sectors $START-$END of $DISK_SIZE"
	if [ -z "$PART" ] || [ -z "$DISK_SIZE" ] || [ -z "$START" ];then
		echo "grow: couldn't read the partition layout from sysfs"
		return
	fi

	# Only the last partition can grow
	for P in /sys/class/block/$DISK_NAME/$DISK_NAME*; do
		[ -f $P/start ] || continue
		if [ $(cat $P/start) -gt $START ];then
			echo "grow: ${P##*/} comes after userdata, not growing"
			return
		fi
	done

	# Less than 16M (in 512 byte sectors) to gain: already done
	if [ $(($DISK_SIZE - $END)) -le 32768 ];then
		echo "grow: userdata already fills $DISK"
		return
	fi
	if [ ! -x $SGDISK ] || [ ! -x $PARTX ];then
		echo "grow: $SGDISK or $PARTX missing"
		return
	fi

	echo "grow: growing $DEV to the end of $DISK..."
	INFO=$($SGDISK -i $PART $DISK)
	TYPE=$(echo "$INFO" | sed -n 's/^Partition GUID code: \([0-9A-F-]*\).*/\1/p')
	UUID=$(echo "$INFO" | sed -n 's/^Partition unique GUID: \([0-9A-F-]*\).*/\1/p')
	NAME=$(echo "$INFO" | sed -n "s/^Partition name: '\(.*\)'/\1/p")
	if [ -z "$TYPE" ] || [ -z "$UUID" ];then
		echo "grow: sgdisk couldn't read partition $PART:"
		echo "$INFO"
		return
	fi

	# Move the backup GPT to the real end of the card, then recreate the
	# partition with the same start (no realignment), type, GUID and name
	if ! $SGDISK -e $DISK;then
		echo "grow: sgdisk -e failed"
		return
	fi
	if ! $SGDISK -a 1 -d $PART -n $PART:$START:0 -t $PART:$TYPE \
			-u $PART:$UUID -c $PART:"$NAME" $DISK;then
		echo "grow: sgdisk failed to recreate partition $PART"
		return
	fi

	# Tell the kernel; if that fails the new size shows up after a reboot
	if $PARTX -u -n $PART $DISK;then
		echo "grow: done, userdata is now $(($(cat $PART_SYS/size) / 2048)) MiB"
	else
		echo "grow: partx failed, rebooting to use the new partition table"
		sync
		reboot -f
	fi
}

# Keep this boot's storage log on the SD card (logs/boot.log), so a problem
# with growing or checking the card can be read on a PC
# Seconds since power-on, to time each step in boot.log
uptime_s()
{
	cut -d ' ' -f 1 /proc/uptime
}

save_boot_log()
{
	mountpoint -q /userdata || return
	mkdir -p /userdata/logs
	{
		echo "Retron SQ boot log"
		cat /etc/retronsq-version 2>/dev/null
		uname -a
		echo; echo "--- /proc/partitions"; cat /proc/partitions
		echo; echo "--- grow and resize ($T_RESIZE s -> $T_CHECK s since power-on)"; cat $RESIZE_LOG
		echo; echo "--- check ($T_CHECK s -> $T_MOUNT s)"; cat $CHECK_LOG
		echo; echo "--- mount ($T_MOUNT s -> $(uptime_s) s)"; cat $MOUNT_LOG
		echo; echo "--- df"; df -h
	} > /userdata/logs/boot.log 2>&1
	sync
}

# Resize every filesystem listed in /etc/fstab
resizeall()
{
	echo "Will now resize all file systems of types $MOUNTALL_USER_FSTYPES"
	while read LINE;do
		do_resize $LINE
	done < /etc/fstab
}

# fsck every filesystem listed in /etc/fstab (force_fsck on the kernel
# command line forces a full check)
checkall()
{
	grep -wq "force_fsck" /proc/cmdline && FORCE_FSCK="-f"
	grep -wq "no_force_fsck" /proc/cmdline && unset FORCE_FSCK

	# Uncomment below to enable force fsck
	# FORCE_FSCK="-f"

	echo "Will now check all file systems of types $MOUNTALL_USER_FSTYPES"

	SKIP_FSCK="/var/.skip_fsck"
	if [ -f $SKIP_FSCK ];then
		echo "Skipped, remove $SKIP_FSCK to enable it again"
		return
	else
		echo "Create $SKIP_FSCK to skip it"
	fi

	fsck -ARy $FORCE_FSCK -t $MOUNTALL_USER_FSTYPES

	# The fsck might not work for vfat/ntfs...
	# But no worry, we've done that in resize_fat/resize_ntfs ;)
}

# Mount everything in /etc/fstab
mountall()
{
	echo "Will now mount all file systems of types $MOUNTALL_FSTYPES"
	mount -a -t $MOUNTALL_FSTYPES
}

is_recovery()
{
	# Recovery's rootfs is ramfs
	mountpoint -d /|grep -wq 0:1
}

case "$1" in
  start|"")
	RESIZE_LOG=/tmp/resizefs.log
	CHECK_LOG=/tmp/checkfs.log
	MOUNT_LOG=/tmp/mountfs.log

	MOUNTALL_BASE_FSTYPES="proc,devpts,tmpfs,sysfs"
	MOUNTALL_USER_FSTYPES="ext2,ext3,ext4,vfat,ntfs,squashfs"
	MOUNTALL_FSTYPES="${MOUNTALL_USER_FSTYPES},${MOUNTALL_BASE_FSTYPES}"

	# Mount /tmp firstly to save logs
	mountpoint -q /tmp || mount -t tmpfs tmpfs /tmp

	if is_recovery;then
		# Only mount basic file systems for recovery
		MOUNTALL_FSTYPES=$MOUNTALL_BASE_FSTYPES
		mountall 2>&1 | tee $MOUNT_LOG
		echo Log saved to $MOUNT_LOG
	else
		T_RESIZE=$(uptime_s)
		grow_userdata_part 2>&1 | tee $RESIZE_LOG
		resizeall 2>&1 | tee -a $RESIZE_LOG
		echo Log saved to $RESIZE_LOG
		T_CHECK=$(uptime_s)
		checkall 2>&1 | tee $CHECK_LOG
		echo Log saved to $CHECK_LOG
		T_MOUNT=$(uptime_s)
		mountall 2>&1 | tee $MOUNT_LOG
		echo Log saved to $MOUNT_LOG
		save_boot_log
	fi
	touch /tmp/boot_complete
	echo "\n\n S21mountall.sh done\n\n" > /dev/console
	;;
  restart|reload|force-reload)
	echo "Error: argument '$1' not supported" >&2
	exit 3
	;;
  stop|status)
	# No-op
	;;
  *)
	echo "Usage: [start|stop]" >&2
	exit 3
	;;
esac

:
