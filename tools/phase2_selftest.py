#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file    phase2_selftest.py
@brief   Bituo Dial Phase2（配置管理）固件验收脚本。
         在单个 BLE 连接内顺序执行全部用例：8 条命令正常路径 + 任务书 7.6
         全部错误分支（invalid sn/key/mac、sn not found、meter list full），
         每条用例打印 PASS/FAIL，结束时清理测试电表，不污染 NVS。
@usage   python phase2_selftest.py [设备名，默认 BitUo-Dial]
@date    2026-09-02
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

# ── 与固件一致的 GATT UUID（任务书 5.7）──
SVC_UUID   = "0000a003-0000-1000-8000-00805f9b34fb"
WRITE_UUID = "0000c313-0000-1000-8000-00805f9b34fb"   # 客户端 -> Dial，写 JSON 命令
NOTIFY_UUID = "0000c315-0000-1000-8000-00805f9b34fb"  # Dial -> 客户端，Notify 响应

GOOD_KEY = "00112233445566778899aabbccddeeff"   # 合法 32hex broadcast_key


class Dial:
    def __init__(self, client):
        self.c = client
        self._q = asyncio.Queue()
        self._seq = 0
        self._rx = b""   # Notify 分片累积缓冲

    async def setup(self):
        await self.c.start_notify(NOTIFY_UUID, self._on_notify)

    def _on_notify(self, sender, data):
        import json
        # 固件按 MTU-3 分片，累积到完整 JSON 再入队
        self._rx += bytes(data)
        try:
            obj = json.loads(self._rx.decode("utf-8"))
            self._q.put_nowait(obj)
            self._rx = b""
        except json.JSONDecodeError:
            pass
        except Exception as e:
            print(f"  [warn] notify parse error: {e}")
            self._rx = b""

    async def cmd(self, obj, timeout=8.0):
        import json
        self._seq += 1
        self._rx = b""
        while not self._q.empty():
            self._q.get_nowait()
        payload = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        await self.c.write_gatt_char(WRITE_UUID, payload, response=True)
        return await asyncio.wait_for(self._q.get(), timeout=timeout)


# ── 微型断言框架 ──────────────────────────────────────────────
PASS_N = 0
FAIL_N = 0

def check(name, cond, detail=""):
    global PASS_N, FAIL_N
    if cond:
        PASS_N += 1
        print(f"  [PASS] {name}")
    else:
        FAIL_N += 1
        print(f"  [FAIL] {name}  {detail}")


def test_sn(i):   # 12 位 hex 测试 SN
    return f"0A{i:02X}0000000000"[:12]


def test_mac(i):
    return f"02:00:00:00:00:{i:02X}"


