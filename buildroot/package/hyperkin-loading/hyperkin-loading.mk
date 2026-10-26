HYPERKIN_LOADING_SITE = $(TOPDIR)/package/hyperkin-loading/src
HYPERKIN_LOADING_SITE_METHOD = local
HYPERKIN_LOADING_DEPENDENCIES = host-pkgconf sdl2 sdl2_image sdl2_mixer sdl2_ttf libegl libgles usbmount
HYPERKIN_LOADING_BUILD_OPTS =

HYPERKIN_LOADING_MAKE = $(MAKE) BOARD=$(BR2_ARCH)

HYPERKIN_LOADING_MAKE_OPTS = CC="$(TARGET_CC) $(HYPERKIN_LOADING_BUILD_OPTS)" \
			CXX="$(TARGET_CXX) $(HYPERKIN_LOADING_BUILD_OPTS)"

define HYPERKIN_LOADING_CONFIGURE_CMDS
	# Do nothing
endef

define HYPERKIN_LOADING_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(HYPERKIN_LOADING_MAKE) -C $(@D) $(HYPERKIN_LOADING_MAKE_OPTS)
endef

ifeq ($(BR2_PACKAGE_LIBUSB),y)
LIBPCAP_DEPENDENCIES += libusb
LIBPCAP_CFLAGS += "-I$(STAGING_DIR)/usr/include/libusb-1.0"
LIBPCAP_CONF_OPTS += --with-libusb=$(STAGING_DIR)/usr
else
LIBPCAP_CONF_OPTS += --without-libusb
endif


define HYPERKIN_LOADING_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/hyperkin-loading $(TARGET_DIR)/usr/bin
	$(INSTALL) -D -m 0755 $(@D)/hyperkin-crc $(TARGET_DIR)/usr/bin

#	$(STRIP) $(TARGET_DIR)/usr/bin/hyperkin-loading
	cp $(@D)/S50ui $(TARGET_DIR)/etc/init.d
	cp $(@D)/01_try_launch_retroarch $(TARGET_DIR)/etc/usbmount/mount.d/
	cp $(@D)/01_kill_retroarch $(TARGET_DIR)/etc/usbmount/umount.d/
endef

# Factory copy of the SD card's userdata files. sq-launcher puts back any of
# them that went missing (deleted from a PC, card reformatted...). Done as
# a finalize hook so every build picks up changes to device/rockchip/userdata.
HYPERKIN_LOADING_USERDATA_DIR = $(TOPDIR)/../device/rockchip/userdata/$(if $(RK_USERDATA_DIR),$(RK_USERDATA_DIR),userdata_brk01)

define HYPERKIN_LOADING_INSTALL_USERDATA_DEFAULTS
	rm -rf $(TARGET_DIR)/usr/share/retronsq/userdata
	mkdir -p $(TARGET_DIR)/usr/share/retronsq
	cp -a $(HYPERKIN_LOADING_USERDATA_DIR) $(TARGET_DIR)/usr/share/retronsq/userdata
	find $(TARGET_DIR)/usr/share/retronsq/userdata \
		\( -name .gitignore -o -name .gitkeep \) -delete
endef
ifeq ($(BR2_PACKAGE_HYPERKIN_LOADING),y)
TARGET_FINALIZE_HOOKS += HYPERKIN_LOADING_INSTALL_USERDATA_DEFAULTS
endif

$(eval $(autotools-package))

