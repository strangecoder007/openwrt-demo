#
# Copyright (C) 2026
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=wol-proxy
PKG_RELEASE:=3

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Local <root@localhost>

include $(INCLUDE_DIR)/package.mk

define Package/wol-proxy
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Wake-on-LAN broadcast relay for WireGuard tunnels
  DEPENDS:=+libc
endef

define Package/wol-proxy/description
  Listens on UDP port 9 and re-emits received Wake-on-LAN magic
  packets as a fresh broadcast on the LAN interface, so that WoL
  works from WireGuard remote clients.
endef

define Package/wol-proxy/conffiles
/etc/config/wol-proxy
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra \
		-o $(PKG_BUILD_DIR)/wol-proxy $(PKG_BUILD_DIR)/wol-proxy.c
endef

define Package/wol-proxy/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/wol-proxy $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/wol-proxy.init $(1)/etc/init.d/wol-proxy
	$(INSTALL_CONF) ./files/wol-proxy.config $(1)/etc/config/wol-proxy
endef

$(eval $(call BuildPackage,wol-proxy))
