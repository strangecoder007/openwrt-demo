/**
 * App Traffic Analyzer - Frontend JavaScript
 * Copyright (C) 2024
 * Licensed under the Apache License 2.0
 */

var AppTraffic = (function() {
    'use strict';

    var charts = {};
    var colorPalette = [
        '#36A2EB', '#FF6384', '#4BC0C0', '#FF9F40', '#9966FF',
        '#FFCD56', '#C9CBCF', '#7BC8A4', '#E8A87C', '#95E1D3',
        '#F38181', '#AA96DA', '#FCBAD3', '#A6D0DD', '#FFD4B2',
        '#8DDFCB', '#D0BFFF', '#FFC8A2', '#B5EAEA', '#FFABAB',
        '#60B99A', '#DF5E88', '#F6C065', '#6A7BC3', '#C44D58',
        '#45ADA8', '#E27A3F', '#786FA6', '#F19066', '#3DC1D3'
    ];

    function formatBytes(bytes) {
        if (bytes === undefined || bytes === null) return '0 B';
        bytes = parseInt(bytes);
        if (bytes >= 1099511627776) return (bytes / 1099511627776).toFixed(2) + ' TB';
        if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(2) + ' GB';
        if (bytes >= 1048576) return (bytes / 1048576).toFixed(2) + ' MB';
        if (bytes >= 1024) return (bytes / 1024).toFixed(1) + ' KB';
        return bytes + ' B';
    }

    function formatNumber(n) {
        if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
        if (n >= 1000) return (n / 1000).toFixed(1) + 'K';
        return n.toString();
    }

    function setText(id, v) {
        var el = document.getElementById(id);
        if (el) el.textContent = v;
    }

    function fetchData(endpoint, callback, extra) {
        var period = document.getElementById('period-select');
        var periodVal = period ? period.value : '3600';
        var url = L.url('admin/apptraffic/' + endpoint) + '?period=' + periodVal;
        if (extra) {
            url += '&' + extra;
        }

        var xhr = new XMLHttpRequest();
        xhr.open('GET', url, true);
        xhr.onload = function() {
            if (xhr.status === 200) {
                try {
                    var data = JSON.parse(xhr.responseText);
                    callback(data);
                } catch(e) {
                    console.error('Parse error:', e);
                    callback(null);
                }
            } else {
                callback(null);
            }
        };
        xhr.onerror = function() { callback(null); };
        xhr.send();
    }

    function updateSummary() {
        fetchData('top_apps', function(d) {
            var rx = 0, tx = 0, apps = 0;
            if (d && d.entries) {
                apps = d.entries.length;
                d.entries.forEach(function(e) {
                    rx += e.rx_bytes || 0;
                    tx += e.tx_bytes || 0;
                });
            }
            setText('sum-rx', formatBytes(rx));
            setText('sum-tx', formatBytes(tx));
            setText('sum-apps', formatNumber(apps));
        });
        fetchData('top_hosts', function(d) {
            setText('sum-devices', (d && d.entries) ? formatNumber(d.entries.length) : '0');
        });
    }

    function createPieChart(canvasId, data) {
        var canvas = document.getElementById(canvasId);
        if (!canvas) return;

        var ctx = canvas.getContext('2d');

        if (charts[canvasId]) {
            charts[canvasId].destroy();
        }

        if (!data || data.length === 0) {
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            ctx.font = '12px sans-serif';
            ctx.fillStyle = '#999';
            ctx.textAlign = 'center';
            ctx.fillText('No data available', canvas.width/2, canvas.height/2);
            return;
        }

        var total = data.reduce(function(s, d) { return s + d.value; }, 0);

        // Limit to top 10 slices, group the rest as "Other"
        var slices = data.slice(0, 10);
        var otherSum = 0;
        for (var i = 10; i < data.length; i++) {
            otherSum += data[i].value;
        }
        if (otherSum > 0) {
            slices.push({ label: 'Other (' + (data.length - 10) + ' more)', value: otherSum });
        }

        var colors = slices.map(function(_, i) { return colorPalette[i % colorPalette.length]; });

        // Use Chart.js if available, otherwise draw simple canvas pie
        if (typeof Chart !== 'undefined') {
            charts[canvasId] = new Chart(ctx, {
                type: 'pie',
                data: {
                    labels: slices.map(function(s) { return s.label; }),
                    datasets: [{
                        data: slices.map(function(s) { return s.value; }),
                        backgroundColor: colors,
                        borderWidth: 1
                    }]
                },
                options: {
                    responsive: false,
                    legend: {
                        display: true,
                        position: 'right',
                        labels: {
                            boxWidth: 12,
                            fontSize: 10,
                            padding: 4
                        }
                    },
                    tooltips: {
                        callbacks: {
                            label: function(item, obj) {
                                var val = obj.datasets[0].data[item.index];
                                var pct = total > 0 ? (val / total * 100).toFixed(1) : 0;
                                return ' ' + formatBytes(val) + ' (' + pct + '%)';
                            }
                        }
                    }
                }
            });
        } else {
            // Fallback: simple canvas pie chart
            drawSimplePie(ctx, canvas.width, canvas.height, slices, colors, total);
        }
    }

    function drawSimplePie(ctx, w, h, slices, colors, total) {
        ctx.clearRect(0, 0, w, h);

        var cx = w * 0.35, cy = h / 2, radius = Math.min(cx, cy) - 10;
        var startAngle = -Math.PI / 2;

        slices.forEach(function(slice, i) {
            var sliceAngle = (slice.value / total) * 2 * Math.PI;
            ctx.beginPath();
            ctx.moveTo(cx, cy);
            ctx.arc(cx, cy, radius, startAngle, startAngle + sliceAngle);
            ctx.closePath();
            ctx.fillStyle = colors[i];
            ctx.fill();
            ctx.strokeStyle = '#fff';
            ctx.lineWidth = 1;
            ctx.stroke();
            startAngle += sliceAngle;
        });

        // Draw legend
        var lx = w * 0.7, ly = 10;
        ctx.font = '9px sans-serif';
        slices.forEach(function(slice, i) {
            if (ly > h - 15) return;
            ctx.fillStyle = colors[i];
            ctx.fillRect(lx, ly, 8, 8);
            ctx.fillStyle = '#333';
            var label = slice.label.length > 15 ? slice.label.substr(0, 14) + '…' : slice.label;
            var pct = total > 0 ? (slice.value / total * 100).toFixed(1) : 0;
            ctx.fillText(label + ' (' + pct + '%)', lx + 12, ly + 8);
            ly += 13;
        });
    }

    function renderTable(tableId, entries, columns, formatFns) {
        var table = document.getElementById(tableId);
        if (!table) return;

        // Remove existing data rows
        var rows = table.querySelectorAll('.tr:not(.table-titles):not(.placeholder)');
        rows.forEach(function(r) { r.remove(); });

        var placeholder = table.querySelector('.placeholder');
        if (!entries || entries.length === 0) {
            if (placeholder) placeholder.style.display = '';
            return;
        }
        if (placeholder) placeholder.style.display = 'none';

        entries.forEach(function(entry, idx) {
            var row = document.createElement('div');
            row.className = 'tr';

            columns.forEach(function(col, i) {
                var cell = document.createElement('div');
                cell.className = 'td' + (col.align === 'right' ? ' right' : ' left');
                var val = entry[col.key];
                if (formatFns && formatFns[i]) {
                    cell.textContent = formatFns[i](val, entry);
                } else {
                    cell.textContent = val !== undefined ? val : '-';
                }
                row.appendChild(cell);
            });

            table.appendChild(row);
        });
    }

    function loadApps() {
        fetchData('top_apps', function(data) {
            if (!data || !data.entries) {
                document.querySelector('#tab-apps .placeholder').style.display = '';
                return;
            }

            var entries = data.entries;

            // Update KPIs
            document.getElementById('apps-count').textContent = formatNumber(entries.length);
            var totalRx = 0, totalTx = 0;
            entries.forEach(function(e) {
                totalRx += e.rx_bytes;
                totalTx += e.tx_bytes;
            });
            document.getElementById('apps-total-rx').textContent = formatBytes(totalRx);
            document.getElementById('apps-total-tx').textContent = formatBytes(totalTx);

            if (entries.length > 0) {
                document.getElementById('top-app-name').textContent = entries[0].app_name || entries[0].key || 'Unknown';
            }

            // Pie chart data
            var pieData = entries.map(function(e) {
                return { label: e.app_name || e.key || 'Unknown', value: e.total_bytes || (e.rx_bytes + e.tx_bytes) };
            });
            createPieChart('apps-pie', pieData);

            // Table
            var columns = [
                { key: 'app_name', align: 'left' },
                { key: 'app_category', align: 'left' },
                { key: 'connections', align: 'right' },
                { key: 'rx_bytes', align: 'right' },
                { key: 'tx_bytes', align: 'right' },
                { key: 'total_bytes', align: 'right' },
                { key: 'share', align: 'right' }
            ];
            var formatters = [
                function(v) { return v || 'Unknown'; },
                function(v) { return v || 'General'; },
                function(v) { return formatNumber(v); },
                function(v) { return formatBytes(v); },
                function(v) { return formatBytes(v); },
                function(v) { return formatBytes(v); },
                function(v, entry) {
                    var share = totalRx + totalTx > 0
                        ? ((entry.rx_bytes + entry.tx_bytes) / (totalRx + totalTx) * 100).toFixed(1)
                        : 0;
                    return share + '%';
                }
            ];
            renderTable('apps-table', entries, columns, formatters);
        });
    }

    function loadDomains() {
        fetchData('top_domains', function(data) {
            if (!data || !data.entries) {
                document.querySelector('#tab-domains .placeholder').style.display = '';
                return;
            }

            var entries = data.entries;

            document.getElementById('domains-count').textContent = formatNumber(entries.length);
            var totalRx = 0, totalTx = 0;
            entries.forEach(function(e) {
                totalRx += e.rx_bytes;
                totalTx += e.tx_bytes;
            });
            document.getElementById('domains-total-rx').textContent = formatBytes(totalRx);
            document.getElementById('domains-total-tx').textContent = formatBytes(totalTx);

            if (entries.length > 0) {
                document.getElementById('top-domain-name').textContent = entries[0].domain || entries[0].key || 'Unknown';
            }

            var pieData = entries.map(function(e) {
                return { label: e.domain || e.key || 'Unknown', value: e.total_bytes || (e.rx_bytes + e.tx_bytes) };
            });
            createPieChart('domains-pie', pieData);

            var columns = [
                { key: 'domain', align: 'left' },
                { key: 'app_name', align: 'left' },
                { key: 'connections', align: 'right' },
                { key: 'rx_bytes', align: 'right' },
                { key: 'tx_bytes', align: 'right' },
                { key: 'total_bytes', align: 'right' }
            ];
            var formatters = [
                function(v) { return v || 'Unknown'; },
                function(v) { return v || '-'; },
                function(v) { return formatNumber(v); },
                function(v) { return formatBytes(v); },
                function(v) { return formatBytes(v); },
                function(v) { return formatBytes(v); }
            ];
            renderTable('domains-table', entries, columns, formatters);
        });
    }

    function loadHosts() {
        fetchData('top_hosts', function(data) {
            if (!data || !data.entries) {
                document.querySelector('#tab-hosts .placeholder').style.display = '';
                return;
            }

            var entries = data.entries;

            document.getElementById('hosts-count').textContent = formatNumber(entries.length);
            var totalRx = 0, totalTx = 0;
            entries.forEach(function(e) {
                totalRx += e.rx_bytes;
                totalTx += e.tx_bytes;
            });
            document.getElementById('hosts-total-rx').textContent = formatBytes(totalRx);
            document.getElementById('hosts-total-tx').textContent = formatBytes(totalTx);

            if (entries.length > 0) {
                document.getElementById('top-host-name').textContent = entries[0].host || entries[0].src_ip || entries[0].key || 'Unknown';
            }

            var pieData = entries.map(function(e) {
                return { label: e.host || e.src_ip || e.key || 'Unknown', value: e.total_bytes || (e.rx_bytes + e.tx_bytes) };
            });
            createPieChart('hosts-pie', pieData);

            var columns = [
                { key: 'src_ip', align: 'left' },
                { key: 'app_name', align: 'left' },
                { key: 'app_category', align: 'left' },
                { key: 'connections', align: 'right' },
                { key: 'rx_bytes', align: 'right' },
                { key: 'tx_bytes', align: 'right' },
                { key: 'total_bytes', align: 'right' }
            ];
            var formatters = [
                function(v) { return v || 'Unknown'; },
                function(v) { return v || '-'; },
                function(v) { return v || 'General'; },
                function(v) { return formatNumber(v); },
                function(v) { return formatBytes(v); },
                function(v) { return formatBytes(v); },
                function(v) { return formatBytes(v); }
            ];
            renderTable('hosts-table', entries, columns, formatters);
        });
    }

    function loadDeviceApps() {
        fetchData('device_apps', function(data) {
            if (!data || !data.entries) {
                var ph = document.querySelector('#tab-device-apps .placeholder');
                if (ph) ph.style.display = '';
                return;
            }

            var entries = data.entries;

            // Group by src_ip, take top 5 apps per device
            var deviceMap = {};
            for (var i = 0; i < entries.length; i++) {
                var e = entries[i];
                var ip = e.src_ip || 'Unknown';
                if (!deviceMap[ip]) {
                    deviceMap[ip] = { src_ip: ip, apps: [], total_rx: 0, total_tx: 0 };
                }
                if (deviceMap[ip].apps.length < 5) {
                    deviceMap[ip].apps.push({
                        app_name: e.app_name || 'Unknown',
                        app_category: e.app_category || '',
                        rx_bytes: e.rx_bytes,
                        tx_bytes: e.tx_bytes,
                        total_bytes: e.total_bytes || (e.rx_bytes + e.tx_bytes)
                    });
                }
                deviceMap[ip].total_rx += e.rx_bytes;
                deviceMap[ip].total_tx += e.tx_bytes;
            }

            var keys = [];
            for (var k in deviceMap) {
                if (deviceMap.hasOwnProperty(k)) keys.push(k);
            }
            var deviceList = keys.map(function(k) {
                return deviceMap[k];
            }).sort(function(a, b) {
                return (b.total_rx + b.total_tx) - (a.total_rx + a.total_tx);
            });

            // Build flattened table rows
            var rows = [];
            for (var d = 0; d < deviceList.length; d++) {
                var dev = deviceList[d];
                rows.push({
                    _type: 'header',
                    src_ip: dev.src_ip,
                    total_rx: dev.total_rx,
                    total_tx: dev.total_tx,
                    total_bytes: dev.total_rx + dev.total_tx
                });
                for (var a = 0; a < dev.apps.length; a++) {
                    var app = dev.apps[a];
                    rows.push({
                        _type: 'app',
                        src_ip: '',
                        app_name: (a + 1) + '. ' + app.app_name,
                        app_category: app.app_category,
                        rx_bytes: app.rx_bytes,
                        tx_bytes: app.tx_bytes,
                        total_bytes: app.total_bytes
                    });
                }
            }

            // Render table
            var table = document.getElementById('device-apps-table');
            if (!table) return;

            // Remove old data rows (compatible with older browsers)
            var oldRows = table.querySelectorAll('.tr:not(.table-titles):not(.placeholder)');
            for (var r = oldRows.length - 1; r >= 0; r--) {
                oldRows[r].parentNode.removeChild(oldRows[r]);
            }

            var placeholder = table.querySelector('.placeholder');
            if (rows.length === 0) {
                if (placeholder) placeholder.style.display = '';
                return;
            }
            if (placeholder) placeholder.style.display = 'none';

            for (var ri = 0; ri < rows.length; ri++) {
                var row = rows[ri];
                var tr = document.createElement('div');
                tr.className = 'tr';

                if (row._type === 'header') {
                    tr.style.fontWeight = 'bold';
                    tr.style.backgroundColor = '#f5f5f5';
                    tr.style.borderTop = '2px solid #ccc';
                    addCell(tr, row.src_ip, 'left', true);
                    addCell(tr, '', 'left');
                    addCell(tr, '', 'right');
                    addCell(tr, formatBytes(row.total_rx), 'right', true);
                    addCell(tr, formatBytes(row.total_tx), 'right', true);
                    addCell(tr, formatBytes(row.total_bytes), 'right', true);
                } else {
                    addCell(tr, '', 'left');
                    addCell(tr, row.app_name, 'left');
                    addCell(tr, row.app_category, 'left');
                    addCell(tr, formatBytes(row.rx_bytes), 'right');
                    addCell(tr, formatBytes(row.tx_bytes), 'right');
                    addCell(tr, formatBytes(row.total_bytes), 'right');
                }
                table.appendChild(tr);
            }

            // Update KPI
            var countEl = document.getElementById('device-apps-count');
            if (countEl) countEl.textContent = formatNumber(deviceList.length);
        });

        function addCell(row, text, align, bold) {
            var cell = document.createElement('div');
            cell.className = 'td ' + (align === 'right' ? 'right' : 'left');
            if (bold) cell.style.fontWeight = 'bold';
            cell.textContent = text;
            row.appendChild(cell);
        }
    }

    function loadCategories() {
        fetchData('top_apps', function(data) {
            // For categories, we use the app data but group by category
            // In a full implementation, the backend would support category grouping directly
            if (!data || !data.entries) {
                document.querySelector('#tab-categories .placeholder').style.display = '';
                return;
            }

            var entries = data.entries;

            // Simple category aggregation (the backend stores categories)
            var catMap = {};
            entries.forEach(function(e) {
                var cat = e.app_category || 'General';
                if (!catMap[cat]) {
                    catMap[cat] = {
                        rx_bytes: 0, tx_bytes: 0, connections: 0, apps: 0
                    };
                }
                catMap[cat].rx_bytes += e.rx_bytes;
                catMap[cat].tx_bytes += e.tx_bytes;
                catMap[cat].connections += e.connections;
                catMap[cat].apps += 1;
            });

            var catEntries = Object.keys(catMap).map(function(k) {
                return {
                    category: k,
                    rx_bytes: catMap[k].rx_bytes,
                    tx_bytes: catMap[k].tx_bytes,
                    total_bytes: catMap[k].rx_bytes + catMap[k].tx_bytes,
                    connections: catMap[k].connections,
                    apps: catMap[k].apps
                };
            }).sort(function(a, b) { return b.total_bytes - a.total_bytes; });

            document.getElementById('categories-count').textContent = formatNumber(catEntries.length);
            if (catEntries.length > 0) {
                document.getElementById('top-category-name').textContent = catEntries[0].category;
            }

            var pieData = catEntries.map(function(e) {
                return { label: e.category, value: e.total_bytes };
            });
            createPieChart('categories-pie', pieData);

            var columns = [
                { key: 'category', align: 'left' },
                { key: 'apps', align: 'right' },
                { key: 'connections', align: 'right' },
                { key: 'rx_bytes', align: 'right' },
                { key: 'tx_bytes', align: 'right' },
                { key: 'total_bytes', align: 'right' }
            ];
            var formatters = [
                function(v) { return v || 'Unknown'; },
                function(v) { return formatNumber(v); },
                function(v) { return formatNumber(v); },
                function(v) { return formatBytes(v); },
                function(v) { return formatBytes(v); },
                function(v) { return formatBytes(v); }
            ];
            renderTable('categories-table', catEntries, columns, formatters);
        });
    }

    function drawLineChart(canvasId, points) {
        var canvas = document.getElementById(canvasId);
        if (!canvas) return;
        var ctx = canvas.getContext('2d');

        // Resize to the row's content width so the line is legible on wide screens
        var parent = canvas.parentElement;
        var cw = parent && parent.clientWidth ? parent.clientWidth : 820;
        if (cw < 500) cw = 500;
        canvas.width = cw;
        canvas.height = 340;

        ctx.clearRect(0, 0, canvas.width, canvas.height);

        if (!points || points.length === 0) {
            ctx.font = '12px sans-serif';
            ctx.fillStyle = '#999';
            ctx.textAlign = 'center';
            ctx.fillText('No data available', canvas.width / 2, canvas.height / 2);
            return;
        }

        var max = 1;
        points.forEach(function(p) {
            if ((p.rx || 0) > max) max = p.rx || 0;
            if ((p.tx || 0) > max) max = p.tx || 0;
        });
        var padL = 46, padT = 24, padR = 16, padB = 28;
        var w = canvas.width - padL - padR;
        var h = canvas.height - padT - padB;

        // horizontal grid
        ctx.strokeStyle = '#ddd';
        ctx.lineWidth = 1;
        ctx.beginPath();
        for (var i = 0; i <= 4; i++) {
            var y = padT + h - (h * i / 4);
            ctx.moveTo(padL, y);
            ctx.lineTo(padL + w, y);
        }
        ctx.stroke();

        function drawLine(key, color) {
            ctx.strokeStyle = color;
            ctx.lineWidth = 2;
            ctx.beginPath();
            for (var j = 0; j < points.length; j++) {
                var x = padL + (points.length === 1 ? 0 : (j / (points.length - 1)) * w);
                var yy = padT + h - ((points[j][key] || 0) / max) * h;
                if (j === 0) ctx.moveTo(x, yy); else ctx.lineTo(x, yy);
            }
            ctx.stroke();
        }
        drawLine('rx', '#36A2EB');
        drawLine('tx', '#4BC0C0');

        // y-axis max
        ctx.font = '10px sans-serif';
        ctx.fillStyle = '#666';
        ctx.textAlign = 'left';
        ctx.fillText(formatBytes(max), padL, padT - 6);

        // x-axis start/end time (HH:MM)
        function fmt(ts) {
            var d = new Date(ts * 1000);
            function p2(n) { return (n < 10 ? '0' : '') + n; }
            return p2(d.getHours()) + ':' + p2(d.getMinutes());
        }
        ctx.fillStyle = '#999';
        ctx.textAlign = 'left';
        ctx.fillText(fmt(points[0].ts), padL, canvas.height - 8);
        ctx.textAlign = 'right';
        ctx.fillText(fmt(points[points.length - 1].ts), padL + w, canvas.height - 8);
    }

    function loadLive() {
        fetchData('timeseries', function(data) {
            var ph = document.querySelector('#tab-live .placeholder');
            if (!data || !data.entries || data.entries.length === 0) {
                if (ph) ph.style.display = '';
                return;
            }
            if (ph) ph.style.display = 'none';

            var entries = data.entries;

            // per-bucket RX/TX (for the line chart)
            var bucketMap = {};
            entries.forEach(function(e) {
                var ts = parseInt(e.ts);
                if (!bucketMap[ts]) bucketMap[ts] = { rx: 0, tx: 0 };
                bucketMap[ts].rx += e.rx || 0;
                bucketMap[ts].tx += e.tx || 0;
            });
            var tsKeys = Object.keys(bucketMap).map(Number).sort(function(a, b) { return a - b; });
            var points = tsKeys.map(function(t) {
                return { ts: t, rx: bucketMap[t].rx, tx: bucketMap[t].tx };
            });
            drawLineChart('live-chart', points);
            var bucketsEl = document.getElementById('live-buckets');
            if (bucketsEl) bucketsEl.textContent = tsKeys.length;

            // aggregate per device x app over the window
            var agg = {};
            entries.forEach(function(e) {
                var k = (e.src_ip || '?') + '|' + (e.app || '?');
                if (!agg[k]) {
                    agg[k] = { src_ip: e.src_ip || '?', app: e.app || '?',
                               cat: e.category || '', rx: 0, tx: 0, conn: 0 };
                }
                agg[k].rx += e.rx || 0;
                agg[k].tx += e.tx || 0;
                agg[k].conn += e.conn || 0;
            });
            var rows = Object.keys(agg).map(function(k) {
                var a = agg[k];
                a.total = a.rx + a.tx;
                return a;
            }).sort(function(a, b) { return b.total - a.total; }).slice(0, 20);

            var columns = [
                { key: 'src_ip', align: 'left' },
                { key: 'app', align: 'left' },
                { key: 'cat', align: 'left' },
                { key: 'rx', align: 'right' },
                { key: 'tx', align: 'right' },
                { key: 'total', align: 'right' }
            ];
            var fns = [
                function(v) { return v || '?'; },
                function(v) { return v || '?'; },
                function(v) { return v || '-'; },
                function(v) { return formatBytes(v); },
                function(v) { return formatBytes(v); },
                function(v) { return formatBytes(v); }
            ];
            renderTable('live-table', rows, columns, fns);
        });
    }

    function loadAlerts() {
        var mbInput = document.getElementById('alert-mb-input');
        var mb = mbInput ? mbInput.value : '200';

        fetchData('alerts', function(data) {
            var ph = document.querySelector('#tab-alerts .placeholder');
            if (!data || !data.entries) {
                if (ph) ph.style.display = '';
                return;
            }
            if (ph) ph.style.display = 'none';

            var rows = data.entries;
            var countEl = document.getElementById('alerts-count');
            if (countEl) countEl.textContent = formatNumber(rows.length);

            var columns = [
                { key: 'src_ip', align: 'left' },
                { key: 'app', align: 'left' },
                { key: 'category', align: 'left' },
                { key: 'total_bytes', align: 'right' },
                { key: 'conn', align: 'right' }
            ];
            var fns = [
                function(v) { return v || '?'; },
                function(v) { return v || '?'; },
                function(v) { return v || '-'; },
                function(v) { return formatBytes(v); },
                function(v) { return formatNumber(v); }
            ];
            renderTable('alerts-table', rows, columns, fns);
        }, 'alert_mb=' + mb);
    }

    function loadCurrentTab() {
        var activeTab = document.querySelector('#apptraffic-tabs .cbi-tab.active');
        var tabName = activeTab ? activeTab.getAttribute('data-tab') : 'apps';

        switch(tabName) {
            case 'apps': loadApps(); break;
            case 'domains': loadDomains(); break;
            case 'hosts': loadHosts(); break;
            case 'device-apps': loadDeviceApps(); break;
            case 'categories': loadCategories(); break;
            case 'live': loadLive(); break;
            case 'alerts': loadAlerts(); break;
        }

        var now = new Date();
        document.getElementById('last-update').textContent =
            'Last update: ' + now.toLocaleTimeString();
    }

    function switchTab(tabName) {
        // Update tab styling
        document.querySelectorAll('#apptraffic-tabs .cbi-tab').forEach(function(t) {
            t.classList.remove('active');
        });
        var targetTab = document.querySelector('#apptraffic-tabs .cbi-tab[data-tab="' + tabName + '"]');
        if (targetTab) targetTab.classList.add('active');

        // Show/hide content
        document.querySelectorAll('.apptraffic-tab-content').forEach(function(c) {
            c.style.display = 'none';
        });
        var targetContent = document.getElementById('tab-' + tabName);
        if (targetContent) targetContent.style.display = '';

        loadCurrentTab();
    }

    function init() {
        // Tab switching
        document.querySelectorAll('#apptraffic-tabs .cbi-tab').forEach(function(tab) {
            tab.addEventListener('click', function() {
                switchTab(this.getAttribute('data-tab'));
            });
        });

        // Period selector
        var periodSelect = document.getElementById('period-select');
        if (periodSelect) {
            periodSelect.addEventListener('change', function() {
                loadCurrentTab();
                updateSummary();
            });
        }

        // Refresh button
        var refreshBtn = document.getElementById('refresh-btn');
        if (refreshBtn) {
            refreshBtn.addEventListener('click', function() {
                loadCurrentTab();
                updateSummary();
            });
        }

        // Alert threshold apply button
        var alertBtn = document.getElementById('alert-apply-btn');
        if (alertBtn) {
            alertBtn.addEventListener('click', function() {
                loadAlerts();
            });
        }

        // Load initial data
        switchTab('apps');
        updateSummary();

        // Auto-refresh every 30 seconds
        setInterval(function() {
            if (document.visibilityState === 'visible') {
                loadCurrentTab();
                updateSummary();
            }
        }, 30000);
    }

    return {
        init: init,
        loadApps: loadApps,
        loadDomains: loadDomains,
        loadHosts: loadHosts,
        loadCategories: loadCategories,
        formatBytes: formatBytes
    };
})();
