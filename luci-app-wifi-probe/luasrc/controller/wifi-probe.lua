-- WiFi Probe - LuCI Controller
-- Licensed under Apache License 2.0

module("luci.controller.wifi-probe", package.seeall)

function index()
    entry({"admin", "network", "wifi_probe"}, firstchild(), _("WiFi Probe"), 60).dependent = false
    entry({"admin", "network", "wifi_probe", "display"}, template("wifi-probe/display"), _("Overview"), 1)
    entry({"admin", "network", "wifi_probe", "config"}, cbi("wifi-probe/config"), _("Configuration"), 2)
    entry({"admin", "network", "wifi_probe", "data"}, call("action_data"), nil, 3)
end

local function exec_cmd(cmd)
    local h = io.popen(cmd)
    if not h then return nil end
    local r = h:read("*a")
    h:close()
    return r
end

local function whitelist(v, ok, def)
    if v and ok[v] then return v end
    return def
end

function action_data()
    local http = require "luci.http"
    local groups = { device = true, ssid = true, rssi = true, visits = true }
    local periods = { today = true, yesterday = true, week = true, month = true }
    local g = whitelist(http.formvalue("group_by"), groups, "device")
    local p = whitelist(http.formvalue("period"), periods, "today")
    http.prepare_content("application/json")
    local data = exec_cmd("/usr/sbin/wifi-probe -c json -g " .. g .. " -t " .. p)
    if data then
        http.write(data)
    else
        http.write('{"group":"' .. g .. '","entries":[]}')
    end
end
