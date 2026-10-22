include $(TOPDIR)/rules.mk

PKG_NAME:=shcmd
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_MAINTAINER:=OpenWrt Community
PKG_LICENSE:=GPL-2.0
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/shcmd
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Allowlisted HTTP command executor
  DEPENDS:=
endef

define Package/shcmd/description
  A tiny static daemon exposing a token-authenticated HTTP endpoint that runs
  a fixed, operator-defined list of commands. Requests select a command by
  name and may append positional arguments; there is no shell interpolation,
  so request data can never be interpreted as a second command.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu99 -Wall -Wextra -Os -static \
		-ffunction-sections -fdata-sections \
		-Wl,--gc-sections \
		-o $(PKG_BUILD_DIR)/shcmd \
		$(PKG_BUILD_DIR)/shcmd.c
	# Use $(STRIP), not a made-up $(TARGET_STRIP): on 19.07 the latter is
	# undefined, so a recipe line starting with it degenerates into running
	# the ARM binary on the x86 host ("Exec format error").
	$(STRIP) $(PKG_BUILD_DIR)/shcmd
endef

define Package/shcmd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/shcmd $(1)/usr/sbin/shcmd
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) ./files/shc $(1)/usr/bin/shc
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/shcmd.init $(1)/etc/init.d/shcmd
	$(INSTALL_DIR) $(1)/etc
	$(INSTALL_CONF) ./files/shcmd.conf $(1)/etc/shcmd.conf
endef

define Package/shcmd/conffiles
/etc/shcmd.conf
endef

$(eval $(call BuildPackage,shcmd))
