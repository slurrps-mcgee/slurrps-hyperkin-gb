#!/bin/bash

COMMON_DIR=$(cd `dirname $0`; pwd)
if [ -h $0 ]
then
        CMD=$(readlink $0)
        COMMON_DIR=$(dirname $CMD)
fi
cd $COMMON_DIR
cd ../../..
TOP_DIR=$(pwd)
BOARD_CONFIG=$1
source $BOARD_CONFIG
if [ -z $RK_CFG_BUILDROOT ]
then
        echo "RK_CFG_BUILDROOT is empty, skip building buildroot rootfs!"
        exit 0
fi
source $TOP_DIR/buildroot/build/envsetup.sh $RK_CFG_BUILDROOT
# Buildroot only copies a local-source package once, so later edits to these
# packages would silently never reach the image. They are tiny: always
# refresh the ones this config uses.
for PKG in partinit hyperkin-loading; do
	PKG_SYM=BR2_PACKAGE_$(echo $PKG | tr a-z- A-Z_)
	if grep -q "^$PKG_SYM=y" $TARGET_OUTPUT_DIR/.config && \
		ls -d $TARGET_OUTPUT_DIR/build/$PKG $TARGET_OUTPUT_DIR/build/$PKG-* \
			2> /dev/null | grep -q .; then
		make $PKG-rebuild || exit 1
	fi
done
make
if [ $? -ne 0 ]; then
    exit 1
fi
echo "pack buildroot image at: $TOP_DIR/buildroot/output/$RK_CFG_BUILDROOT/images/rootfs.$RK_ROOTFS_TYPE"
