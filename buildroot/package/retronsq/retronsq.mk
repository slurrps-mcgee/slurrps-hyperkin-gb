################################################################################
#
# retronsq
#
# The Retron SQ custom firmware's own software, all in one package:
#
#   src/lib/      libsqcart: cartridge reader, copy + CRC, saves, backups
#   src/load/     sqload: loading screen for a cartridge's first copy
#   src/cli/      sqcart: libsqcart for the shell scripts
#   src/tools/    Cartridge Tools, a libretro core run by RetroArch
#   src/home/     sqhome: the home screen
#   scripts/      sq-session (runs games), cart-event (hot-plug), common.sh,
#                 sq-scan-roms, reset-btn, S50ui (boot)
#   udev/         sends cartridge reader events to cart-event
#   config/       setting defaults (defaults.cfg), RetroArch settings the
#                 firmware needs (retroarch-cfw.cfg)
#   splash/       boot splash and loading screen, rendered at build time
#
# See docs/DOCUMENTATION.md and docs/DEVELOPING.md at the top of the tree.
#
################################################################################

RETRONSQ_SITE = $(TOPDIR)/package/retronsq
RETRONSQ_SITE_METHOD = local
RETRONSQ_LICENSE = GPL-3.0+ (tools core, RGUI font), MIT (libretro.h)
RETRONSQ_DEPENDENCIES = sdl2 sdl2_image sdl2_ttf dejavu

define RETRONSQ_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)/src CC="$(TARGET_CC)" AR="$(TARGET_AR)" \
		CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
endef

RETRONSQ_SCRIPTS = common.sh sq-session sq-scan-roms cart-event reset-btn

define RETRONSQ_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/src/sqload $(TARGET_DIR)/usr/bin/sqload
	$(INSTALL) -D -m 0755 $(@D)/src/sqcart $(TARGET_DIR)/usr/bin/sqcart
	$(INSTALL) -D -m 0755 $(@D)/src/sqhome $(TARGET_DIR)/usr/bin/sqhome
	$(INSTALL) -D -m 0644 $(@D)/src/sqtools_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/sqtools_libretro.so
	$(INSTALL) -D -m 0644 $(@D)/config/retroarch-cfw.cfg \
		$(TARGET_DIR)/usr/share/retronsq/retroarch-cfw.cfg
	$(INSTALL) -D -m 0644 $(@D)/config/defaults.cfg \
		$(TARGET_DIR)/usr/share/retronsq/defaults.cfg
	$(INSTALL) -D -m 0644 $(@D)/src/tools/cartridge-tools.sqt \
		$(TARGET_DIR)/usr/share/retronsq/cartridge-tools.sqt
	for f in $(RETRONSQ_SCRIPTS); do \
		$(INSTALL) -D -m 0755 $(@D)/scripts/$$f $(TARGET_DIR)/usr/lib/retronsq/$$f || exit 1; \
	done
	$(INSTALL) -D -m 0755 $(@D)/scripts/S50ui $(TARGET_DIR)/etc/init.d/S50ui
	$(INSTALL) -D -m 0644 $(@D)/udev/90-retronsq-cart.rules \
		$(TARGET_DIR)/lib/udev/rules.d/90-retronsq-cart.rules
	mkdir -p $(TARGET_DIR)/media/cart
endef

# Firmware version: RETRONSQ_VERSION from the environment (the release
# workflow sets it), else the VERSION file at the top of the tree. While the
# major version is 0 the screens say "v0.3.1 beta".
RETRONSQ_VERSION ?= $(shell cat $(TOPDIR)/../VERSION 2>/dev/null)
RETRONSQ_VERSION_LABEL = v$(RETRONSQ_VERSION)$(if $(filter 0.%,$(RETRONSQ_VERSION)), beta)

# The boot splash (shown by S50ui) and the loading screen background (shown
# by sqload) carry the version, so they are rendered from their SVG
# templates on every build: a finalize hook, so a version change never needs
# a package rebuild. rsvg-convert and its fonts come with the build
# container (docker/Containerfile).
define RETRONSQ_INSTALL_VERSION
	@if ! command -v rsvg-convert > /dev/null; then \
		echo "rsvg-convert not found: build with ./build-in-container.sh" >&2; \
		exit 1; \
	fi
	echo "Retron SQ CFW $(RETRONSQ_VERSION_LABEL)" > $(TARGET_DIR)/etc/retronsq-version
	for img in splash loading; do \
		sed 's/@VERSION_LABEL@/$(RETRONSQ_VERSION_LABEL)/g' \
			$(TOPDIR)/package/retronsq/splash/retronsq-$$img.svg \
			> $(BUILD_DIR)/retronsq-$$img.svg && \
		rsvg-convert -w 1280 -h 720 -o $(TARGET_DIR)/etc/retronsq-$$img.png \
			$(BUILD_DIR)/retronsq-$$img.svg || exit 1; \
	done
endef

# Factory copy of the SD card's userdata files. sq-session puts back any of
# them that went missing (deleted from a PC, card reformatted...). Also a
# finalize hook, so changes to device/rockchip/userdata always land.
RETRONSQ_USERDATA_DIR = $(TOPDIR)/../device/rockchip/userdata/$(if $(RK_USERDATA_DIR),$(RK_USERDATA_DIR),userdata_brk01)

define RETRONSQ_INSTALL_USERDATA_DEFAULTS
	rm -rf $(TARGET_DIR)/usr/share/retronsq/userdata
	mkdir -p $(TARGET_DIR)/usr/share/retronsq
	cp -a $(RETRONSQ_USERDATA_DIR) $(TARGET_DIR)/usr/share/retronsq/userdata
	find $(TARGET_DIR)/usr/share/retronsq/userdata \
		\( -name .gitignore -o -name .gitkeep \) -delete
endef

ifeq ($(BR2_PACKAGE_RETRONSQ),y)
TARGET_FINALIZE_HOOKS += RETRONSQ_INSTALL_VERSION RETRONSQ_INSTALL_USERDATA_DEFAULTS
endif

$(eval $(generic-package))
