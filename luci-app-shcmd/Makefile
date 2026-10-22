include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-shcmd
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=Apache-2.0
PKG_MAINTAINER:=OpenWrt Community
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-shcmd
  SECTION:=luci
  CATEGORY:=LuCI
  DEPENDS:=+luci-compat +luci-base +shcmd +wget
  TITLE:=shcmd - token-authenticated remote command runner
endef

define Package/luci-app-shcmd/description
  LuCI interface for the shcmd daemon. Provides a Run page with one button
  per whitelisted command, a status badge, and a Settings page for the
  token / bind address / port / allowlist.
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

define Package/luci-app-shcmd/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/controller
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/controller/*.lua $(1)/usr/lib/lua/luci/controller/

	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/model/cbi/shcmd
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/model/cbi/shcmd/*.lua $(1)/usr/lib/lua/luci/model/cbi/shcmd/

	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/view/shcmd
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/view/shcmd/*.htm $(1)/usr/lib/lua/luci/view/shcmd/

	$(INSTALL_DIR) $(1)/www/luci-static/resources/view
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/luci-static/resources/view/*.js $(1)/www/luci-static/resources/view/
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/luci-static/resources/view/*.css $(1)/www/luci-static/resources/view/

	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/etc/uci-defaults/* $(1)/etc/uci-defaults/

	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/usr/share/rpcd/acl.d/*.json $(1)/usr/share/rpcd/acl.d/
endef

$(eval $(call BuildPackage,luci-app-shcmd))
