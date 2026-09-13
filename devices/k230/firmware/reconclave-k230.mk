################################################################################
# Reconclave K230
################################################################################

RECONCLAVE_K230_VERSION = 0.1.0
RECONCLAVE_K230_SITE = $(RECONCLAVE_K230_PKGDIR)/src
RECONCLAVE_K230_SITE_METHOD = local
RECONCLAVE_K230_INSTALL_TARGET = YES
RECONCLAVE_K230_SUPPORTS_IN_SOURCE_BUILD = NO
RECONCLAVE_K230_DEPENDENCIES = lvgl libdrm

RECONCLAVE_K230_CONF_OPTS = \
	-DRECONCLAVE_BUILD_K230=ON \
	-DRECONCLAVE_BUILD_TESTS=OFF \
	-DK230_DEVICE_SYSROOT=$(STAGING_DIR)/usr

define RECONCLAVE_K230_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/buildroot-build/devices/k230/reconclave_k230 \
		$(TARGET_DIR)/usr/bin/reconclave-k230-node
	$(INSTALL) -D -m 0755 $(@D)/buildroot-build/devices/k230/reconclave_k230_ui \
		$(TARGET_DIR)/usr/bin/reconclave-k230-ui
endef

$(eval $(cmake-package))
