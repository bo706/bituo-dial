#!/usr/bin/env python3
"""
dial_config.py - Bituo Dial 明文配置工具 v1.1
BLE 通道：直接收发 JSON，无加密握手（与 Basic/Basic+ 固件一致）
依赖：pip install bleak requests
创建日期：2026-09-01
"""
import asyncio, json, argparse, sys
import requests
from bleak import BleakClient, BleakScanner

SVC_UUID    = "0000A003-0000-1000-8000-00805F9B34FB"
WRITE_UUID  = "0000C313-0000-1000-8000-00805F9B34FB"
READ_UUID   = "0000C314-0000-1000-8000-00805F9B34FB"
NOTIFY_UUID = "0000C315-0000-1000-8000-00805F9B34FB"

# ──────────────────────────────────────────
# BLE 客户端封装
# ──────────────────────────────────────────
class DialBLEClient:
    def __init__(self, device_name):
        self.device_name = device_name
        self.client = None
        self._response = asyncio.Queue()
        self._rx_buf = b""   # Notify 分片累积缓冲（长 JSON 会被固件按 MTU-3 切多包）

    async def connect(self, retries=1):
        # 断开后设备恢复广播有 1~2s 窗口，首次扫描偶发落空时自动重试一次
        device = None
        for attempt in range(retries + 1):
            tip = "（重试）" if attempt else ""
            print(f"[BLE] 扫描设备: {self.device_name} ...{tip}")
            device = await BleakScanner.find_device_by_name(self.device_name, timeout=10.0)
            if device:
                break
            if attempt < retries:
                await asyncio.sleep(2)
        if not device:
            print(f"[错误] 未找到设备: {self.device_name}")
            sys.exit(1)
        self.client = BleakClient(device)
        await self.client.connect()
        await self.client.start_notify(NOTIFY_UUID, self._on_notify)
        print(f"[BLE] 已连接: {device.address}")

    def _on_notify(self, sender, data: bytearray):
        # 固件对超过 MTU-3 的响应分片 notify，这里累积到能解析出完整 JSON 为止
        self._rx_buf += bytes(data)
        try:
            obj = json.loads(self._rx_buf.decode("utf-8"))
            self._response.put_nowait(obj)
            self._rx_buf = b""
        except json.JSONDecodeError:
            pass   # 分片未到齐，继续等待
        except Exception as e:
            print(f"[警告] 解析响应失败: {e}, 已累积 {len(self._rx_buf)} 字节，清空")
            self._rx_buf = b""

    async def send_command(self, cmd: dict, timeout=8.0):
        self._rx_buf = b""   # 发新命令前清空上一条残留
        # 排空队列里可能的旧响应
        while not self._response.empty():
            self._response.get_nowait()
        payload = json.dumps(cmd, ensure_ascii=False).encode("utf-8")
        await self.client.write_gatt_char(WRITE_UUID, payload, response=True)
        try:
            return await asyncio.wait_for(self._response.get(), timeout=timeout)
        except asyncio.TimeoutError:
            print("[错误] 等待响应超时")
            sys.exit(1)

    async def disconnect(self):
        if self.client and self.client.is_connected:
            await self.client.stop_notify(NOTIFY_UUID)
            await self.client.disconnect()
            print("[BLE] 已断开")

# ──────────────────────────────────────────
# HTTP 客户端封装
# ──────────────────────────────────────────
class DialHTTPClient:
    def __init__(self, host: str, port: int = 80):
        self.base = f"http://{host}:{port}"

    def get(self, path: str):
        r = requests.get(f"{self.base}{path}", timeout=5)
        r.raise_for_status()
        return r.json()

    def post(self, path: str, body: dict):
        r = requests.post(f"{self.base}{path}", json=body, timeout=5)
        r.raise_for_status()
        return r.json()

    def delete(self, path: str):
        r = requests.delete(f"{self.base}{path}", timeout=5)
        r.raise_for_status()
        return r.json()

# ──────────────────────────────────────────
# BLE 子命令实现
# ──────────────────────────────────────────
async def ble_scan(args):
    """扫描附近的 BitUo-Dial 设备"""
    print("[BLE] 扫描中（5秒）...")
    devices = await BleakScanner.discover(timeout=5.0)
    found = [d for d in devices if d.name and "BitUo-Dial" in d.name]
    if not found:
        print("未发现 BitUo-Dial 设备")
    else:
        print(f"发现 {len(found)} 台设备：")
        for d in found:
            print(f"  {d.name:<20} {d.address}  RSSI={d.rssi} dBm")

