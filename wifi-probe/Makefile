include $(TOPDIR)/rules.mk

PKG_NAME:=wifi-probe
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_MAINTAINER:=OpenWrt Community
PKG_LICENSE:=GPL-2.0
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/wifi-probe
  SECTION:=net
  CATEGORY:=Network
  DEPENDS:=+libpcap +libsqlite3 +libpthread
  TITLE:=802.11 probe-request based device proximity tracker
endef

define Package/wifi-probe/description
  Captures 802.11 Probe Request/Beacon management frames on a monitor
  interface, aggregates device MAC (anonymized), RSSI bin, and session
  visits, and reports via CLI and LuCI.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu99 -Wall -Os \
		-I$(STAGING_DIR)/usr/include \
		-o $(PKG_BUILD_DIR)/wifi-probe \
		$(PKG_BUILD_DIR)/main.c \
		$(PKG_BUILD_DIR)/args.c \
		$(PKG_BUILD_DIR)/capture.c \
		$(PKG_BUILD_DIR)/frame.c \
		$(PKG_BUILD_DIR)/radiotap.c \
		$(PKG_BUILD_DIR)/ieee80211.c \
		$(PKG_BUILD_DIR)/probe.c \
		$(PKG_BUILD_DIR)/device.c \
		$(PKG_BUILD_DIR)/db.c \
		-L$(STAGING_DIR)/usr/lib \
		-lpcap -lsqlite3 -lpthread
endef

define Package/wifi-probe/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/wifi-probe $(1)/usr/sbin/wifi-probe
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/wifi-probe.init $(1)/etc/init.d/wifi-probe
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/wifi-probe.config $(1)/etc/config/wifi-probe
endef

define Package/wifi-probe/conffiles
/etc/config/wifi-probe
endef

$(eval $(call BuildPackage,wifi-probe))
