# HTTP / 配置命令 API

统一 JSON 信封（GATT Notify、HTTP、MQTT `cdata` 相同）：

```json
{"ok": true, "msg": "", "event": "<cmd>", "d": {}}
```

无 token 时（出厂 `g_api_token` 空）HTTP 不校验。若设置了 token，请求头加 `X-Api-Token`。

局域网：`http://<STA-IP>/` 或 `http://bituo-dial.local/`。ESP32-S3 **只能 2.4 GHz Wi-Fi**。

## REST（任务书 5.8）

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/` | 内嵌配网页（Wi-Fi / MQTT） |
| GET | `/dash` | 三表仪表盘（浏览器每 2s 刷新） |
| GET | `/api/info` | `get_info` |
| GET | `/api/meters` | `get_meters` |
| GET | `/api/meters/{sn}` | 单表；不存在 `sn not found` |
| GET | `/config` | Wi-Fi SSID、MQTT host/port/tls、表数量（不含密码） |
| POST | `/config` | body 为通用命令 JSON |
| POST | `/config/wifi` | `setwifi`（`ssid`,`pass`） |
| POST | `/config/mqtt` | `set_mqtt` |
| GET | `/config/meters` | `list_meters` |
| POST | `/config/meters` | `add_meter` |
| DELETE | `/config/meters/{sn}` | `del_meter` |
| POST | `/sys/restart` | 延迟 500ms 重启 |

PowerShell 请用 `curl.exe`，不要用别名 `curl`。

## 命令（7.3）

| cmd | 必填 | 说明 |
| --- | --- | --- |
| `get_info` | — | SN、版本、IP、Wi-Fi/MQTT、扫描、uptime |
| `setwifi` | `ssid`,`pass` | 写入 NVS 并重连；无凭据或 60s 失败则 SoftAP |
| `set_mqtt` | `host` | 可选 `port`,`user`,`pass`,`tls` |
| `add_meter` | `sn`,`mac`,`bcast_key` | 可选 `label`；SN 12 hex，key 32 hex；最多 16 槽；重复 SN 覆盖 |
| `del_meter` | `sn` | |
| `list_meters` | — | 配置列表 + `online`（队列 valid） |
| `get_meters` | — | 实时量测；功率字段为 **kW**（与 MQTT data 的 W 不同） |
| `restart` | — | |

`get_info.d.ble_scan.online_count`：深度 1 队列 `valid=true` 的表数（>30s 无广播由 UI 置 false）。

识别设备用 `dial_sn`（`DIAL-` + MAC 后三字节），不要用蓝牙 MAC。

## SoftAP 配网

无 STA 或 60s 连不上：热点 `BitUo-Dial-XXXXXX` / 密码 `12345678`，页面 `http://192.168.4.1/`。保存 Wi-Fi 后保持 APSTA 片刻以便 HTTP 返回。手机可待在 5G SSID，只要与 Dial 的 2.4G 同网段。

GATT 通道 UUID 见任务书 5.7.1（服务 A003）；PC 工具 `tools/dial_config.py`。
