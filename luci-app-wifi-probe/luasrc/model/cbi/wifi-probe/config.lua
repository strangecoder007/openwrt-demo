-- WiFi Probe - Configuration
-- Licensed under Apache License 2.0

local m = Map("wifi-probe", translate("WiFi Probe - Configuration"),
    translate("Configure the 802.11 probe daemon. It captures Probe Request / " ..
              "Beacon frames on a monitor interface and aggregates device visits."))

local s = m:section(TypedSection, "wifi-probe", translate("General Settings"))
s.anonymous = true
s.addremove = false

local enabled = s:option(Flag, "enabled", translate("Enable"),
    translate("Enable the WiFi probe daemon."))
enabled.default = enabled.enabled
enabled.rmempty = false

local i = s:option(Value, "interface", translate("Interface"),
    translate("Monitor interface to capture on. Default mon0."))
i.default = "mon0"
i.rmempty = false

local g = s:option(Value, "session_gap", translate("Session gap (s)"),
    translate("Seconds without a probe to close a visit."))
g.default = 300
g.rmempty = false
g.datatype = "uinteger"

local n = s:option(Value, "rssi_near", translate("RSSI near (dBm)"),
    translate("Signal >= this value counts as near."))
n.default = "-55"
n.rmempty = false

local md = s:option(Value, "rssi_mid", translate("RSSI mid (dBm)"),
    translate("Signal >= this value (and below near) counts as mid."))
md.default = "-65"
md.rmempty = false

local r = s:option(Value, "retention_days", translate("Retention (days)"),
    translate("Drop visits older than this many days."))
r.default = 30
r.rmempty = false
r.datatype = "uinteger"

local a = s:option(Flag, "anonymize_mac", translate("Anonymize MAC"),
    translate("Hash device MAC before storing."))
a.default = a.enabled
a.rmempty = false

local f = s:option(ListValue, "capture_filter", translate("Capture filter"),
    translate("all = beacon + probe; probe = probe only."))
f:value("all", translate("Beacon + Probe"))
f:value("probe", translate("Probe only"))
f.default = "all"
f.rmempty = false

return m
