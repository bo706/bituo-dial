# -*- coding: utf-8 -*-
"""一次性诊断：连接测试表，读取 ble-crypto-ver（无需 PIN），判断当前 step。"""
import asyncio, json
from bleak import BleakScanner, BleakClient

SVC = "0000a003-0000-1000-8000-00805f9b34fb"
RD  = "0000c314-0000-1000-8000-00805f9b34fb"
WR  = "0000c313-0000-1000-8000-00805f9b34fb"
NTF = "0000c315-0000-1000-8000-00805f9b34fb"
NEEDLE = "SDM01_3PN"

async def main():
    print("[1] 扫描 12 秒定位测试表 ...")
    dev = None
    def on(d, a):
        nonlocal dev
        name = d.name or a.local_name or ""
        if NEEDLE.lower() in name.lower():
            dev = d
            print("    命中:", d.address, name, "rssi=", a.rssi)
    async with BleakScanner(detection_callback=on, scanning_mode="active"):
        await asyncio.sleep(12)
    if dev is None:
        print("未找到名称含 SDM01_3PN 的设备")
        return
    print("[2] 连接", dev.address)
    inbox = asyncio.Queue()
    async with BleakClient(dev, timeout=15) as cli:
        print("    connected:", cli.is_connected)
        await cli.start_notify(NTF, lambda _, d: inbox.put_nowait(bytes(d).decode("utf-8", "replace")))
        await cli.write_gatt_char(WR, json.dumps({"cmd": "ble-crypto-ver"}, separators=(",", ":")).encode(), response=True)
        print("[3] 已发送 ble-crypto-ver，等待应答 ...")
        try:
            for _ in range(3):
                msg = await asyncio.wait_for(inbox.get(), timeout=6)
                print("    [NTF]", msg)
        except asyncio.TimeoutError:
            print("    notify 等待超时")
        try:
            raw = await cli.read_gatt_char(RD)
            print("[4] READ C314:", bytes(raw).decode("utf-8", "replace"))
        except Exception as e:
            print("[4] read 失败:", e)
        print("[5] 服务列表:")
        for s in cli.services:
            print("    svc", s.uuid)
            for c in s.characteristics:
                print("      char", c.uuid, c.properties)

asyncio.run(main())
