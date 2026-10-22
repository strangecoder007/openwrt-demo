-- shcmd - CBI settings page
-- Licensed under Apache License 2.0
--
-- IMPORTANT: /etc/shcmd.conf is a flat "directive value" file, NOT a UCI
-- config. It has no "config <type>" lines, so a Map + TypedSection renders as
-- "no configuration yet" and shows no fields at all. This page therefore uses
-- SimpleForm and lets each field own its read/write callbacks.
--
-- Never require "luci.fs" here: the module does not exist on 19.07 and the
-- resulting error takes the entire page down.
--
-- The command whitelist is deliberately NOT editable from the web UI. Adding
-- a command is a security decision that should require shell access; a form
-- that can add arbitrary argv vectors would turn any LuCI session into a
-- shell.

local CONF = "/etc/shcmd.conf"

-- luci.sys exists on 19.07 (unlike luci.fs) and is what we use to restart the
-- daemon once the form is committed.
local sys = require "luci.sys"

local function read_lines()
	local out = {}
	local f = io.open(CONF, "r")
	if not f then return out end
	for line in f:lines() do
		out[#out + 1] = line
	end
	f:close()
	return out
end

local function write_lines(lines)
	local f = io.open(CONF, "w")
	if not f then return false end
	for _, line in ipairs(lines) do
		f:write(line, "\n")
	end
	f:close()
	return true
end

local function get_directive(name, default)
	local pat = "^%s*" .. name .. "%s+(.-)%s*$"
	for _, line in ipairs(read_lines()) do
		local v = line:match(pat)
		if v then
			return (v:gsub('^"(.*)"$', "%1"))
		end
	end
	return default
end

-- Replace a directive in place, preserving every other line verbatim
-- (comments and the command whitelist included). Duplicates are collapsed.
local function set_directive(directive, value)
	local out, replaced = {}, false
	for _, line in ipairs(read_lines()) do
		local d = line:match("^%s*([%w_%-]+)")
		if d == directive then
			if not replaced then
				out[#out + 1] = directive .. " " .. value
				replaced = true
			end
		else
			out[#out + 1] = line
		end
	end
	if not replaced then
		out[#out + 1] = directive .. " " .. value
	end
	write_lines(out)
end

local function get_allows()
	local out = {}
	for _, line in ipairs(read_lines()) do
		local v = line:match("^%s*allow%s+(%S+)%s*$")
		if v then out[#out + 1] = v end
	end
	return out
end

local function set_allows(list)
	local kept = {}
	for _, line in ipairs(read_lines()) do
		if not line:match("^%s*allow%s+") then
			kept[#kept + 1] = line
		end
	end
	for _, v in ipairs(list or {}) do
		v = tostring(v):gsub("%s", "")
		if v ~= "" then
			kept[#kept + 1] = "allow " .. v
		end
	end
	write_lines(kept)
end

m = SimpleForm("shcmd", translate("Remote Commands"),
	translate("Token-authenticated HTTP endpoint that runs a fixed list of " ..
		  "shell commands. The command whitelist lives in the config file " ..
		  "and is deliberately not editable from this page."))

m.reset = false
m.submit = translate("Save and apply")

s = m:section(SimpleSection, translate("Endpoint"))

-- ------------------------------------------------------------------ token
local token = s:option(Value, "token", translate("Token"),
	translate("Shared secret. Requests without it are refused. " ..
		  "Leave the masked value alone to keep the current one."))
token.password = true
token.rmempty = false
token.cfgvalue = function(self, section)
	local t = get_directive("token", "")
	-- Never render the real secret back into page source.
	if t ~= "" and not t:find("^CHANGE%-ME") then
		return "********"
	end
	return t
end
token.validate = function(self, value)
	if value and value ~= "********" and value:find('"') then
		return nil, translate("The token must not contain a double quote.")
	end
	return value
end
token.write = function(self, section, value)
	if not value or value == "" or value == "********" then
		return
	end
	set_directive("token", '"' .. value .. '"')
end

-- ----------------------------------------------------------------- listen
local listen = s:option(Value, "listen", translate("Bind address"))
listen.placeholder = "0.0.0.0"
listen.rmempty = false
listen.cfgvalue = function(self, section)
	return get_directive("listen", "0.0.0.0")
end
listen.write = function(self, section, value)
	if value and value ~= "" then
		set_directive("listen", value)
	end
end

-- ------------------------------------------------------------------- port
local port = s:option(Value, "port", translate("Port"))
port.placeholder = "8099"
port.datatype = "port"
port.rmempty = false
port.cfgvalue = function(self, section)
	return get_directive("port", "8099")
end
port.write = function(self, section, value)
	if value and value ~= "" then
		set_directive("port", value)
	end
end

-- --------------------------------------------------------------- max_args
local maxargs = s:option(Value, "max_args", translate("Max extra arguments"))
maxargs.placeholder = "4"
maxargs.datatype = "range(0,16)"
maxargs.rmempty = false
maxargs.cfgvalue = function(self, section)
	return get_directive("max_args", "4")
end
maxargs.write = function(self, section, value)
	if value and value ~= "" then
		set_directive("max_args", value)
	end
end

-- ------------------------------------------------------------------ allow
local allow = s:option(DynamicList, "allow", translate("Allowed source networks"),
	translate("CIDR list. An empty list accepts any source, which is not " ..
		  "recommended when the port is reachable from outside the LAN."))
allow.cfgvalue = function(self, section)
	return get_allows()
end
allow.write = function(self, section, value)
	set_allows(value)
end

-- Restart the daemon after the form is committed, so new values take effect.
-- procd will not notice a config change on its own.
local orig_parse = m.parse
m.parse = function(self, ...)
	orig_parse(self, ...)
	sys.call("/etc/init.d/shcmd restart >/dev/null 2>&1")
end

return m
