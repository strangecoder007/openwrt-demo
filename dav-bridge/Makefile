#
# Copyright (C) 2026
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=dav-bridge
PKG_RELEASE:=4

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Local <root@localhost>

include $(INCLUDE_DIR)/package.mk

define Package/dav-bridge
  SECTION:=net
  CATEGORY:=Network
  TITLE:=WebDAV JSON CGI bridge for WeChat Mini Program
  DEPENDS:=+lighttpd +lighttpd-mod-cgi
endef

define Package/dav-bridge/description
  Minimal CGI exposing PROPFIND (list) and MKCOL (mkdir) as plain GET
  endpoints that return JSON, because WeChat wx.request cannot issue
  PROPFIND/MKCOL on real devices. PUT/GET/DELETE still go straight to
  lighttpd mod_webdav; only listing and directory creation go through
  this bridge. Basic auth is enforced by lighttpd mod_auth (REMOTE_USER).
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra -O2 \
		-o $(PKG_BUILD_DIR)/dav-bridge $(PKG_BUILD_DIR)/dav-bridge.c
endef

define Package/dav-bridge/install
	$(INSTALL_DIR) $(1)/usr/lib/cgi-bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/dav-bridge $(1)/usr/lib/cgi-bin/dav-bridge.cgi
endef

$(eval $(call BuildPackage,dav-bridge))