async def ble_setwifi(args):
    c = DialBLEClient(args.ble)
    await c.connect()
    r = await c.send_command({"cmd": "setwifi", "ssid": args.ssid, "pass": args.password})
    _print_resp(r)
    await c.disconnect()

async def ble_set_mqtt(args):
    c = DialBLEClient(args.ble)
    await c.connect()
    cmd = {"cmd": "set_mqtt", "host": args.host, "port": args.port,
           "user": args.user or "", "pass": args.password or "",
           "tls": 1 if args.tls else 0}
    r = await c.send_command(cmd)
    _print_resp(r)
    await c.disconnect()

async def ble_add_meter(args):
    c = DialBLEClient(args.ble)
    await c.connect()
    r = await c.send_command({
        "cmd": "add_meter",
        "sn":        args.sn,
        "mac":       args.mac.upper(),
        "bcast_key": args.key.lower(),
        "label":     args.label or args.sn
    })
    _print_resp(r)
    await c.disconnect()

async def ble_del_meter(args):
    c = DialBLEClient(args.ble)
    await c.connect()
    r = await c.send_command({"cmd": "del_meter", "sn": args.sn})
    _print_resp(r)
    await c.disconnect()

async def ble_list_meters(args):
    c = DialBLEClient(args.ble)
    await c.connect()
    r = await c.send_command({"cmd": "list_meters"})
    _print_resp(r)
    meters = r.get("d", {}).get("meters", [])
    if meters:
        print(f"\n{'#':<4} {'SN':<14} {'标签':<16} {'MAC':<18} {'在线'}")
        print("-" * 62)
        for i, m in enumerate(meters):
            online = "Y" if m.get("online") else "N"
            print(f"{i:<4} {m.get('sn',''):<14} {m.get('label',''):<16} "
                  f"{m.get('mac',''):<18} {online}")
    await c.disconnect()

async def ble_get_info(args):
    c = DialBLEClient(args.ble)
    await c.connect()
    r = await c.send_command({"cmd": "get_info"})
    _print_resp(r)
    await c.disconnect()

async def ble_get_meters(args):
    c = DialBLEClient(args.ble)
    await c.connect()
    r = await c.send_command({"cmd": "get_meters"})
    print(json.dumps(r, indent=2, ensure_ascii=False))
    await c.disconnect()

async def ble_restart(args):
    c = DialBLEClient(args.ble)
    await c.connect()
    r = await c.send_command({"cmd": "restart"})
    _print_resp(r)
    await c.disconnect()

# ──────────────────────────────────────────
# HTTP 子命令实现
# ──────────────────────────────────────────
def http_add_meter(args):
    cli = DialHTTPClient(args.http)
    body = {"cmd": "add_meter", "sn": args.sn, "mac": args.mac.upper(),
            "bcast_key": args.key.lower(), "label": args.label or args.sn}
    r = cli.post("/config/meters", body)
    _print_resp(r)

def http_del_meter(args):
    cli = DialHTTPClient(args.http)
    r = cli.delete(f"/config/meters/{args.sn}")
    _print_resp(r)

def http_list_meters(args):
    cli = DialHTTPClient(args.http)
    r = cli.get("/config/meters")
    _print_resp(r)
    meters = r.get("d", {}).get("meters", [])
    if meters:
        print(f"\n{'#':<4} {'SN':<14} {'标签':<16} {'MAC':<18}")
        print("-" * 56)
        for i, m in enumerate(meters):
            print(f"{i:<4} {m.get('sn',''):<14} {m.get('label',''):<16} {m.get('mac',''):<18}")

def http_get_data(args):
    cli = DialHTTPClient(args.http)
    path = f"/api/meters/{args.sn}" if args.sn else "/api/meters"
    r = cli.get(path)
    print(json.dumps(r, indent=2, ensure_ascii=False))

def http_set_mqtt(args):
    cli = DialHTTPClient(args.http)
    body = {"host": args.host, "port": args.port,
            "user": args.user or "", "pass": args.password or "",
            "tls": 1 if args.tls else 0}
    r = cli.post("/config/mqtt", body)
    _print_resp(r)

def http_set_wifi(args):
    cli = DialHTTPClient(args.http)
    r = cli.post("/config/wifi", {"ssid": args.ssid, "pass": args.password})
    _print_resp(r)

def http_get_info(args):
    cli = DialHTTPClient(args.http)
    r = cli.get("/api/info")
    print(json.dumps(r, indent=2, ensure_ascii=False))

def http_restart(args):
    cli = DialHTTPClient(args.http)
    r = cli.post("/sys/restart", {})
    _print_resp(r)