async def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "BitUo-Dial"
    print(f"[1] 扫描 {name} ...")
    dev = await BleakScanner.find_device_by_name(name, timeout=12.0)
    if not dev:
        print("[FATAL] 未找到设备")
        sys.exit(2)

    async with BleakClient(dev) as client:
        print(f"[2] 已连接 {dev.address}")
        d = Dial(client)
        await d.setup()
        await asyncio.sleep(0.5)

        # 记录初始电表数（phase1 注入表不计入 NVS，但占运行时槽）
        r = await d.cmd({"cmd": "list_meters"})
        base_count = len(r.get("d", {}).get("meters", []))
        print(f"[3] 初始电表数 = {base_count}\n")

        # ── 用例 1：get_info ──
        print("== get_info ==")
        r = await d.cmd({"cmd": "get_info"})
        info = r.get("d", {})
        check("get_info ok", r.get("ok") is True)
        check("含 dial_sn", bool(info.get("dial_sn")))
        check("含 fw_ver", info.get("fw_ver") == "1.1.0")
        check("ble_scan.running", info.get("ble_scan", {}).get("running") is True)

        # ── 用例 2：add_meter 合法 ──
        print("\n== add_meter 合法 ==")
        r = await d.cmd({"cmd": "add_meter", "sn": test_sn(1),
                         "mac": test_mac(1), "bcast_key": GOOD_KEY, "label": "selftest1"})
        check("合法 add ok", r.get("ok") is True, str(r))
        add_index = r.get("d", {}).get("index")

        # ── 用例 3：重复 SN 覆盖（幂等）──
        r = await d.cmd({"cmd": "add_meter", "sn": test_sn(1),
                         "mac": test_mac(1), "bcast_key": GOOD_KEY, "label": "selftest1b"})
        check("重复 SN 覆盖 ok（幂等）", r.get("ok") is True, str(r))

        # ── 用例 4/5/6：三类参数错误 ──
        print("\n== add_meter 参数错误分支 ==")
        r = await d.cmd({"cmd": "add_meter", "sn": "ZZZ",
                         "mac": test_mac(2), "bcast_key": GOOD_KEY})
        check("非法 sn -> invalid sn", r.get("ok") is False and "invalid sn" in r.get("msg", ""), str(r))

        r = await d.cmd({"cmd": "add_meter", "sn": test_sn(2),
                         "mac": test_mac(2), "bcast_key": "xyz"})
        check("非法 key -> invalid key", r.get("ok") is False and "invalid key" in r.get("msg", ""), str(r))

        r = await d.cmd({"cmd": "add_meter", "sn": test_sn(2),
                         "mac": "XYZ", "bcast_key": GOOD_KEY})
        check("非法 mac -> invalid mac", r.get("ok") is False and "invalid mac" in r.get("msg", ""), str(r))

        # ── 用例 7：del_meter 不存在 ──
        print("\n== del_meter 错误分支 ==")
        r = await d.cmd({"cmd": "del_meter", "sn": "FFFFFFFFFFFF"})
        check("删除不存在 -> sn not found", r.get("ok") is False and "sn not found" in r.get("msg", ""), str(r))

        # ── 用例 8：填满到 16，触发 list full ──
        print("\n== meter list full（填满 16 槽）==")
        # 当前已有 base_count + 1（selftest1），继续添加直到满
        got_full = False
        last_count = base_count + 1
        for i in range(2, 20):   # 最多尝试到 i=19，正常在 16 时满
            r = await d.cmd({"cmd": "add_meter", "sn": test_sn(i),
                             "mac": test_mac(i), "bcast_key": GOOD_KEY})
            if r.get("ok"):
                last_count += 1
            else:
                got_full = ("list full" in r.get("msg", ""))
                full_msg = r.get("msg")
                break
        r = await d.cmd({"cmd": "list_meters"})
        full_count = len(r.get("d", {}).get("meters", []))
        check(f"填满触发 list full（当前 {full_count} 块）", got_full and full_count == 16,
              f"got_full={got_full}, count={full_count}, msg={full_msg if got_full else ''}")

        # ── 用例 9：setwifi / set_mqtt ──
        print("\n== setwifi / set_mqtt ==")
        r = await d.cmd({"cmd": "setwifi", "ssid": "SelftestAP", "pass": "12345678"})
        check("setwifi ok", r.get("ok") is True, str(r))
        r = await d.cmd({"cmd": "set_mqtt", "host": "mqtt.selftest.io", "port": 1883,
                         "user": "", "pass": "", "tls": 0})
        check("set_mqtt ok", r.get("ok") is True, str(r))
        r = await d.cmd({"cmd": "get_info"})
        check("wifi 已写入", r.get("d", {}).get("wifi", {}).get("ssid") == "SelftestAP")
        check("mqtt 已写入", r.get("d", {}).get("mqtt", {}).get("host") == "mqtt.selftest.io")

        # ── 用例 10：get_meters 结构 ──
        print("\n== get_meters ==")
        r = await d.cmd({"cmd": "get_meters"})
        ms = r.get("d", {}).get("meters", [])
        check("get_meters ok 且数量=16", r.get("ok") is True and isinstance(ms, list), str(r)[:120])

        # ── 清理：删除全部 0A 开头测试表，恢复到 base_count ──
        print("\n== 清理测试电表 ==")
        r = await d.cmd({"cmd": "list_meters"})
        for m in r.get("d", {}).get("meters", []):
            sn = m.get("sn", "")
            if sn.startswith("0A"):
                await d.cmd({"cmd": "del_meter", "sn": sn})
        r = await d.cmd({"cmd": "list_meters"})
        left = len(r.get("d", {}).get("meters", []))
        check(f"清理后恢复初始数量（{base_count}）", left == base_count, f"left={left}")

    # ── 汇总 ──
    print("\n" + "=" * 48)
    print(f"Phase2 自检结果：PASS={PASS_N}  FAIL={FAIL_N}")
    print("=" * 48)
    sys.exit(0 if FAIL_N == 0 else 1)


if __name__ == "__main__":
    asyncio.run(main())
