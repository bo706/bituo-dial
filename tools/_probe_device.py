# -*- coding: utf-8 -*-
"""一次性诊断：active 扫描 15 秒，列出所有协议相关设备（短包/长包/SDM 名称）。"""
import asyncio
from bleak import BleakScanner

async def main():
    rows = {}
    def on(dev, adv):
        mfr = adv.manufacturer_data or {}
        blob = None
        for k, v in mfr.items():
            if k == 0xFFFF or len(mfr) == 1:
                blob = bytes(v)
        name = dev.name or adv.local_name or ""
        interesting = blob is not None or (name and "SDM" in name.upper())
        if not interesting:
            return
        key = dev.address
        prev = rows.get(key)
        # 只在首次或形态变化时打印
        sig = (name, None if blob is None else blob.hex())
        if prev == sig:
            return
        rows[key] = sig
        tag = ""
        if blob is not None:
            if len(blob) == 5 and blob[0:2] == b"\xff\xff":
                disc = int.from_bytes(blob[3:5], "little") & 0xFFF
                tag = f"SHORT5 state={blob[2]} disc=0x{disc:03x}"
            elif len(blob) in (24, 26):
                tag = f"LONG{len(blob)} ctrl=0x{blob[2 if len(blob)==26 else 0]:02x}"
            else:
                tag = f"UNK{len(blob)}"
        print(f"{dev.address} rssi={adv.rssi} name={name!r} {tag} {blob.hex() if blob else ''}")

    async with BleakScanner(detection_callback=on, scanning_mode="active"):
        await asyncio.sleep(15)
    print("---- scan done ----")

asyncio.run(main())
