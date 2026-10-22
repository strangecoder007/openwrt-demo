-- shcmd - LuCI controller
-- Licensed under Apache License 2.0
--
-- Design notes:
--
--   The daemon binds a loopback-reachable TCP port and requires a token. The
--   browser must never see that token, so this controller is a thin proxy:
--
--       browser -> LuCI (authenticated by session) -> wget -> 127.0.0.1:8099
--
--   Consequences worth remembering:
--     * We re-validate the command name against the daemon's own index before
--       proxying. Never interpolate a client-supplied name straight into a
--       URL: "&" or "%26" inside it would let a caller smuggle extra query
--       parameters.
--     * Arguments are percent-encoded here, not in the browser, so the
--       browser cannot inject "a=...&k=<its own token>".
--     * /etc/shcmd.conf is read with io.open, not uci: it is a flat
--       directive file, not a UCI config.

module("luci.controller.shcmd", package.seeall)

local http = require "luci.http"
local sys  = require "luci.sys"

-- Note: there is no "luci.fs" module on 19.07 -- requiring it makes the whole
-- controller fail to load and uhttpd returns 500 for every route. File tests
-- go through luci.sys / io.open instead.

local CONF      = "/etc/shcmd.conf"
local INITS     = "/etc/init.d/shcmd"

-- Keep in sync with the daemon's MAX_OUT. Reading more than this risks
-- interpolating a truncated JSON document.
local MAX_BODY  = 96 * 1024

function index()
	local page = entry({"admin", "system", "shcmd"}, firstchild(), _("Remote Commands"), 88)
	page.dependent = false
	page.acl_depends = { "luci-app-shcmd" }

	entry({"admin", "system", "shcmd", "run"}, template("shcmd/run"), _("Run"), 1)
	entry({"admin", "system", "shcmd", "list"}, call("action_list"), nil, 2)
	entry({"admin", "system", "shcmd", "exec"}, call("action_exec"), nil, 3)
	entry({"admin", "system", "shcmd", "status"}, call("action_status"), nil, 4)
	-- SimpleForm models must be mounted with form(), not cbi(); dispatching a
	-- SimpleForm through cbi() logs a warning and skips the save handler.
	entry({"admin", "system", "shcmd", "config"}, form("shcmd/config"), _("Settings"), 5)
	entry({"admin", "system", "shcmd", "upload"}, call("action_upload"), nil, 6)
	entry({"admin", "system", "shcmd", "installed"}, call("action_installed"), nil, 7)
	entry({"admin", "system", "shcmd", "uploadinfo"}, call("action_uploadinfo"), nil, 8)
end

----------------------------------------------------------------------
-- config file parsing
----------------------------------------------------------------------

