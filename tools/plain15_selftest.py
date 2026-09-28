#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file    plain15_selftest.py
@brief   Phase1 收尾：15 字节广播明文（安全通道文档 §9.3.1，编码版本 1）
         编解码主机侧自测。解码逻辑与 main/ble_scanner.c::parse_plaintext
         严格同构（同样的偏移、小端、int24 符号扩展、倍率），用于在电表
         开路（I/P/FE/RE 明文全 0）时，离线证明非零电流、负功率、电能的
         解析公式正确。
@version 2026-09-03
@run     python plain15_selftest.py
"""
import os
import random
import struct
import sys


# ── 编码：物理量 -> 15 字节小端明文（模拟电表固件侧打包，仅自测用）────────
def encode_plain15(v_v, i_a, p_kw, fe_kwh, re_kwh):
    v_raw = int(round(v_v * 10))          # uint16 /10
    i_raw = int(round(i_a * 100))         # uint16 /100
    p_raw = int(round(p_kw * 1000))       # int24  有符号 /1000
    fe_raw = int(round(fe_kwh * 100))     # uint32 /100
    re_raw = int(round(re_kwh * 100))     # uint32 /100
    # 饱和裁剪到字段位宽
    v_raw = max(0, min(0xFFFF, v_raw))
    i_raw = max(0, min(0xFFFF, i_raw))
    p_raw = max(-0x800000, min(0x7FFFFF, p_raw))
    fe_raw = max(0, min(0xFFFFFFFF, fe_raw))
    re_raw = max(0, min(0xFFFFFFFF, re_raw))
    b = bytearray(15)
    struct.pack_into('<H', b, 0, v_raw)
    struct.pack_into('<H', b, 2, i_raw)
    # int24 小端：取低 3 字节（负数为补码）
    b[4] = p_raw & 0xFF
    b[5] = (p_raw >> 8) & 0xFF
    b[6] = (p_raw >> 16) & 0xFF
    struct.pack_into('<I', b, 7, fe_raw)
    struct.pack_into('<I', b, 11, re_raw)
    return bytes(b)


# ── 解码：15 字节 -> 物理量（与 ble_scanner.c::parse_plaintext 逐行同构）────
def decode_plain15(b):
    assert len(b) == 15
    # [0:2] 电压 uint16 小端 /10
    v_raw = b[0] | (b[1] << 8)
    v = v_raw / 10.0
    # [2:4] 电流 uint16 小端 /100
    i_raw = b[2] | (b[3] << 8)
    i = i_raw / 100.0
    # [4:7] 有功 int24【有符号】小端 /1000，必须符号扩展
    p_raw = b[4] | (b[5] << 8) | (b[6] << 16)
    if p_raw & 0x00800000:          # 24 位最高位为 1 -> 负
        p_raw -= 0x01000000
    p = p_raw / 1000.0
    # [7:11] 正向电能 uint32 小端 /100
    fe_raw = b[7] | (b[8] << 8) | (b[9] << 16) | (b[10] << 24)
    fe = fe_raw / 100.0
    # [11:15] 反向电能 uint32 小端 /100
    re_raw = b[11] | (b[12] << 8) | (b[13] << 16) | (b[14] << 24)
    re = re_raw / 100.0
    return v, i, p, fe, re


def approx(x, y, tol=1e-6):
    return abs(x - y) <= tol * max(1.0, abs(x), abs(y))


def main():
    failures = 0

    # 用例 1：文档 §9.3.1 原文例子 0x08FC(小端 FC 08) -> 230.0V
    b = bytes.fromhex("fc08" + "00" * 13)
    v, i, p, fe, re = decode_plain15(b)
    ok = approx(v, 230.0) and i == p == fe == re == 0.0
    print(f"[1] doc example FC08 -> V={v} (expect 230.0)  {'PASS' if ok else 'FAIL'}")
    failures += (not ok)

    # 用例 2：真机抓包样例 9e 08 00.. -> 220.6V，其余 0（开路无负载）
    b = bytes.fromhex("9e08" + "00" * 13)
    v, i, p, fe, re = decode_plain15(b)
    ok = approx(v, 220.6) and i == p == fe == re == 0.0
    print(f"[2] real capture 9e08 -> V={v} (expect 220.6)  {'PASS' if ok else 'FAIL'}")
    failures += (not ok)

    # 用例 3：非零全字段 + 负有功功率（反向送电），手工可核对
    #   V=230.0 I=5.01 P=-1.234kW FE=1234.56 RE=0.02
    b = encode_plain15(230.0, 5.01, -1.234, 1234.56, 0.02)
    print(f"[3] encoded hex = {b.hex(' ')}")
    v, i, p, fe, re = decode_plain15(b)
    ok = (approx(v, 230.0, 1e-3) and approx(i, 5.01, 1e-3)
          and approx(p, -1.234, 1e-3) and approx(fe, 1234.56, 1e-3)
          and approx(re, 0.02, 1e-3))
    print(f"    decoded: V={v} I={i} P={p}kW FE={fe} RE={re}  "
          f"{'PASS' if ok else 'FAIL'}")
    failures += (not ok)
    # int24 负功率补码字节独立核对：-1234 的 3 字节小端
    neg = (-1234) & 0xFFFFFF
    expect_bytes = bytes([neg & 0xFF, (neg >> 8) & 0xFF, (neg >> 16) & 0xFF])
    ok2 = b[4:7] == expect_bytes
    print(f"    int24(-1234) bytes={b[4:7].hex(' ')} "
          f"expect {expect_bytes.hex(' ')}  {'PASS' if ok2 else 'FAIL'}")
    failures += (not ok2)

    # 用例 4：1000 组随机 roundtrip，覆盖满量程与负功率
    rnd = random.Random(20260903)
    rt_fail = 0
    for _ in range(1000):
        vin = rnd.uniform(0, 6553.5)
        iin = rnd.uniform(0, 655.35)
        pin = rnd.uniform(-8388.608, 8388.607)
        fein = rnd.uniform(0, 42949672.95)
        rein = rnd.uniform(0, 42949672.95)
        b = encode_plain15(vin, iin, pin, fein, rein)
        v, i, p, fe, re = decode_plain15(b)
        # 量化误差：分辨率分别 0.1/0.01/0.001/0.01/0.01
        if not (abs(v - vin) <= 0.05 and abs(i - iin) <= 0.005
                and abs(p - pin) <= 0.0005
                and abs(fe - fein) <= 0.005 and abs(re - rein) <= 0.005):
            rt_fail += 1
    ok = rt_fail == 0
    print(f"[4] 1000 random roundtrips (full range, signed P): "
          f"{'PASS' if ok else f'FAIL ({rt_fail} mismatches)'}")
    failures += (not ok)

    # 用例 5：边界值——功率正/负饱和、电压电流为 0
    for name, kw in [("P max", 8388.607), ("P min", -8388.608)]:
        b = encode_plain15(0, 0, kw, 0, 0)
        _, _, p, _, _ = decode_plain15(b)
        ok = approx(p, kw, 1e-3)
        print(f"[5] boundary {name}: decoded P={p}kW  {'PASS' if ok else 'FAIL'}")
        failures += (not ok)

    print("-" * 60)
    if failures == 0:
        print("ALL PASSED: §9.3.1 15B plaintext codec verified (offsets, "
              "little-endian, int24 sign extension, scaling).")
        return 0
    print(f"{failures} CHECK(S) FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
