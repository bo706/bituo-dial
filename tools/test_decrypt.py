#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
test_decrypt.py — Bituo Dial Phase1 PC 端 AES-128-CCM 对拍工具
================================================================
用途：
  1. 固定向量自测（无参数运行）：与固件 main/crypto.c::crypto_selftest()
     使用完全相同的一组 key/nonce/ct/tag，证明 PC(cryptography) 与
     设备(mbedTLS) 的 CCM 参数（tag=4、AAD 空、nonce=8 零 + BE32 counter）一致。
  2. 单帧解密：传入完整 26B 厂商广播 hex 与 broadcast_key(32 hex)，
     输出 counter、nonce、15B 明文 hex，并按《BLE 安全通道 API 文档 V2》
     §9.3.1（编码版本1）解析本帧这一相的 U/I/P/FE/RE（P 单位 kW，无 PF）。
  3. 短包校验：--short 校验 5B 短包 12bit 鉴别器是否与 SN 推导一致。

依赖：pip install cryptography
对应文档：BLE_Crypto_API_Documentation.md §9.2 / §9.3；任务书 5.2.2
创建日期：2026-09-01
"""
from __future__ import annotations

import argparse
import sys

try:
    from cryptography.exceptions import InvalidTag
    from cryptography.hazmat.primitives.ciphers.aead import AESCCM
except ImportError:
    sys.exit("缺少依赖：请先执行  pip install cryptography")

# ── 协议常量（与固件 ble_scanner.h 一致）────────────────────────────
ADV_LEN = 26            # 长包厂商数据总长
SHORT_LEN = 5           # 短包总长
PLAIN_LEN = 15
TAG_LEN = 4
NONCE_LEN = 12

# ── 固定测试向量（由本文件 selftest 生成，同步写入固件 crypto.c）────
TV = {
    "key":     "000102030405060708090a0b0c0d0e0f",
    "counter": 0x11223344,
    "plain":   "000102030405060708090a0b0c0d0e",
    "ct":      "dba850e60b9abcb3eb60293fdd56e7",
    "tag":     "58c643e3",
    "adv26":   "ffff1244332211dba850e60b9abcb3eb60293fdd56e758c643e3",
}


def build_nonce(counter: int) -> bytes:
    """nonce12 = 8 字节全 0 || BE32(counter)。

    !!! 坑点（任务书坑2）：广播偏移 3..6 的 counter 是小端，先转成整数后
    必须以大端写入 nonce 高 4 字节；严禁对小端原始 4 字节直接拷贝。
    """
    return bytes(8) + counter.to_bytes(4, "big")


def decrypt_adv(adv: bytes, key: bytes) -> tuple[int, bytes]:
    """解密 26B 长包，返回 (counter, plaintext15)。失败抛 InvalidTag。"""
    if len(adv) != ADV_LEN:
        raise ValueError(f"长包必须 {ADV_LEN} 字节，实际 {len(adv)}")
    if adv[0:2] != b"\xff\xff":
        raise ValueError("前两字节必须是 FF FF（Company ID）")
    counter = int.from_bytes(adv[3:7], "little")          # 小端计数器
    nonce = build_nonce(counter)
    ct = adv[7:22]
    tag = adv[22:26]
    plain = AESCCM(key, tag_length=TAG_LEN).decrypt(nonce, ct + tag, None)
    return counter, plain


def parse_plaintext_v1(plain: bytes, ctrl: int) -> None:
    """按《BLE 安全通道 API 文档 V2》§9.3.1（编码版本 ver=ctrl.bit6-4=1）
    解析【本帧这一相】的 15B 明文，全部小端。三相表每帧只带一相
    （相位=ctrl.bit3-2：0=A/X,1=B/Y,2=C/Z，按帧轮换），连续 3 帧才覆盖三相。
    与固件 main/ble_scanner.c::parse_plaintext、tools/plain15_selftest.py 同构。
    """
    if len(plain) != PLAIN_LEN:
        return
    ver = (ctrl >> 4) & 0x7
    phase = (ctrl >> 2) & 0x3
    cat = ctrl & 0x3
    pname = ["A/X", "B/Y", "C/Z"][phase] if phase < 3 else f"ph{phase}"

    u_v = (plain[0] | plain[1] << 8) / 10.0            # uint16 /10 -> V
    u_i = (plain[2] | plain[3] << 8) / 100.0           # uint16 /100 -> A
    p24 = plain[4] | (plain[5] << 8) | (plain[6] << 16)   # int24 有符号
    if p24 & 0x800000:                                 # 24位最高位=1 -> 负功率
        p24 -= 0x1000000
    p_kw = p24 / 1000.0                                # /1000 -> kW
    fe = int.from_bytes(plain[7:11], "little") / 100.0   # 正向电能 kWh
    re = int.from_bytes(plain[11:15], "little") / 100.0  # 反向电能 kWh

    print(f"  ctrl=0x{ctrl:02x} ver={ver} phase={phase}({pname}) cat={cat}")
    print(f"    本相: U={u_v:.1f}V  I={u_i:.2f}A  P={p_kw:.3f}kW  "
          f"FE={fe:.2f}kWh  RE={re:.2f}kWh")
    print("    注: 广播不含功率因数 PF; 三相分帧轮换, 需连续3帧覆盖 A/B/C; "
          "P 单位为 kW 且可负")


def expected_discriminator(sn_hex: str) -> int:
    """12bit 鉴别器 = SN(12hex→48bit) 的最高 12bit（SN 前 3 个 hex 位）。"""
    s = sn_hex.strip().lower()
    if len(s) != 12:
        raise ValueError("SN 必须为 12 位十六进制")
    int(s, 16)
    return (int(s, 16) >> 36) & 0x0FFF


def selftest() -> bool:
    print("== 固定向量自测（与固件 crypto_selftest 同源）==")
    key = bytes.fromhex(TV["key"])
    adv = bytes.fromhex(TV["adv26"])
    counter, plain = decrypt_adv(adv, key)
    nonce = build_nonce(counter)
    ok = True
    checks = [
        ("counter == 0x11223344", counter == TV["counter"]),
        ("nonce12", nonce.hex() == "000000000000000011223344"),
        ("plain15", plain.hex() == TV["plain"]),
    ]
    for name, passed in checks:
        print(f"  [{'PASS' if passed else 'FAIL'}] {name}")
        ok &= passed

    # 负样本：翻转 tag 末字节，必须认证失败
    bad = bytearray(adv)
    bad[25] ^= 0xFF
    try:
        decrypt_adv(bytes(bad), key)
        print("  [FAIL] 篡改 tag 未被拒绝")
        ok = False
    except InvalidTag:
        print("  [PASS] 篡改 tag 被正确拒绝（InvalidTag）")

    # 负样本：故意用小端拼 nonce（经典错误），必须解密失败
    wrong_nonce = bytes(8) + TV["counter"].to_bytes(4, "little")
    try:
        AESCCM(key, tag_length=TAG_LEN).decrypt(
            wrong_nonce, adv[7:22] + adv[22:26], None)
        print("  [FAIL] 小端 nonce 竟然解密成功（不应发生）")
        ok = False
    except InvalidTag:
        print("  [PASS] 小端 nonce 复现坑2：认证失败，证明必须 BE32")
    print(f"=> {'全部通过' if ok else '存在失败项'}\n")
    return ok


def main() -> None:
    ap = argparse.ArgumentParser(description="Bituo Dial 广播 CCM 对拍工具")
    ap.add_argument("adv_hex", nargs="?", help="26B 长包 hex（含 FF FF）")
    ap.add_argument("key_hex", nargs="?", help="broadcast_key，32 hex")
    ap.add_argument("--short", help="5B 短包 hex，配合 --sn 校验鉴别器")
    ap.add_argument("--sn", help="电表 SN（12 hex）")
    args = ap.parse_args()

    # 无参数：固定向量自测
    if not any((args.adv_hex, args.short)):
        sys.exit(0 if selftest() else 1)

    if args.short:
        short = bytes.fromhex(args.short.replace(" ", ""))
        if len(short) != SHORT_LEN or short[0:2] != b"\xff\xff":
            sys.exit("短包必须为 5 字节且以 FF FF 开头")
        state = short[2]
        disc = int.from_bytes(short[3:5], "little") & 0x0FFF
        print(f"短包 state={state} discriminator=0x{disc:03x}")
        if args.sn:
            exp = expected_discriminator(args.sn)
            print(f"SN 推导值=0x{exp:03x}  ->  {'一致' if disc == exp else '不一致'}")
        return

    if not args.key_hex:
        sys.exit("解密长包需要提供 broadcast_key（32 hex）")
    adv = bytes.fromhex(args.adv_hex.replace(" ", ""))
    key = bytes.fromhex(args.key_hex.replace(" ", ""))
    if len(key) != 16:
        sys.exit("broadcast_key 必须为 16 字节（32 hex）")
    counter, plain = decrypt_adv(adv, key)
    print(f"counter(LE->int) = {counter} (0x{counter:08x})")
    print(f"nonce12          = {build_nonce(counter).hex()}")
    print(f"plaintext15 hex  = {plain.hex()}")
    parse_plaintext_v1(plain, adv[2])   # adv[2]=ctrl，含 ver/phase/cat


if __name__ == "__main__":
    main()