-- Parse a quoted-or-bare token from a directive line. Mirrors the daemon's
-- own parser closely enough that both agree on what a token is.
local function split_line(line)
	local out, i, n = {}, 1, #line
	while i <= n do
		local c = line:sub(i, i)
		if c == "#" then
			break
		elseif c == " " or c == "\t" then
			i = i + 1
		elseif c == '"' then
			local j = line:find('"', i + 1, true)
			if not j then break end
			out[#out + 1] = line:sub(i + 1, j - 1)
			i = j + 1
		else
			local j = line:find("[ \t]", i)
			if not j then
				out[#out + 1] = line:sub(i)
				break
			end
			out[#out + 1] = line:sub(i, j - 1)
			i = j
		end
	end
	return out
end

local function read_conf()
	local f = io.open(CONF, "r")
	if not f then return nil end
	local conf = { tokens = {}, commands = {}, port = 8099, listen = "0.0.0.0", allows = {} }
	for line in f:lines() do
		local t = split_line(line)
		local d = t[1]
		if d == "token" and t[2] then
			conf.tokens[#conf.tokens + 1] = t[2]
		elseif d == "port" and t[2] then
			conf.port = tonumber(t[2]) or 8099
		elseif d == "listen" and t[2] then
			conf.listen = t[2]
		elseif d == "allow" and t[2] then
			conf.allows[#conf.allows + 1] = t[2]
		elseif d == "command" and t[2] then
			conf.commands[#conf.commands + 1] = {
				name = t[2],
				timeout = tonumber(t[3]) or 10,
				prog = t[4] or "",
			}
		end
	end
	f:close()
	return conf
end

-- The daemon refuses every request when no token is configured. The UI has to
-- know that so it can say so instead of showing a bare 401.
local function active_token(conf)
	if not conf then return nil end
	for _, t in ipairs(conf.tokens) do
		if t ~= "" and t:find("^CHANGE%-ME") == nil then
			return t
		end
	end
	return nil
end

----------------------------------------------------------------------
-- helpers
----------------------------------------------------------------------

local function json_str(s)
	if s == nil then return '""' end
	s = tostring(s)
	s = s:gsub('[%z\1-\31\\"]', function(ch)
		if ch == '"' then return '\\"'
		elseif ch == '\\' then return '\\\\'
		elseif ch == '\n' then return '\\n'
		elseif ch == '\r' then return '\\r'
		elseif ch == '\t' then return '\\t'
		else return string.format('\\u%04x', ch:byte()) end
	end)
	return '"' .. s .. '"'
end

local function urlenc(s)
	s = tostring(s)
	return (s:gsub("[^%w%-%._~]", function(c)
		return string.format("%%%02X", c:byte())
	end))
end

local function shellq(s)
	return "'" .. tostring(s):gsub("'", "'\\''") .. "'"
end

local function json_out(payload)
	http.prepare_content("application/json")
	http.write(payload)
end

local function fail(code, msg)
	http.status(code, msg)
	json_out(string.format('{"ok":false,"error":%s}', json_str(msg)))
end

----------------------------------------------------------------------
-- proxy to the daemon over loopback
----------------------------------------------------------------------

-- Returns the raw JSON body, or nil plus a reason.
--
-- `path` is the endpoint ("c" or "r"); `query` is the parameter string
-- WITHOUT a leading "?" and may be nil. The daemon splits on the first '?',
-- so appending "&k=..." directly to a bare path would make the whole thing
-- look like a path and the token would never be parsed -- which is exactly
-- what produced "auth failure from client".
local function daemon_get(path, query)
	local conf = read_conf()
	local token = active_token(conf)
	if not token then
		return nil, "no-usable-token"
	end

	local qs = (query and query ~= "") and (query .. "&k=" .. urlenc(token))
					   or ("k=" .. urlenc(token))
	local url = string.format("http://127.0.0.1:%d/%s?%s",
				  conf.port, path, qs)

	-- GNU wget on this image (not busybox): -q quiet, -O - to stdout,
	-- -T connect/read timeout, -t 1 no retry. stderr is dropped because a
	-- failed connection would otherwise be spliced into the JSON.
	local cmd = string.format(
		"%s -q -T 5 -t 1 -O - %s 2>/dev/null | head -c %d",
		shellq("/usr/bin/wget"), shellq(url), MAX_BODY)

	local body = sys.exec(cmd)
	if not body or body == "" then
		return nil, "daemon-unreachable"
	end
	return body, nil
end

----------------------------------------------------------------------
-- actions
----------------------------------------------------------------------

function action_list()
	-- Proxy the daemon's own index instead of re-deriving it from the config
	-- file. Keeping one source of truth for the metadata (desc / danger /
	-- arg labels) means the UI cannot drift from what the daemon will accept.
	local body, err = daemon_get("c", nil)
	if body then
		http.prepare_content("application/json")
		http.write(body)
		return
	end

	-- Daemon unreachable: still tell the UI what is configured, minus the
	-- runtime metadata.
	local conf = read_conf()
	local rows = {}
	if conf then
		for _, c in ipairs(conf.commands) do
			rows[#rows + 1] = string.format(
				'{"name":%s,"timeout":%d,"desc":"","danger":false,"args":[]}',
				json_str(c.name), c.timeout)
		end
	end

	json_out(string.format(
		'{"ok":false,"error":%s,"port":%d,"listen":%s,"has_token":%s,"commands":[%s]}',
		json_str(err or "daemon-unreachable"),
		conf and conf.port or 8099,
		json_str(conf and conf.listen or "0.0.0.0"),
		(active_token(conf) and "true" or "false"),
		table.concat(rows, ",")))
end

function action_status()
	-- Ask init rather than scanning /proc: that is the state the user cares
	-- about, and busybox pgrep is unreliable on this image.
	local out = sys.exec(shellq(INITS) .. " running >/dev/null 2>&1; echo $?")
	local running = (out and out:gsub("%s", "") == "0")

	local conf = read_conf()
	json_out(string.format('{"ok":true,"running":%s,"has_token":%s}',
		running and "true" or "false",
		(active_token(conf) and "true" or "false")))
end

function action_exec()
	local name = http.formvalue("n")
	if not name or name == "" then
		return fail(400, "missing parameter n")
	end

	-- Re-validate against the on-disk whitelist. This is the security-relevant
	-- step: only a name that the daemon itself knows can be proxied, and the
	-- value we forward is the copy read from the file, not the client's string.
	local conf = read_conf()
	local known = nil
	if conf then
		for _, c in ipairs(conf.commands) do
			if c.name == name then
				known = c.name
				break
			end
		end
	end
	if not known then
		return fail(404, "unknown command")
	end

	local parts = { "n=" .. urlenc(known) }

	-- Accept a<index>= arguments, same convention as the daemon.
	for i = 0, 15 do
		local v = http.formvalue(i == 0 and "a" or ("a" .. i))
		if v and v ~= "" then
			local key = (i == 0) and "a" or ("a" .. i)
			parts[#parts + 1] = key .. "=" .. urlenc(v)
		end
	end

	local body, err = daemon_get("r", table.concat(parts, "&"))
	if not body then
		if err == "no-usable-token" then
			return fail(503, "shcmd has no usable token configured")
		end
		return fail(503, "shcmd daemon is not reachable on loopback")
	end

	http.prepare_content("application/json")
	http.write(body)
end

----------------------------------------------------------------------
-- ipk upload
----------------------------------------------------------------------

-- Hard limits. /tmp is tmpfs with ~230 MB free on this board, but a package
-- upload has no business being large, and bounding it keeps a stray POST from
-- filling RAM.
local UPLOAD_MAX   = 8 * 1024 * 1024
local UPLOAD_DIR   = "/tmp"

function action_upload()
	-- luci.http has no recvfile() on 19.07, and http.content() returns empty
	-- for multipart because the dispatcher has already consumed the body.
	-- The supported path is setfilehandler(): LuCI parses the multipart body
	-- and calls the handler with (part, chunk, eof) for each file part.
	local saved_path, saved_bytes, saved_name, err = nil, 0, nil, nil
	local fh = nil

	http.setfilehandler(function(part, chunk, eof)
		-- Field names on this LuCI version: the uploaded filename lives in
		-- part.file (set from the Content-Disposition header), NOT in
		-- part.filename. Testing part.filename silently matched nothing and
		-- the whole handler body was skipped while bytes still streamed past.
		local fname = part and (part.file or part.filename) or nil

		if err then
			return
		end
		if not fname or fname == "" then
			return
		end

		if not fh then
			-- Extension gate on the SERVER side: only packages may be stored,
			-- otherwise an arbitrary executable could be parked in /tmp.
			saved_name = fname
			local lower = saved_name:lower()
			if not lower:match("%.ipk$") then
				err = "only .ipk files are accepted"
				return
			end

			-- The stored name is generated here and never taken from the
			-- client, so "../../etc/passwd" or a colliding name cannot
			-- decide where the bytes land.
			--
			-- Do NOT rely on math.random alone: Lua 5.1 does not seed it, so
			-- every process yields the same sequence (observed: both uploads
			-- got suffix 100000, meaning the second silently overwrote the
			-- first). os.time is second-granular, so a monotonic counter plus
			-- the PID is what actually guarantees uniqueness here.
			UPLOAD_SEQ = (UPLOAD_SEQ or 0) + 1
			local pid = tostring(sys.exec("echo $PPID") or "0"):gsub("%s", "")
			local uniq = string.format("%d-%d-%s",
						   os.time(), UPLOAD_SEQ, pid)
			saved_path = string.format("%s/upload-%s.ipk", UPLOAD_DIR, uniq)

			-- Refuse to clobber an existing file even if the name collides.
			local probe = io.open(saved_path, "rb")
			if probe then
				probe:close()
				err = "generated name already exists, retry"
				return
			end
			fh = io.open(saved_path, "wb")
			if not fh then
				err = "cannot write to " .. UPLOAD_DIR
				return
			end
		end

		if chunk and #chunk > 0 then
			saved_bytes = saved_bytes + #chunk
			if saved_bytes > UPLOAD_MAX then
				err = "upload larger than 8 MB"
				fh:close()
				fh = nil
				os.remove(saved_path)
				return
			end
			fh:write(chunk)
		end

		if eof then
			fh:close()
			fh = nil
		end
	end)

	-- Force LuCI to parse the body now, which drives the handler above.
	http.formvalue("file")

	if err then
		if saved_path then
			os.remove(saved_path)
		end
		return fail(400, err)
	end

	if not saved_path then
		return fail(400, "no file part in the request")
	end
	if saved_bytes == 0 then
		os.remove(saved_path)
		return fail(400, "uploaded file is empty")
	end

	json_out(string.format(
		'{"ok":true,"file":%s,"original":%s,"bytes":%d}',
		json_str(saved_path), json_str(saved_name or ""), saved_bytes))
end

-- Diagnostic: report what LuCI sees for the current request, so the upload
-- path can be debugged without guessing. Read-only and harmless.
function action_uploadinfo()
	local ctype = http.getenv("CONTENT_TYPE") or ""
	local clen  = http.getenv("CONTENT_LENGTH") or ""
	local method = http.getenv("REQUEST_METHOD") or ""

	-- Install a handler that only counts, then force parsing.
	local parts, bytes, names = 0, 0, {}
	http.setfilehandler(function(part, chunk, eof)
		local fname = part and (part.file or part.filename) or nil
		if fname then
			names[#names + 1] = string.format('{"name":%s,"chunk":%d,"eof":%s}',
				json_str(fname), chunk and #chunk or -1,
				eof and "true" or "false")
		end
		if chunk then
			bytes = bytes + #chunk
		end
		if eof then
			parts = parts + 1
		end
	end)

	-- Parse whatever form fields exist (this is what drives the handler).
	local fv = http.formvalue("file")

	json_out(string.format(
		'{"ok":true,"method":%s,"content_type":%s,"content_length":%s,'
		.. '"file_field_present":%s,"handler_calls":[%s],"handler_bytes":%d,'
		.. '"handler_eof":%d}',
		json_str(method), json_str(ctype), json_str(clen),
		(fv ~= nil) and "true" or "false",
		table.concat(names, ","), bytes, parts))
end

function action_installed()
	local files = {}
	local p = io.popen('ls -1t /tmp/upload-*.ipk 2>/dev/null | head -n 10')
	if p then
		for line in p:lines() do
			local size = 0
			local fh = io.open(line, "rb")
			if fh then
				size = fh:seek("end")
				fh:close()
			end
			files[#files + 1] = string.format('{"path":%s,"bytes":%d}',
							  json_str(line), size)
		end
		p:close()
	end

	json_out(string.format('{"ok":true,"uploads":[%s]}',
			       table.concat(files, ",")))
end
