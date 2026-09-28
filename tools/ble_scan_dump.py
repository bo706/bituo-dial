#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ble_scan_dump.py — Phase1 空口原始广播抓取工具（无需密钥）
=========================================================
作用：在拿到 broadcast_key 之前，先用 PC 蓝牙被动扫描，把所有携带
0xFFFF 厂商数据的电表广播原样落盘，确认：
  1. 附近是否存在目标电表、其 BLE 地址与名称；
  2. 广播是 5B 短包（step=0，未配网）还是 26B 长包（step≠0，已配网）；
  3. 长包 counter 是否逐帧递增（验证 filter_duplicates=0 的必要性）；
  4. 抓到的 26B hex 可直接喂给 test_decrypt.py 解密。

依赖：pip install bleak
用法：
  python ble_scan_dump.py                 # 持续扫描，Ctrl+C 停止
  python ble_scan_dump.py --duration 30   # 扫 30 秒
  python ble_scan_dump.py --match SXM     # 只看名称/地址含关键字的设备
创建日期：2026-09-01
"""
from __future__ import annotations

import argparse
import asyncio
import time

try:
    from bleak import BleakScanner
except ImportError:
    raise SystemExit("缺少依赖：请先执行  pip install bleak")

ADV_COMPANY_ID = 0xFFFF
LONG_LEN = 26
SHORT_LEN = 5


def extract_mfg(manufacturer_data: dict) -> bytes | None:
    """兼容不同后端：bleak 的 manufacturer_data 值可能已剥掉 Company ID。"""
    if not manufacturer_data:
        return None
    raw = manufacturer_data.get(ADV_COMPANY_ID)
    if raw is None and len(manufacturer_data) == 1:
        raw = next(iter(manufacturer_data.values()))
    if raw is None:
        return None
    if len(raw) == LONG_LEN and raw[0:2] == b"\xff\xff":
        return raw
    if len(raw) == LONG_LEN - 2:           # Company ID 被后端剥除
        return b"\xff\xff" + raw
    if len(raw) == SHORT_LEN and raw[0:2] == b"\xff\xff":
        return raw
    if len(raw) == SHORT_LEN - 2:
        return b"\xff\xff" + raw
    return raw                              # unknown 形态也回显，便于人工判断


async def scan(duration: float, match: str | None) -> None:
    counters: dict[str, int] = {}
    print(f"[scan] 开始被动扫描 {duration}s（match={match or '无'}）...")
    seen: dict[str, int] = {}

    def on_detected(device, advertisement_data):
        name = device.name or ""
        if match and match.lower() not in (name + device.address).lower():
            return
        blob = extract_mfg(getattr(advertisement_data, "manufacturer_data", {}))
        if blob is None:
            return
        key = device.address
        kind = {LONG_LEN: "LONG26", SHORT_LEN: "SHORT5"}.get(len(blob), f"UNK{len(blob)}")
        extra = ""
        if kind == "LONG26":
            ctr = int.from_bytes(blob[3:7], "little")
            prev = counters.get(key)
            mark = "" if prev is None else (" delta=%d" % ((ctr - prev) & 0xFFFFFFFF))
            counters[key] = ctr
            extra = f" ctrl=0x{blob[2]:02x} counter={ctr}{mark}"
        elif kind == "SHORT5":
            disc = int.from_bytes(blob[3:5], "little") & 0x0FFF
            extra = f" state={blob[2]} disc=0x{disc:03x}"
        # 限频：同一设备每 5 帧全量打印 1 次，避免刷屏
        seen[key] = seen.get(key, 0) + 1
        if seen[key] % 5 == 1:
            print(f"[{time.strftime('%H:%M:%S')}] {kind} {device.address} "
                  f"rssi={advertisement_data.rssi} name={name!r}{extra}")
            print(f"    hex={blob.hex()}")

    # 被动扫描（不发 SCAN_REQ，与固件 passive=1 行为一致）
    scanner = BleakScanner(detection_callback=on_detected, scanning_mode="passive")
    async with scanner:
        await asyncio.sleep(duration)
    print("[scan] 结束")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=float, default=60.0, help="扫描秒数，默认 60")
    ap.add_argument("--match", default=None, help="名称/地址过滤关键字")
    args = ap.parse_args()
    try:
        asyncio.run(scan(args.duration, args.match))
    except KeyboardInterrupt:
        print("\n[scan] 用户中断")


if __name__ == "__main__":
    main()
