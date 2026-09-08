#!/usr/bin/env python3
"""Poll Dial /api/info on an interval and append JSONL for 24h soak.

Usage:
  python tools/soak_poll.py [url] [--interval 300]
Default url: http://192.168.50.151/api/info
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("url", nargs="?", default="http://192.168.50.151/api/info")
    p.add_argument("--interval", type=float, default=300.0)
    p.add_argument("--duration", type=float, default=0.0,
                   help="Stop after N seconds (0 = run forever)")
    p.add_argument("--out", default="")
    args = p.parse_args()

    root = Path(__file__).resolve().parents[1]
    out = Path(args.out) if args.out else (
        root / "tools" / "captures" / "phase5_soak_20260904.jsonl"
    )
    out.parent.mkdir(parents=True, exist_ok=True)
    print(f"soak poll {args.url} every {args.interval:.0f}s -> {out}", flush=True)
    deadline = time.time() + args.duration if args.duration > 0 else None

    while True:
        row = {
            "ts": datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds"),
        }
        try:
            with urllib.request.urlopen(args.url, timeout=20) as resp:
                row["http"] = getattr(resp, "status", 200)
                body = resp.read().decode("utf-8", errors="replace")
            row["body"] = json.loads(body)
        except Exception as e:
            row["error"] = str(e)
        with out.open("a", encoding="utf-8") as f:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
        d = (row.get("body") or {}).get("d") or {}
        mqtt = (d.get("mqtt") or {}).get("connected")
        wifi = (d.get("wifi") or {}).get("connected")
        rssi = (d.get("wifi") or {}).get("rssi")
        print(
            f"{row['ts']} uptime={d.get('uptime_s')} wifi={wifi} rssi={rssi} "
            f"mqtt={mqtt} online={((d.get('ble_scan') or {}).get('online_count'))} "
            f"{'ERR ' + row['error'] if 'error' in row else 'ok'}",
            flush=True,
        )
        if deadline is not None and time.time() >= deadline:
            print("soak duration reached, stop", flush=True)
            return 0
        remaining = None if deadline is None else deadline - time.time()
        sleep_s = max(args.interval, 5.0)
        if remaining is not None:
            if remaining <= 0:
                print("soak duration reached, stop", flush=True)
                return 0
            sleep_s = min(sleep_s, remaining)
        time.sleep(sleep_s)


if __name__ == "__main__":
    raise SystemExit(main())
