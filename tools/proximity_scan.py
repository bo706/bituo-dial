#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
proximity_scan.py — 近距离广播观察工具
只打印 RSSI 强于阈值的设备，逐包输出（不限频），用于确认桌上测试表的：
  1. 短包(5B)/长包(26B)出现规律；2. BLE 随机地址是否随时间变化；3. 广播频率。
用法：python proximity_scan.py [秒数=15] [RSSI阈值=-55]
创建日期：2026-09-02
"""
import asyncio
import sys
import time
from bleak import BleakScanner

DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
RSSI_THR = int(sys.argv[2]) if len(sys.argv) > 2 else -55


def extract_mfg(md: dict) -> bytes:
    if not md:
        return b""
    raw = md.get(0xFFFF)
    if raw is None and len(md) == 1:
        raw = next(iter(md.values()))
    return bytes(raw) if raw else b""


async def main():
    addr_stat = {}

    def on(dev, adv):
        if adv.rssi is None or adv.rssi < RSSI_THR:
            return
        blob = extract_mfg(getattr(adv, "manufacturer_data", {}))
        addr_stat[dev.address] = addr_stat.get(dev.address, 0) + 1
        ts = time.strftime("%H:%M:%S")
        if len(blob) == 26:
            ctr = int.from_bytes(blob[3:7], "little")
            print(f"[{ts}] LONG26 {dev.address} rssi={adv.rssi} ctrl=0x{blob[2]:02x} "
                  f"ctr={ctr} hex={blob.hex()}")
        elif len(blob) >= 5:
            disc = int.from_bytes(blob[3:5], "little") & 0x0FFF if len(blob) >= 5 else 0
            print(f"[{ts}] SHORT{len(blob)} {dev.address} rssi={adv.rssi} "
                  f"state={blob[2] if len(blob)>2 else 0} disc=0x{disc:03x} hex={blob.hex()}")
        else:
            print(f"[{ts}] ?{len(blob)} {dev.address} rssi={adv.rssi} name={dev.name!r}")

    sc = BleakScanner(detection_callback=on, scanning_mode="passive")
    print(f"[prox] 观察 {DUR}s，只显示 RSSI >= {RSSI_THR} 的设备...\n")
    async with sc:
        await asyncio.sleep(DUR)
    print("\n[prox] 近距离设备包数统计：")
    for a, n in sorted(addr_stat.items(), key=lambda x: -x[1]):
        print(f"  {a}  {n} 包")


asyncio.run(main())
