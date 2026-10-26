#!/bin/bash

COMMON_DIR=$(cd "$(dirname "$0")"; pwd)
if [ -h "$0" ]
then
        CMD=$(readlink "$0")
        COMMON_DIR=$(dirname "$CMD")
fi

if [ -n "$1" ]
then
        USERDATA_DIR="$1"
else
        exit 1
fi

if [ -n "$2" ]
then
        USERDATA_IMG="$2"
else
        exit 1
fi

if [ -n "$3" ]
then
        FS_TYPE="$3"
else
        exit 1
fi

if [ $FS_TYPE = ext2 ]
then
        $COMMON_DIR/mke2img.sh $USERDATA_DIR $USERDATA_IMG

elif [ $FS_TYPE = fat ]
then
	# Stage a copy without the git placeholder files
	STAGE=$(mktemp -d)
	cp -a "$USERDATA_DIR/." "$STAGE/"
	find "$STAGE" \( -name .gitignore -o -name .gitkeep \) -delete

	# FAT32 needs ~65525 clusters, so never go below 64M; leave room to grow.
	# The partition is expanded to the whole card on first boot anyway.
	SIZE=$(du -s -BM "$STAGE" | awk '{print int($1)}')
	SIZE=$((SIZE + SIZE / 4 + 16))
	[ $SIZE -lt 64 ] && SIZE=64
	# echo "create image size=${SIZE}M"
	rm -f "$USERDATA_IMG"
	dd if=/dev/zero of="$USERDATA_IMG" bs=1M count=$SIZE >/dev/null 2>&1
	if ! mkfs.vfat -F 32 -n RETRONSQ "$USERDATA_IMG" >/dev/null || \
		! MTOOLS_SKIP_CHECK=1 mcopy -s -i "$USERDATA_IMG" "$STAGE"/* ::/; then
		rm -rf "$STAGE"
		exit 1
	fi
	rm -rf "$STAGE"
else
        echo "file system: $FS_TYPE not support."
        exit 1
fi
