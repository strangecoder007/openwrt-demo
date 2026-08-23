#!/usr/bin/env python3
"""Inspect an apptraffic traffic.db: top flows, and everything bound to the
board's own LAN IP (the gateway / cloud-drive server). Usage:
    python3 inspect_db.py /path/to/traffic.db
"""
import sqlite3
import sys
import datetime


def main():
    db = sys.argv[1]
    con = sqlite3.connect(db)
    cur = con.cursor()

    cols = [r[1] for r in cur.execute("PRAGMA table_info(traffic)")]
    print("columns:", cols)
    print("rows:", cur.execute("SELECT COUNT(*) FROM traffic").fetchone()[0])

    # Derive the current UTC day bucket from the newest row's timestamp so the
    # script works even when the host running it has a different clock than the
    # device that wrote the database.
    max_ts = cur.execute("SELECT MAX(timestamp) FROM traffic").fetchone()[0]
    day = max_ts // 86400
    print("max timestamp:", datetime.datetime.utcfromtimestamp(max_ts),
          "day bucket:", day)

    print("\n=== today: top flows (src,dst,dport,proto) by rx+tx ===")
    for row in cur.execute(
        """
        SELECT src_ip,dst_ip,dst_port,protocol,app_name,domain,
               SUM(rx_bytes) rx, SUM(tx_bytes) tx, COUNT(*) c
        FROM traffic
        WHERE day=?
        GROUP BY src_ip,dst_ip,dst_port,protocol
        ORDER BY rx DESC LIMIT 15
        """, (day,)):
        print(row)

    print("\n=== today: flows to dst=192.168.100.1 by (src,dport) ===")
    for row in cur.execute(
        """
        SELECT src_ip,src_port,dst_port,domain,app_name,
               SUM(rx_bytes) rx, SUM(tx_bytes) tx, COUNT(*) c
        FROM traffic
        WHERE day=? AND dst_ip='192.168.100.1'
        GROUP BY src_ip,dst_port
        ORDER BY rx DESC LIMIT 20
        """, (day,)):
        print(row)

    print("\n=== today: rx by dst_ip (top 12) ===")
    for row in cur.execute(
        """
        SELECT dst_ip, SUM(rx_bytes) rx, SUM(tx_bytes) tx, COUNT(*) c
        FROM traffic
        WHERE day=?
        GROUP BY dst_ip
        ORDER BY rx DESC LIMIT 12
        """, (day,)):
        print(row)


if __name__ == "__main__":
    main()