# ──────────────────────────────────────────
# 工具函数
# ──────────────────────────────────────────
def _print_resp(r: dict):
    ok = r.get("ok", False)
    msg = r.get("msg", "")
    status = "[OK] 成功" if ok else "[FAIL] 失败"
    print(f"{status} {msg}")
    d = r.get("d")
    if d:
        print(json.dumps(d, indent=2, ensure_ascii=False))

# ──────────────────────────────────────────
# 参数解析
# ──────────────────────────────────────────
def build_parser():
    parser = argparse.ArgumentParser(
        prog="dial_config",
        description="Bituo Dial 配置工具 v1.1 — BLE 明文 / HTTP 双通道"
    )

    # 全局互斥：--ble 或 --http
    transport = parser.add_mutually_exclusive_group()
    transport.add_argument("--ble",  metavar="NAME",
                           help="BLE 设备名（默认 BitUo-Dial）", default=None)
    transport.add_argument("--http", metavar="HOST",
                           help="Dial 的 IP 或 mDNS 地址（如 192.168.1.100 或 bituo-dial.local）",
                           default=None)

    sub = parser.add_subparsers(dest="action", required=True)

    # scan（仅 BLE）
    sub.add_parser("scan", help="扫描附近的 BitUo-Dial 设备")

    # setwifi
    p = sub.add_parser("setwifi", help="配置 Wi-Fi")
    p.add_argument("--ssid",     required=True)
    p.add_argument("--password", required=True)

    # set-mqtt
    p = sub.add_parser("set-mqtt", help="配置 MQTT Broker")
    p.add_argument("--host",     required=True)
    p.add_argument("--port",     type=int, default=1883)
    p.add_argument("--user",     default="")
    p.add_argument("--password", default="")
    p.add_argument("--tls",      action="store_true", help="启用 TLS")

    # add-meter
    p = sub.add_parser("add-meter", help="添加电表")
    p.add_argument("--sn",    required=True, help="电表 SN，如 50701B597664")
    p.add_argument("--mac",   required=True, help="电表 MAC，如 AA:BB:CC:DD:EE:FF")
    p.add_argument("--key",   required=True, help="broadcast_key（32位十六进制）")
    p.add_argument("--label", default="",    help="显示标签")

    # del-meter
    p = sub.add_parser("del-meter", help="删除电表")
    p.add_argument("--sn", required=True)

    # list-meters
    sub.add_parser("list-meters", help="列出已配置电表")

    # get-data
    p = sub.add_parser("get-data", help="查询电表实时数据（HTTP）")
    p.add_argument("--sn", default="", help="指定 SN；不填则返回全部")

    # get-info
    sub.add_parser("get-info", help="查询 Dial 设备信息")

    # get-meters（BLE 通道查询实时数据）
    sub.add_parser("get-meters", help="查询所有表实时数据（BLE）")

    # restart
    sub.add_parser("restart", help="重启 Dial 设备")

    return parser

# ──────────────────────────────────────────
# 主入口
# ──────────────────────────────────────────
BLE_ACTIONS = {
    "scan":        ble_scan,
    "setwifi":     ble_setwifi,
    "set-mqtt":    ble_set_mqtt,
    "add-meter":   ble_add_meter,
    "del-meter":   ble_del_meter,
    "list-meters": ble_list_meters,
    "get-info":    ble_get_info,
    "get-meters":  ble_get_meters,
    "restart":     ble_restart,
}
HTTP_ACTIONS = {
    "setwifi":     http_set_wifi,
    "set-mqtt":    http_set_mqtt,
    "add-meter":   http_add_meter,
    "del-meter":   http_del_meter,
    "list-meters": http_list_meters,
    "get-data":    http_get_data,
    "get-info":    http_get_info,
    "restart":     http_restart,
}

def main():
    parser = build_parser()
    args = parser.parse_args()

    if args.action == "scan" or args.ble is not None:
        # BLE 通道
        if args.ble is None:
            args.ble = "BitUo-Dial"
        handler = BLE_ACTIONS.get(args.action)
        if not handler:
            print(f"[错误] BLE 通道不支持 '{args.action}'")
            sys.exit(1)
        asyncio.run(handler(args))

    elif args.http is not None:
        # HTTP 通道
        handler = HTTP_ACTIONS.get(args.action)
        if not handler:
            print(f"[错误] HTTP 通道不支持 '{args.action}'")
            sys.exit(1)
        handler(args)

    else:
        parser.print_help()

if __name__ == "__main__":
    main()
