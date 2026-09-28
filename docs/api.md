# HTTP / 配置命令 API

统一 JSON 信封（GATT Notify、HTTP、MQTT `cdata` 相同）：

```json
{"ok": true, "msg": "", "event": "<cmd>", "d": {}}
```

局域网：**默认关闭**。系统页长按 Setup → LAN web 打开后，才有 `http://<STA-IP>/` 或 `http://bituo-dial.local/`。ESP32-S3 **只能 2.4 GHz Wi-Fi**。HTTP 明文（需求 5：不做设备 HTTPS）；密码与 `bcast_key` 不在 GET 里回显。

无 token 时（出厂 `g_api_token` 空）HTTP 不校验。若设置了 token：请求头 `X-Api-Token` **或** 查询参数 `token=`（电表风格）。

## REST（任务书 5.8，三标签网页仍用这些）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 三标签页：状态数据 / 添加电表 / 配置（`main/index.html` 内嵌固件，约 10KB） |
| GET | `/dash` | 同上（兼容旧书签） |
| GET | `/api/info` | `get_info` |
| GET | `/api/meters` | `get_meters`，功率 **kW** |
| GET | `/api/meters/{sn}` | 单表；不存在 `sn not found` |
| GET | `/api/scan/wifi` | `scan_wifi` |
| GET | `/api/scan/ble` | `scan_ble` |
| GET | `/config` | Wi-Fi SSID、MQTT host/port/tls、http_lan（不含密码） |
| POST | `/config` | body 为通用命令 JSON（含 `set_meter_label`）；亦接受表单 `plain=` |
| POST | `/config/wifi` | `setwifi`（`ssid`,`pass`） |
| POST | `/config/mqtt` | `set_mqtt` |
| POST | `/config/http` | `set_http`（`enable`） |
| GET | `/config/meters` | `list_meters` |
| POST | `/config/meters` | `add_meter` |
| DELETE | `/config/meters/{sn}` | `del_meter` |
| POST | `/sys/restart` | `restart` |
| POST | `/ota` | 上传 `bituo-dial.bin`（`Content-Type: application/octet-stream`），双槽 OTA |
| GET | `/api/ota` | 同 `get_info`（含 `d.ota` 槽位/状态） |

## 电表同名路径（需求 5，与 SPM/SDM HTTP 对齐）

与上一表进同一套命令分发。三标签网页不必改。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| POST | `/save-config` | 同 `POST /config`。JSON 正文或表单字段 `plain=<JSON>` |
| GET | `/data` | 多表量测，帕斯卡字段，功率 **W**（与 MQTT `data` 相同） |
| GET | `/model` | `get_info` 的 `d`，`event=model` |
| GET | `/status` | Wi-Fi / MQTT / HTTP / 在线表数摘要 |
| GET | `/snapshot` | `d.data` + `d.model` + `d.status` 一次返回 |
| GET | `/sn` | 纯文本一行：`bituo-dial DIAL-0180B4` |
| GET | `/restart` | 同 `POST /sys/restart` |
| GET | `/signin` | 未设 token：`ok` + `msg=open`；已设则须 token |

不做：HTTPS 443、`GET /ota?url=` 拉包、继电器、电能清零、恢复出厂擦除。升级仍是 `POST /ota` 上传 bin。

PowerShell 请用 `curl.exe`，不要用别名 `curl`。

## 命令（7.3）

| cmd | 必填 | 说明 |
| --- | --- | --- |
| `get_info` | — | SN、版本、IP、Wi-Fi/MQTT/http、扫描、uptime |
| `setwifi` / `wifi` | `ssid`,`pass` | 写入 NVS 并重连；`wifi` 为电表侧别名 |
| `set_mqtt` / `mqtt` / `mqtr` | `host` | 可选 `port`,`user`,`pass`,`tls`,`insecure`；`tls=1` 时默认端口 8883；`mqtt`/`mqtr` 为电表侧别名 |
| `set_http` | `enable` | 局域网 HTTP 开关，默认关，掉电保持 |
| `scan_wifi` | — | 附近 2.4G AP：`d.aps[]` ssid/rssi/auth |
| `scan_ble` | — | 附近 SPM/SDM：`d.meters[]` name/sn/rssi/adv/configured |
| `add_meter` | `sn`,`bcast_key` | 可选 `mac`,`label`；无 mac 时填 `00:00:00:00:00:00`；SN 12 hex，key 32 hex |
| `set_meter_label` | `sn` | 仅改显示名（厨房/空调支路），不改密钥；`label` 可空则回退 SN |
| `del_meter` | `sn` | |
| `list_meters` | — | 配置列表 + `online`（队列 valid） |
| `get_meters` | — | 实时量测；功率字段为 **kW**（与 MQTT data 的 W 不同） |
| `restart` | — | |

`get_info.d.ble_scan.online_count`：深度 1 队列 `valid=true` 的表数（>30s 无广播由 UI 置 false）。

识别设备用 `dial_sn`（`DIAL-` + MAC 后三字节），不要用蓝牙 MAC。

## SoftAP 配网

无 STA 或 60s 连不上仍会开热点 `BitUo-Dial-XXXXXX` / 密码 `12345678`。**热点网页同样要先打开 LAN web。** 首次配网走圆屏 Setup → Wi-Fi scan。设置菜单里可手动开热点。

GATT 通道 UUID 见任务书 5.7.1（服务 A003）；PC 工具 `tools/dial_config.py`。
