-- App Traffic Analyzer - Configuration
-- Copyright (C) 2024
-- Licensed under the Apache License 2.0

local fs = require "nixio.fs"

local m = Map("apptraffic", translate("App Traffic Analyzer - Configuration"),
    translate("Configure the App Traffic Analyzer daemon. This tool identifies " ..
              "which applications and websites are being accessed on your network " ..
              "by analyzing DNS queries, TLS SNI headers, and HTTP requests."))

local s = m:section(TypedSection, "apptraffic", translate("General Settings"))
s.anonymous = true
s.addremove = false

-- Enable/Disable
local enabled = s:option(Flag, "enabled", translate("Enable"),
    translate("Enable or disable the traffic analyzer daemon."))
enabled.default = enabled.enabled
enabled.rmempty = false

-- Interface selection
local iface = s:option(Value, "interface", translate("Capture Interface"),
    translate("Network interface to capture traffic on. Use 'any' to monitor all interfaces."))
iface.default = "br-lan"
iface.placeholder = "br-lan"
iface.rmempty = false

-- Database path
local db = s:option(Value, "database", translate("Database Directory"),
    translate("Directory to store the traffic database. One SQLite database file " ..
              "will be created here along with WAL journal files."))
db.default = "/var/lib/apptraffic"
db.placeholder = "/var/lib/apptraffic"
db.rmempty = false

-- Commit interval
local commit = s:option(ListValue, "commit_interval", translate("Commit Interval"),
    translate("How often traffic data is committed to disk. Shorter intervals " ..
              "reduce data loss risk but increase flash wear."))
commit:value("30", translate("30s - Frequent commits (less data loss risk)"))
commit:value("60", translate("60s - Normal (recommended)"))
commit:value("300", translate("5m - Reduced flash wear"))
commit:value("600", translate("10m - Minimal flash wear (more data loss risk)"))
commit:value("3600", translate("1h - Least frequent (highest data loss risk)"))
commit.default = "60"

-- DNS timeout
local dns_timeout = s:option(Value, "dns_timeout", translate("DNS Cache Timeout"),
    translate("How long (in seconds) IP-to-domain mappings are kept in memory. " ..
              "Longer timeouts improve detection accuracy but use more memory."))
dns_timeout.default = "3600"
dns_timeout.datatype = "uinteger"
dns_timeout.placeholder = "3600"

-- App Mapping File
local map_file = s:option(TextValue, "_mapping", translate("App Mapping Database"),
    translate("Domain-to-application mapping rules. Each line maps a domain pattern " ..
              "to an application name. Use * as wildcard. " ..
              "Format: domain_pattern,app_name,category,priority"))
map_file.rows = 30
map_file.cfgvalue = function(self, cfg)
    return fs.readfile("/usr/share/apptraffic/app-mapping.txt")
end
map_file.write = function(self, cfg, value)
    fs.writefile("/usr/share/apptraffic/app-mapping.txt",
        (value or ""):gsub("\r\n", "\n"))
end

return m
