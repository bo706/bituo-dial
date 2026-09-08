#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_meter_key.py — 用指定 broadcast_key 实时解密附近电表的 26B 长包，
不依赖 BLE 地址（RPA 会变），对每个信号强的长包盲解，Tag 通过即命中。
用法：python verify_meter_key.py <32hex broadcast_key> [持续秒数=15] [RSSI阈值=-55]
创建日期：2026-09-02
"""
import asyncio
import sys
import time
from bleak import BleakScanner
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

KEY = bytes.fromhex(sys.argv[1])
DUR = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
RSSI_THR = int(sys.argv[3]) if len(sys.argv) > 3 else -55
aesccm = AESCCM(KEY, tag_length=4)

hit = {"n": 0}


def extract_mfg(md: dict) -> bytes:
    # 兼容后端：Windows 常把 Company ID(FF FF) 剥掉，26B 长包只剩 24B，需补回
    if not md:
        return b""
    raw = md.get(0xFFFF)
    if raw is None and len(md) == 1:
        raw = next(iter(md.values()))
    if raw is None:
        return b""
    raw = bytes(raw)
    if len(raw) == 26 and raw[0:2] == b"\xff\xff":
        return raw
    if len(raw) == 24:                    # Company ID 被后端剥除
        return b"\xff\xff" + raw
    return raw


def try_decrypt(blob: bytes):
    """blob 为含 ffff 的 26B；返回 (counter, ctrl, plain15) 或 None。"""
    if len(blob) != 26 or blob[0] != 0xFF or blob[1] != 0xFF:
        return None
    ctrl = blob[2]
    counter = int.from_bytes(blob[3:7], "little")          # 空口小端
    nonce = b"\x00" * 8 + counter.to_bytes(4, "big")        # Nonce 后 4B 用大端
    ct, tag = blob[7:22], blob[22:26]
    try:
        plain = aesccm.decrypt(nonce, ct + tag, None)
        return counter, ctrl, plain
    except Exception:
        return None


def on(dev, adv):
    if adv.rssi is None or adv.rssi < RSSI_THR:
        return
    blob = extract_mfg(getattr(adv, "manufacturer_data", {}))
    if len(blob) != 26 or blob[0] != 0xFF or blob[1] != 0xFF:
        return
    ctrl = blob[2]
    counter = int.from_bytes(blob[3:7], "little")
    r = try_decrypt(blob)
    if r is None:
        print(f"[{time.strftime('%H:%M:%S')}] LONG26 {dev.address} rssi={adv.rssi} "
              f"ctrl=0x{ctrl:02x} ctr={counter}  -> 解密失败(Tag不符)")
        return
    counter, ctrl, plain = r
    phase = (ctrl >> 2) & 0x3
    cat = ctrl & 0x3
    # 前 2 字节按 0.1V/LSB、小端尝试解析（数据字典到位前的旁证）
    v_raw = int.from_bytes(plain[0:2], "little")
    hit["n"] += 1
    print(f"[{time.strftime('%H:%M:%S')}] HIT {dev.address} rssi={adv.rssi} "
          f"ctrl=0x{ctrl:02x}(phase={phase} cat={cat}) ctr={counter}")
    print(f"    plain15={plain.hex()}  前2字节=>{v_raw/10.0:.1f} (按0.1V/LSB小端)")


async def main():
    print(f"[verify] key={sys.argv[1]} 观察 {DUR}s, RSSI>={RSSI_THR}, 盲解命中即打印...\n")
    sc = BleakScanner(detection_callback=on, scanning_mode="passive")
    async with sc:
        await asyncio.sleep(DUR)
    print(f"\n[verify] 共解密成功 {hit['n']} 帧")

asyncio.run(main())
