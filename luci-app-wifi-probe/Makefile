include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-wifi-probe
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=OpenWrt Community
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-wifi-probe
  SECTION:=luci
  CATEGORY:=LuCI
  DEPENDS:=+luci-compat +wifi-probe
  TITLE:=WiFi Probe - 802.11 device proximity statistics
endef

define Package/luci-app-wifi-probe/description
  LuCI interface for the wifi-probe 802.11 device proximity daemon.
  Provides Overview (device visits, RSSI bins) and Configuration pages.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./luasrc/* $(PKG_BUILD_DIR)/
	$(CP) ./htdocs/* $(PKG_BUILD_DIR)/
	$(CP) ./root/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	# LuCI app ships data only; nothing to build.
endef

define Package/luci-app-wifi-probe/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/controller
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/controller/*.lua $(1)/usr/lib/lua/luci/controller/

	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/model/cbi/wifi-probe
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/model/cbi/wifi-probe/*.lua $(1)/usr/lib/lua/luci/model/cbi/wifi-probe/

	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/view/wifi-probe
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/view/wifi-probe/*.htm $(1)/usr/lib/lua/luci/view/wifi-probe/

	$(INSTALL_DIR) $(1)/www/luci-static/resources/view
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/luci-static/resources/view/*.js $(1)/www/luci-static/resources/view/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/luci-static/resources/view/*.css $(1)/www/luci-static/resources/view/

	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/etc/uci-defaults/* $(1)/etc/uci-defaults/

	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/usr/share/rpcd/acl.d/*.json $(1)/usr/share/rpcd/acl.d/
endef

$(eval $(call BuildPackage,luci-app-wifi-probe))
