(function() {
    'use strict';

    var DATA_URL = '/cgi-bin/luci/admin/network/wifi_probe/data';

    function fmtTime(ts) {
        if (!ts) return '-';
        var d = new Date(ts * 1000);
        var p = function(x) { return (x < 10 ? '0' : '') + x; };
        return p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' +
               p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
    }

    function refresh() {
        var p = document.getElementById('wp-period').value;
        fetch(DATA_URL + '?group_by=device&period=' + p)
            .then(function(r) { return r.json(); })
            .then(function(j) { render(j.entries || []); })
            .catch(function() { render([]); });
    }

    function render(rows) {
        document.getElementById('wp-sum-dev').textContent = rows.length;
        var near = 0, mid = 0, far = 0;
        rows.forEach(function(row) {
            if (row.bin === 'near') near++;
            else if (row.bin === 'mid') mid++;
            else if (row.bin === 'far') far++;
        });
        document.getElementById('wp-sum-near').textContent = near;
        document.getElementById('wp-sum-mid').textContent = mid;
        document.getElementById('wp-sum-far').textContent = far;

        var t = document.getElementById('wp-table');
        var html = '<div class="tr table-titles">' +
            '<div class="th left">Device</div>' +
            '<div class="th right">First seen</div>' +
            '<div class="th right">Last seen</div>' +
            '<div class="th right">Visits</div>' +
            '<div class="th right">RSSI</div>' +
            '<div class="th left">SSIDs</div></div>';
        if (rows.length === 0) {
            html += '<div class="tr placeholder"><div class="td">No devices in this period.</div></div>';
        } else {
            rows.forEach(function(r) {
                html += '<div class="tr">' +
                    '<div class="td left">' + r.mac_key + '</div>' +
                    '<div class="td right">' + fmtTime(r.first) + '</div>' +
                    '<div class="td right">' + fmtTime(r.last) + '</div>' +
                    '<div class="td right">' + (r.visits || 0) + '</div>' +
                    '<div class="td right">' + (r.best_rssi || 0) + ' dBm</div>' +
                    '<div class="td left">' + (r.ssids || '') + '</div></div>';
            });
        }
        t.innerHTML = html;
    }

    window.WiFiProbe = {
        init: function() {
            document.getElementById('wp-refresh').addEventListener('click', refresh);
            document.getElementById('wp-period').addEventListener('change', refresh);
            refresh();
        }
    };
})();
