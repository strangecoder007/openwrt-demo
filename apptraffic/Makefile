#
# Copyright (C) 2024 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=apptraffic
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_MAINTAINER:=OpenWrt Community
PKG_LICENSE:=GPL-2.0

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/apptraffic
  SECTION:=net
  CATEGORY:=Network
  DEPENDS:=+libpcap +libsqlite3 +libpthread +kmod-nf-conntrack +kmod-nf-conntrack-netlink
  TITLE:=Application-aware traffic analysis tool
  URL:=https://github.com/openwrt/apptraffic
endef

define Package/apptraffic/description
  apptraffic is a traffic analysis tool that identifies which applications
  and websites users are accessing. It captures DNS queries, TLS SNI
  (Server Name Indication) from HTTPS connections, and correlates them
  with netfilter conntrack flow statistics to provide per-app/per-website
  bandwidth accounting.

  Features:
  - DNS query capture for IP to domain mapping
  - TLS SNI extraction for HTTPS domain identification
  - HTTP Host header capture for plain HTTP
  - Netfilter conntrack integration for flow statistics
  - Domain-to-app mapping database (200+ apps/websites)
  - SQLite persistent storage
  - JSON and CSV data export
  - LuCI web interface integration
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu99 -Wall -Os \
		-I$(STAGING_DIR)/usr/include \
		-o $(PKG_BUILD_DIR)/apptraffic \
		$(PKG_BUILD_DIR)/main.c \
		-L$(STAGING_DIR)/usr/lib \
		-lpcap -lsqlite3 -lpthread
endef

define Package/apptraffic/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/apptraffic $(1)/usr/sbin/apptraffic

	$(INSTALL_DIR) $(1)/usr/share/apptraffic
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/app-mapping.txt $(1)/usr/share/apptraffic/app-mapping.txt

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/apptraffic.init $(1)/etc/init.d/apptraffic

	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/apptraffic.config $(1)/etc/config/apptraffic

	$(INSTALL_DIR) $(1)/etc/hotplug.d/iface
	$(INSTALL_BIN) ./files/apptraffic.hotplug $(1)/etc/hotplug.d/iface/30-apptraffic
endef

define Package/apptraffic/conffiles
/etc/config/apptraffic
/usr/share/apptraffic/app-mapping.txt
endef

$(eval $(call BuildPackage,apptraffic))
