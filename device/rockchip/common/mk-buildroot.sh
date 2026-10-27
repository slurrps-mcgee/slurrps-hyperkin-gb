#!/bin/bash
#
# mk-buildroot.sh <BoardConfig.mk>: build the Buildroot root filesystem for
# the board's RK_CFG_BUILDROOT config (called by build.sh).
#

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
# Kconfig silently drops a defconfig option whose dependencies aren't met
# (partinit vanished that way when usbmount was removed). Fail loudly
# instead of building a firmware that is missing a package.
DEFCONFIG=$TOP_DIR/buildroot/configs/${RK_CFG_BUILDROOT}_defconfig
DROPPED=$(grep "^BR2_.*=" $DEFCONFIG | while IFS= read -r LINE; do
	grep -qxF "$LINE" $TARGET_OUTPUT_DIR/.config || echo "$LINE"
done)
if [ -n "$DROPPED" ]; then
	echo "These defconfig options were dropped by Buildroot (unmet dependencies?):" >&2
	echo "$DROPPED" >&2
	exit 1
fi
# Buildroot only copies a local-source package (its sources live in the tree)
# once, so later edits would silently never reach the image. Rebuild each one
# whose package directory has a file newer than its last copy.
for PKG in partinit retronsq retroarch; do
	PKG_SYM=BR2_PACKAGE_$(echo $PKG | tr a-z- A-Z_)
	grep -q "^$PKG_SYM=y" $TARGET_OUTPUT_DIR/.config || continue
	for STAMP in $TARGET_OUTPUT_DIR/build/$PKG/.stamp_rsynced \
		$TARGET_OUTPUT_DIR/build/$PKG-*/.stamp_rsynced; do
		[ -e "$STAMP" ] || continue
		PKG_DIR=$(find $TOP_DIR/buildroot/package -maxdepth 2 -type d -name $PKG | head -n 1)
		if [ -n "$PKG_DIR" ] && [ -n "$(find "$PKG_DIR" -newer "$STAMP" -print -quit)" ]; then
			echo "$PKG: sources changed, rebuilding"
			make $PKG-rebuild || exit 1
		fi
		break
	done
done
make
if [ $? -ne 0 ]; then
    exit 1
fi
# The image must carry the firmware's current scripts. Compare them with
# their sources: catches a package that was dropped or didn't rebuild (a
# stale output/target once shipped v0.1's storage script in v0.2.0).
TARGET=$TARGET_OUTPUT_DIR/target
PKGS=$TOP_DIR/buildroot/package
STALE=""
for PAIR in rockchip/partinit/S21mountall.sh:etc/init.d/S21mountall.sh \
		retronsq/scripts/S50ui:etc/init.d/S50ui \
		retronsq/udev/90-retronsq-cart.rules:lib/udev/rules.d/90-retronsq-cart.rules \
		retronsq/config/retroarch-cfw.cfg:usr/share/retronsq/retroarch-cfw.cfg \
		$(cd $PKGS/retronsq/scripts && for F in *; do
			[ $F = S50ui ] || echo retronsq/scripts/$F:usr/lib/retronsq/$F; done); do
	cmp -s $PKGS/${PAIR%%:*} $TARGET/${PAIR#*:} || STALE="$STALE ${PAIR#*:}"
done
[ -e $TARGET/lib/udev/rules.d/61-sd-cards-auto-mount.rules ] && STALE="$STALE 61-sd-cards-auto-mount.rules"
if [ -n "$STALE" ]; then
	echo "The rootfs has old or missing firmware files:$STALE" >&2
	exit 1
fi
echo "pack buildroot image at: $TOP_DIR/buildroot/output/$RK_CFG_BUILDROOT/images/rootfs.$RK_ROOTFS_TYPE"
