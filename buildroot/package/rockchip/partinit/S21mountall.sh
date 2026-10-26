#!/bin/sh

# Uncomment below to see more logs
# set -x

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

fat_fs_size()
{
	# Filesystem size in bytes, read straight from the FAT boot sector
	DEV=$1

	SIG=$(od -An -tx1 -j510 -N2 $DEV | tr -d ' \n')
	[ "$SIG" == "55aa" ] || return

	BPS=$(od -An -tu2 -j11 -N2 $DEV | tr -d ' \n')
	TOTAL=$(od -An -tu2 -j19 -N2 $DEV | tr -d ' \n')
	[ "$TOTAL" == "0" ] && TOTAL=$(od -An -tu4 -j32 -N4 $DEV | tr -d ' \n')

	echo $(($BPS * $TOTAL))
}

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

resize_fat()
{
	DEV=$1
	MAX_SIZE=$4
	LABEL=RETRONSQ

	SIZE=$(fat_fs_size $DEV)
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
	# resized to match.
	DEV=$(realpath /dev/block/by-name/userdata 2>/dev/null)
	[ -b "$DEV" ] || return
	PART_SYS=/sys/class/block/${DEV##*/}
	PART=$(cat $PART_SYS/partition 2>/dev/null) || return
	DISK_NAME=$(basename $(dirname $(readlink -f $PART_SYS)))
	DISK=/dev/$DISK_NAME
	DISK_SIZE=$(cat /sys/class/block/$DISK_NAME/size)
	START=$(cat $PART_SYS/start)
	END=$(($START + $(cat $PART_SYS/size)))

	# Only the last partition can grow
	for P in /sys/class/block/$DISK_NAME/$DISK_NAME*; do
		[ -f $P/start ] || continue
		[ $(cat $P/start) -gt $START ] && return
	done

	# Less than 16M (in 512 byte sectors) to gain: already done
	[ $(($DISK_SIZE - $END)) -gt 32768 ] || return
	which sgdisk >/dev/null || return

	echo Growing $DEV to the end of $DISK...
	INFO=$(sgdisk -i $PART $DISK) || return
	TYPE=$(echo "$INFO" | sed -n 's/^Partition GUID code: \([0-9A-F-]*\).*/\1/p')
	UUID=$(echo "$INFO" | sed -n 's/^Partition unique GUID: \([0-9A-F-]*\).*/\1/p')
	NAME=$(echo "$INFO" | sed -n "s/^Partition name: '\(.*\)'/\1/p")
	[ -n "$TYPE" ] && [ -n "$UUID" ] || return

	# Move the backup GPT to the real end of the card, then recreate the
	# partition with the same start (no realignment), type, GUID and name
	sgdisk -e $DISK || return
	sgdisk -a 1 -d $PART -n $PART:$START:0 -t $PART:$TYPE \
		-u $PART:$UUID -c $PART:"$NAME" $DISK || return

	# Tell the kernel; if that fails the new size shows up after a reboot
	partx -u -n $PART $DISK || { sync; reboot -f; }
}

resizeall()
{
	echo "Will now resize all file systems of types $MOUNTALL_USER_FSTYPES"
	while read LINE;do
		do_resize $LINE
	done < /etc/fstab
}

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
		grow_userdata_part 2>&1 | tee $RESIZE_LOG
		resizeall 2>&1 | tee -a $RESIZE_LOG
		echo Log saved to $RESIZE_LOG
		checkall 2>&1 | tee $CHECK_LOG
		echo Log saved to $CHECK_LOG
		mountall 2>&1 | tee $MOUNT_LOG
		echo Log saved to $MOUNT_LOG
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
