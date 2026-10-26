################################################################################
#
# partinit
#
################################################################################

PARTINIT_VERSION = 0.0.1
PARTINIT_SITE = $(TOPDIR)/package/rockchip/partinit
PARTINIT_SITE_METHOD = local

define PARTINIT_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/S21mountall.sh $(TARGET_DIR)/etc/init.d/
	$(INSTALL) -m 0644 -D $(@D)/fstab $(TARGET_DIR)/etc/
	$(INSTALL) -m 0755 -D $(@D)/61-partition-init.rules $(TARGET_DIR)/lib/udev/rules.d/
	$(INSTALL) -m 0755 -D $(@D)/61-sd-cards-auto-mount.rules $(TARGET_DIR)/lib/udev/rules.d/
	echo -e "/dev/block/by-name/misc\t\t/misc\t\t\temmc\t\tdefaults\t\t0\t0" >> $(TARGET_DIR)/etc/fstab
	echo -e "/dev/block/by-name/oem\t\t/oem\t\t\t$$RK_OEM_FS_TYPE\t\tdefaults\t\t0\t2" >> $(TARGET_DIR)/etc/fstab
	# mount(8) and S21mountall only know "vfat", and a FAT userdata that is
	# yanked out of power needs "flush" to keep the PC-visible copy sane.
	USERDATA_FS=$$RK_USERDATA_FS_TYPE; USERDATA_OPTS=defaults; \
	if [ "$$USERDATA_FS" = "fat" -o "$$USERDATA_FS" = "vfat" ]; then \
		USERDATA_FS=vfat; USERDATA_OPTS=defaults,noatime,flush,shortname=mixed; \
	fi; \
	echo -e "/dev/block/by-name/userdata\t/userdata\t\t$$USERDATA_FS\t\t$$USERDATA_OPTS\t\t0\t2" >> $(TARGET_DIR)/etc/fstab
	cd $(TARGET_DIR) && rm -rf oem userdata data mnt udisk sdcard && mkdir -p oem userdata mnt/sdcard && ln -s userdata data && ln -s media/usb0 udisk && ln -s mnt/sdcard sdcard && cd -
endef

$(eval $(generic-package))
