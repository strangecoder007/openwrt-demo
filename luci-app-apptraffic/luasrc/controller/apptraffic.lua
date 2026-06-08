-- App Traffic Analyzer - LuCI Controller
-- Copyright (C) 2024
-- Licensed under the Apache License 2.0

module("luci.controller.apptraffic", package.seeall)

function index()
    entry({"admin", "apptraffic"}, firstchild(), _("App Traffic"), 85)
    entry({"admin", "apptraffic", "display"}, template("apptraffic/display"), _("Traffic Analysis"), 1)
    entry({"admin", "apptraffic", "config"}, cbi("apptraffic/config"), _("Configuration"), 2)
    entry({"admin", "apptraffic", "data"}, call("action_data"), nil, 3)
    entry({"admin", "apptraffic", "top_apps"}, call("action_top_apps"), nil, 4)
    entry({"admin", "apptraffic", "top_domains"}, call("action_top_domains"), nil, 5)
    entry({"admin", "apptraffic", "top_hosts"}, call("action_top_hosts"), nil, 6)
    entry({"admin", "apptraffic", "live"}, call("action_live"), nil, 7)
    entry({"admin", "apptraffic", "device_apps"}, call("action_device_apps"), nil, 8)
end

local function exec_cmd(cmd)
    local handle = io.popen(cmd)
    if not handle then
        return nil
    end
    local result = handle:read("*a")
    handle:close()
    return result
end

function action_data()
    local http = require "luci.http"
    local format = http.formvalue("format") or "json"
    local group_by = http.formvalue("group_by") or "app"
    local period = http.formvalue("period") or "today"

    local cmd = "/usr/sbin/apptraffic -c " .. format
        .. " -g " .. group_by
        .. " -t " .. period

    local data = exec_cmd(cmd)

    if data then
        if format == "json" then
            http.prepare_content("application/json")
        else
            http.prepare_content("text/csv")
            http.header("Content-Disposition", "attachment; filename=\"traffic.csv\"")
        end
        http.write(data)
    else
        http.status(500, "Unable to fetch traffic data")
    end
end

function action_top_apps()
    local http = require "luci.http"
    local period = http.formvalue("period") or "today"
    http.prepare_content("application/json")

    local data = exec_cmd("/usr/sbin/apptraffic -c json -g app -t " .. period)
    if data then
        http.write(data)
    else
        http.write('{"entries":[]}')
    end
end

function action_top_domains()
    local http = require "luci.http"
    local period = http.formvalue("period") or "today"
    http.prepare_content("application/json")

    local data = exec_cmd("/usr/sbin/apptraffic -c json -g domain -t " .. period)
    if data then
        http.write(data)
    else
        http.write('{"entries":[]}')
    end
end

function action_top_hosts()
    local http = require "luci.http"
    local period = http.formvalue("period") or "today"
    http.prepare_content("application/json")

    local data = exec_cmd("/usr/sbin/apptraffic -c json -g host -t " .. period)
    if data then
        http.write(data)
    else
        http.write('{"entries":[]}')
    end
end

function action_device_apps()
    local http = require "luci.http"
    local period = http.formvalue("period") or "today"
    http.prepare_content("application/json")

    local data = exec_cmd("/usr/sbin/apptraffic -c json -g host_app -t " .. period)
    if data then
        http.write(data)
    else
        http.write('{"entries":[]}')
    end
end

function action_live()
    local http = require "luci.http"
    http.prepare_content("application/json")

    -- Get live data (last 10 seconds)
    local data = exec_cmd("/usr/sbin/apptraffic -c json -g app -t 10")
    if data then
        http.write(data)
    else
        http.write('{"entries":[],"timestamp":0}')
    end
end
