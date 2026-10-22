(function() {
	'use strict';

	var BASE      = '/cgi-bin/luci/admin/system/shcmd/';
	var URL_LIST  = BASE + 'list';
	var URL_EXEC  = BASE + 'exec';
	var URL_STAT  = BASE + 'status';
	var URL_UPLOAD = BASE + 'upload';
	var URL_UPLOADS = BASE + 'installed';

	var commands = {};     /* name -> metadata from the daemon */
	var current  = null;
	var busy     = false;

	/* Client-side ceiling so a wedged request cannot spin forever. The
	 * daemon enforces its own per-command timeout; this is just a backstop. */
	var CLIENT_TIMEOUT_MS = 120000;

	function el(id) { return document.getElementById(id); }

	function esc(s) {
		return String(s == null ? '' : s)
			.replace(/&/g, '&amp;').replace(/</g, '&lt;')
			.replace(/>/g, '&gt;').replace(/"/g, '&quot;');
	}

	function setStatus(kind, text) {
		var s = el('sc-status');
		s.className = 'sc-status sc-' + kind;
		s.textContent = text;
	}

	function loadStatus() {
		return fetch(URL_STAT, { credentials: 'same-origin' })
			.then(function(r) { return r.json(); })
			.then(function(j) {
				if (!j.has_token) {
					setStatus('bad', 'No usable token in /etc/shcmd.conf - every request will be refused');
				} else if (j.running) {
					setStatus('ok', 'Daemon running');
				} else {
					setStatus('warn', 'Daemon not running (/etc/init.d/shcmd start)');
				}
			})
			.catch(function() { setStatus('bad', 'Status unavailable'); });
	}

	function loadCommands() {
		var box = el('sc-cmds');
		return fetch(URL_LIST, { credentials: 'same-origin' })
			.then(function(r) { return r.json(); })
			.then(function(j) {
				if (!j.ok || !j.commands || !j.commands.length) {
					box.innerHTML = '<div class="sc-empty">No commands configured. ' +
						'Add <code>command &lt;name&gt; &lt;timeout&gt; &lt;prog&gt; [args...]</code> ' +
						'lines to /etc/shcmd.conf.</div>';
					return;
				}

				commands = {};
				var html = '';
				j.commands.forEach(function(c) {
					commands[c.name] = c;
					var cls = 'sc-cmd' + (c.danger ? ' sc-danger' : '');
					var tip = (c.desc || c.name) + '  (timeout ' + c.timeout + 's)';
					html += '<button type="button" class="' + cls + '" data-name="' +
						esc(c.name) + '" title="' + esc(tip) + '">' +
						esc(c.name) +
						'<span class="sc-cmd-t">' + c.timeout + 's</span></button>';
				});
				box.innerHTML = html;

				Array.prototype.forEach.call(box.querySelectorAll('.sc-cmd'), function(b) {
					b.addEventListener('click', function() {
						select(b.getAttribute('data-name'));
					});
				});
			})
			.catch(function() {
				box.innerHTML = '<div class="sc-empty">Could not load the command list.</div>';
			});
	}

	/*
	 * Map a declared slot number to the request parameter name the daemon
	 * expects. The daemon's convention is: first extra argument is "a", then
	 * "a1", "a2", ... -- so slot N maps to "a(N-1)", NOT "aN". Getting this
	 * wrong silently drops every argument after the first.
	 */
	function slotKey(slot) {
		return (slot === 1) ? 'a' : ('a' + (slot - 1));
	}

	function select(name) {
		var meta = commands[name];
		if (!meta) return;

		Array.prototype.forEach.call(document.querySelectorAll('.sc-cmd'), function(b) {
			b.classList.toggle('sc-selected', b.getAttribute('data-name') === name);
		});

		current = meta;
		el('sc-args-for').textContent = name;
		el('sc-args-section').style.display = '';

		/* Description of the whole command. */
		el('sc-desc').textContent = meta.desc || '';

		var args = meta.args || [];

		if (!args.length) {
			el('sc-args').innerHTML =
				'<div class="sc-noargs">This command takes no arguments.</div>' +
				'<button type="button" class="cbi-button cbi-button-apply" id="sc-run">Run ' +
				esc(name) + '</button>';
			bindRun();
			el('sc-args-section').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
			return;
		}

		/*
		 * Render one labelled input per declared slot. The daemon documents
		 * slots with `arg <slot> <label> <example> [expect]`; using that here
		 * is the difference between "four blank boxes" and a form that tells
		 * you what to type.
		 */
		var html = '';
		args.forEach(function(a) {
			var key = slotKey(a.slot);
			html += '<label class="sc-arg">' +
				'<span class="sc-arg-label">' + esc(a.label) + '</span>' +
				'<input type="text" id="sc-in-' + key + '"' +
				' placeholder="' + esc(a.example) + '"' +
				' data-slot="' + a.slot + '" />' +
				(a.expect ? '<span class="sc-arg-hint">' + esc(a.expect) + '</span>' : '') +
				'</label>';
		});

		html += '<div class="sc-run-row">' +
			'<button type="button" class="cbi-button cbi-button-apply" id="sc-run">Run ' +
			esc(name) + '</button></div>';

		el('sc-args').innerHTML = html;
		bindRun();
		el('sc-args-section').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
	}

	function bindRun() {
		var btn = el('sc-run');
		if (btn) btn.addEventListener('click', run);
	}

	function collectArgs() {
		var out = [];
		var boxes = el('sc-args').querySelectorAll('input[data-slot]');
		Array.prototype.forEach.call(boxes, function(node) {
			var slot = parseInt(node.getAttribute('data-slot'), 10);
			if (node.value !== '') {
				out.push([slotKey(slot), node.value]);
			}
		});
		return out;
	}

	function run() {
		if (busy || !current) return;

		var missing = [];
		(current.args || []).forEach(function(a) {
			var node = el('sc-in-' + slotKey(a.slot));
			if (node && node.value === '' && a.label) missing.push(a.label);
		});

		/* Warn rather than block: some documented slots are optional. */
		if (current.danger) {
			var msg = 'Run "' + current.name + '"?\n\n' +
				(current.desc || '') +
				(missing.length ? '\n\nNote: these arguments are empty: ' + missing.join(', ') : '') +
				'\n\nThis command changes state.';
			if (!confirm(msg)) return;
		}

		busy = true;
		var btn = el('sc-run');
		if (btn) { btn.disabled = true; btn.textContent = 'Running...'; }

		var out = el('sc-out');
		out.className = 'sc-out sc-running';
		out.innerHTML = '<em class="sc-dim">Running ' + esc(current.name) + '...</em>';
		el('sc-meta').textContent = '';

		var body = 'n=' + encodeURIComponent(current.name);
		collectArgs().forEach(function(kv) {
			body += '&' + kv[0] + '=' + encodeURIComponent(kv[1]);
		});

		var started = Date.now();
		var timer = setTimeout(function() {
			out.className = 'sc-out sc-bad';
			out.textContent = 'No response after ' + (CLIENT_TIMEOUT_MS / 1000) +
				's. The daemon may be wedged; check /etc/init.d/shcmd status.';
			finish();
		}, CLIENT_TIMEOUT_MS);

		fetch(URL_EXEC, {
			method: 'POST',
			credentials: 'same-origin',
			headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
			body: body
		})
			.then(function(r) {
				return r.text().then(function(t) {
					return { status: r.status, text: t };
				});
			})
			.then(function(res) {
				clearTimeout(timer);
				var elapsed = ((Date.now() - started) / 1000).toFixed(2);
				render(res, elapsed);
			})
			.catch(function(err) {
				clearTimeout(timer);
				out.className = 'sc-out sc-bad';
				out.textContent = 'Request failed: ' + err;
			})
			.then(function() { finish(); });
	}

	function finish() {
		busy = false;
		var btn = el('sc-run');
		if (btn && current) {
			btn.disabled = false;
			btn.textContent = 'Run ' + current.name;
		}
	}

	function render(res, elapsed) {
		var out = el('sc-out');
		var meta = el('sc-meta');
		var j;

		try {
			j = JSON.parse(res.text);
		} catch (e) {
			out.className = 'sc-out sc-bad';
			out.textContent = 'Non-JSON response (HTTP ' + res.status + '):\n' + res.text;
			return;
		}

		if (!j.ok && j.error) {
			out.className = 'sc-out sc-bad';
			out.textContent = 'Error: ' + j.error + '  (HTTP ' + res.status + ')';
			meta.textContent = '';
			return;
		}

		var cls = 'sc-out';
		if (j.timed_out) cls += ' sc-warn';
		else if (j.status === 0) cls += ' sc-ok';
		else cls += ' sc-bad';
		out.className = cls;
		out.textContent = j.output !== undefined ? j.output : '(empty)';

		var bits = ['exit ' + j.status, elapsed + 's'];
		if (j.timed_out) bits.push('TIMED OUT');
		if (j.truncated) bits.push('output truncated at 64 KB');
		meta.textContent = bits.join('  \u00b7  ');
	}

	function loadUploads() {
		var box = el('sc-uploads');
		if (!box) return;
		return fetch(URL_UPLOADS, { credentials: 'same-origin' })
			.then(function(r) { return r.json(); })
			.then(function(j) {
				if (!j.ok || !j.uploads || !j.uploads.length) {
					box.innerHTML = '<div class="sc-dim">No uploaded packages.</div>';
					return;
				}
				var html = '<div class="sc-hint">Uploaded packages in /tmp:</div>';
				j.uploads.forEach(function(u) {
					html += '<div class="sc-upload-row">' +
						'<code>' + esc(u.path) + '</code>' +
						'<span class="sc-dim"> ' + u.bytes + ' B</span>' +
						'<button type="button" class="cbi-button cbi-button-apply sc-install" ' +
						'data-path="' + esc(u.path) + '">Install</button></div>';
				});
				box.innerHTML = html;
				Array.prototype.forEach.call(box.querySelectorAll('.sc-install'), function(b) {
					/*
					 * Capture the path now, not inside the handler. Referring
					 * to b.getAttribute(...) lazily inside a forEach callback
					 * is fine here, but storing it up front documents the
					 * intent and survives any later DOM re-render.
					 */
					var path = b.getAttribute('data-path');
					b.addEventListener('click', function() {
						installPath(path);
					});
				});
			})
			.catch(function() {
				box.innerHTML = '<div class="sc-dim">Could not list uploads.</div>';
			});
	}

	/* Jump to install-ipk with the uploaded path already filled in, so the
	 * user does not have to copy it by hand. */
	function installPath(path) {
		if (!commands['install-ipk']) {
			setStatus('warn', 'install-ipk is not in the command list');
			return;
		}

		select('install-ipk');

		/* select() rebuilds the argument inputs synchronously, so the element
		 * exists by the time it returns -- but only if the command actually
		 * declares slot 1. Fall back gracefully rather than throwing. */
		var input = el('sc-in-a');
		if (!input) {
			setStatus('warn', 'install-ipk has no argument slot to fill');
			return;
		}
		input.value = path;
		input.scrollIntoView({ behavior: 'smooth', block: 'center' });
		input.focus();
	}

	function doUpload() {
		var picker = el('sc-file');
		var status = el('sc-upload-status');
		if (!picker || !picker.files || !picker.files.length) {
			status.textContent = 'Choose a file first.';
			status.className = 'sc-upload-status sc-bad-text';
			return;
		}

		var file = picker.files[0];
		if (!/\.ipk$/i.test(file.name)) {
			status.textContent = 'Only .ipk files are accepted.';
			status.className = 'sc-upload-status sc-bad-text';
			return;
		}

		status.textContent = 'Uploading ' + file.name + ' (' + file.size + ' B)...';
		status.className = 'sc-upload-status';

		var fd = new FormData();
		fd.append('file', file, file.name);

		fetch(URL_UPLOAD, {
			method: 'POST',
			credentials: 'same-origin',
			body: fd
		})
			.then(function(r) {
				return r.text().then(function(t) {
					return { status: r.status, text: t };
				});
			})
			.then(function(res) {
				var j;
				try { j = JSON.parse(res.text); } catch (e) {
					status.textContent = 'Bad response (HTTP ' + res.status + ')';
					status.className = 'sc-upload-status sc-bad-text';
					return;
				}
				if (!j.ok) {
					status.textContent = 'Upload failed: ' + (j.error || res.status);
					status.className = 'sc-upload-status sc-bad-text';
					return;
				}
				status.textContent = 'Stored as ' + j.file + ' (' + j.bytes + ' B)';
				status.className = 'sc-upload-status sc-ok-text';
				picker.value = '';
				loadUploads();
			})
			.catch(function(err) {
				status.textContent = 'Upload error: ' + err;
				status.className = 'sc-upload-status sc-bad-text';
			});
	}

	window.ShCmd = {
		init: function() {
			el('sc-refresh').addEventListener('click', function() {
				loadStatus();
				loadCommands();
				loadUploads();
			});
			var ub = el('sc-upload');
			if (ub) ub.addEventListener('click', doUpload);
			loadStatus();
			loadCommands();
			loadUploads();
		}
	};
})();
