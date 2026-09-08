# -*- coding: utf-8 -*-
"""
实时抓取测试表 26B 长广播并使用 broadcast_key 做 AES-128-CCM 解密。
用法:
    python decrypt_live.py <broadcast_key_32hex> [匹配名称子串] [持续秒]
必须传入 broadcast_key；本仓库不附带真机密钥。
依据: 《BLE 安全通道 API 文档 V2》§9.3 / ble-braodcasting.md
"""
import asyncio
import sys
import time
from bleak import BleakScanner
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

if len(sys.argv) < 2:
    sys.exit("usage: python decrypt_live.py <broadcast_key_32hex> [name-substr|all] [seconds]")
KEY = bytes.fromhex(sys.argv[1])
NEEDLE = (sys.argv[2] if len(sys.argv) > 2 else "all").lower()
DURATION = int(sys.argv[3]) if len(sys.argv) > 3 else 20
assert len(KEY) == 16, "broadcast key 必须为 16 字节(32 hex)"

aes = AESCCM(KEY, tag_length=4)
ok_count = 0
fail_count = 0
phase_seen = set()


def decode_blob(blob26: bytes):
    """26B: FF FF | ctrl | counter LE u32 | ct15 | tag4"""
    ctrl = blob26[2]
    counter = int.from_bytes(blob26[3:7], "little")
    ct = blob26[7:22]
    tag = blob26[22:26]
    # !!! 鍧戠偣鎻愰啋:nonce 鍚?4 瀛楄妭蹇呴』鏄?counter 鐨勩€愬ぇ绔€?骞挎挱閲?counter 鏄皬绔?涓ョ memcpy
    nonce = bytes(8) + counter.to_bytes(4, "big")
    plain = aes.decrypt(nonce, ct + tag, None)  # AAD 涓虹┖
    return ctrl, counter, plain


def on(dev, adv):
    global ok_count, fail_count
    name = (dev.name or adv.local_name or "")
    mfr = adv.manufacturer_data or {}
    if not mfr:
        return
    raw = next(iter(mfr.values()))
    if len(raw) == 24:           # bleak/鍚庣鍓ユ帀浜?FF FF 鍏徃鏍囪瘑
        blob = b"\xff\xff" + bytes(raw)
    elif len(raw) == 26 and raw[0:2] == b"\xff\xff":
        blob = bytes(raw)
    else:
        return
    # active 鎵弿涓嬪悕绉板湪 scan response;涓嶄緷璧栧悕绉拌繃婊?鐩存帴浠?瀵嗛挜鑳藉惁璁よ瘉"浣滀负璁惧鍒ゆ嵁
    try:
        ctrl, counter, plain = decode_blob(blob)
    except Exception:
        # 鐜閲屽叾瀹冨巶鍟嗙殑闀垮寘鐢ㄦ湰瀵嗛挜蹇呯劧 tag 澶辫触,闈欓粯璁℃暟
        fail_count += 1
        return
    if NEEDLE != "all" and NEEDLE not in name.lower():
        return
    ok_count += 1
    ver = (ctrl >> 4) & 0x7
    gatt = (ctrl >> 7) & 1
    phase = (ctrl >> 2) & 0x3
    cat = ctrl & 0x3
    phase_seen.add(phase)
    print(f"[OK ] {time.strftime('%H:%M:%S')} {dev.address} ctrl=0x{ctrl:02x} "
          f"(gatt={gatt} ver={ver} phase={phase} cat={cat}) counter={counter:<8} "
          f"plain15={plain.hex()} rssi={adv.rssi}")


async def main():
    print(f"key={KEY.hex()} match={NEEDLE!r} duration={DURATION}s")
    async with BleakScanner(detection_callback=on, scanning_mode="active"):
        await asyncio.sleep(DURATION)
    print(f"---- done: 瑙ｅ瘑鎴愬姛 {ok_count} 甯? 澶辫触 {fail_count} 甯? 瑕嗙洊鐩镐綅 {sorted(phase_seen)} ----")


asyncio.run(main())
