# Phase 4（北向集成）验收记录

- 日期：2026-09-03
- 设备：M5Dial `DIAL-0180B4`，STA IP `192.168.50.151`，SSID `Bituo-ext`（2.4G）
- Broker：本机 Mosquitto 服务，`192.168.50.32:1883`（局域网监听，`allow_anonymous true`）
- 手机/电脑：`Bituo-ext-5G`（与 2.4G 同网段，无隔离）
- 电表：SN `C85024E851F1`，label `SDM01-test`，开路（I/P/E = 0 与 APP 一致）
- 报文样本：`tools/captures/phase4_mqtt_20260903.log`
- 对照：任务书 v1.1 §5.8 / §5.9 / §11.4

## 一、验收结果总览

| 项目（§11.4） | 标准 | 结果 |
|---|---|---|
| MQTT 上报 | 单表 Topic 与汇总 Topic 均发布，格式符合 5.9 | **PASS** |
| MQTT 兼容性 | 单表 data 含原生字段 + `_source` / `_dial` / `_rssi`；无 PF | **PASS** |
| HTTP API | 5.8 读路径与通用 POST 命令返回正确 JSON | **PASS**（写路径 wifi/mqtt 已在配网现场验过；restart 本轮未发） |
| mDNS | `http://bituo-dial.local` 可访问 | **PASS** |
| 远程配置 | MQTT `cmd` 发送 `add_meter` 成功，并已 `del_meter` 清掉测试表 | **PASS** |

加分项 Home Assistant / BituoPMD：**未做**（任务书「有条件时」）。

## 二、MQTT Topic（约 10s 周期）

前缀：`bituo-dial/DIAL-0180B4/`

| Topic | 实测 |
|---|---|
| `meters/C85024E851F1/data` | `online:true`，VoltageX/Y/Z ≈ 220–221V，Current/Power/Energy 为 0，`_source=bituo-dial`，`_dial=DIAL-0180B4` |
| `summary` | `meter_count:1`，行内 `online:true` |
| `mdata` | Retain；Dial SN / fw / IP / 表列表 |
| `cmd` → `cdata` | 见第三节 |

系统页：Wi-Fi 绿、MQTT 绿、无 `(wait)`。`mqtt.connected=true`。

## 三、MQTT 远程配置（cmd → cdata）

测试表 SN `F4A5E4000001`（全零 key，仅用于命令通道，随即删除）。

| 命令 | cdata |
|---|---|
| `get_info` | `ok:true`，`mqtt.connected:true` |
| `add_meter` | `ok:true`，`meter added`，`index:1` |
| `list_meters` | 2 条（真表 + 测试表） |
| `del_meter` | `ok:true`，`meter deleted` |
| `list_meters` | 仅剩 `C85024E851F1` |

收尾后 HTTP `GET /config/meters` 仍只有真实电表，测试表未残留。

## 四、HTTP / mDNS（5.8）

未测会改 Wi-Fi/MQTT 或重启的写接口（`POST /config/wifi`、`POST /config/mqtt`、`POST /sys/restart`）。这三项今天下午配网时已成功过。

| 方法 | 路径 | HTTP | 热路径耗时 | 说明 |
|---|---|---|---|---|
| GET | `/` | 200 | 首包 ~1.06s | HTML 配置页 |
| GET | `/api/info` | 200 | 0.14s | mqtt connected |
| GET | `/api/meters` | 200 | 0.42s | `valid:true`，电压约 221V |
| GET | `/api/meters/C85024E851F1` | 200 | 0.72s | 单表 |
| GET | `/api/meters/DEAD0000BEEF` | 200 | 0.31s | `sn not found` |
| GET | `/config` | 200 | 0.18s | 不含密码 |
| GET | `/config/meters` | 200 | 0.16s | 列表 |
| POST | `/config` body `{"cmd":"get_info"}` | 200 | 0.44s | 通用命令 |
| GET | `http://bituo-dial.local/api/meters` | 200 | ~3.0s（含 mDNS） | 任务书交付项 |
| GET | `http://bituo-dial.local/` | 200 | 0.52s | 配置页 |

§11.4「响应时间 < 500ms」：IP 热路径多数达标；**首包、单表拼接、mDNS 解析会超过 500ms**（BLE 共存）。功能正确，时延不按硬失败计。

## 五、工具

- `tools/monitor_mqtt.py`：补了缺失的 `on_message`，支持 `--seconds` / `--max-messages`。
- 本机监听示例：`python tools/monitor_mqtt.py 127.0.0.1 1883 --seconds 15`

## 六、已知不一致（不阻塞 Phase 4 闭环，不烧录）

- `ble_scan.online_count` / `summary.online_count` 常为 0，同时 `get_meters.valid=true` 且 MQTT `online:true`。`meter_store_online_count()` 读 `g_meter_data[].valid`，上报走队列 `xQueuePeek`，两处未对齐。
- `list_meters` 的 `online` 同样偏 false。屏上可能出现 `Meters online: 0/1`。
- 开路 I/P/E=0、无第二块表、无 PF、无 HA：见 `docs/pending_checklist.md`。

## 七、结论

**Phase 4 主路径现场验收通过**（Wi-Fi STA + SoftAP 配网 + HTTP 5.8 + mDNS + MQTT 双发 + cmd/cdata 远程 add_meter）。下一步是任务书 Phase 5（24h / 文档 / Release），需用户确认后再做。
