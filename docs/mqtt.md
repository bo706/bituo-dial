# MQTT 北向

固件：`main/mqtt_client.c`。Client ID = Dial SN（如 `DIAL-0180B4`），**Topic 用 DialSN / MeterSN，禁止 MAC。**

Broker 在配置页或 `set_mqtt` 写入。周期约 10s（`MQTT_SUMMARY_PERIOD_MS`）。无 host 或 STA 无 IP 时不启动。

## Topic

前缀 `bituo-dial/{DialSN}/`

| Topic | 方向 | 说明 |
| --- | --- | --- |
| `meters/{MeterSN}/data` | 上报 | 单表，约 10s；功率字段为 **W** |
| `summary` | 上报 | 全表汇总 |
| `mdata` | 上报 | Dial 元数据，**Retain=1** |
| `cmd` | 订阅 QoS1 | 与 GATT/HTTP 相同 JSON 命令 |
| `cdata` | 上报 | 命令响应 |

监听：

```
python tools/monitor_mqtt.py 127.0.0.1 1883 --seconds 15
```

或 `mosquitto_sub -h 127.0.0.1 -t "bituo-dial/#" -v`

## data 字段

与原生电表风格对齐，额外 `_source` / `_dial` / `_rssi`。内部存储是 kW，**上报时 ×1000 变成 W**。

| 字段 | 含义 |
| --- | --- |
| `sn` `label` `online` | 表号、标签、队列 `valid` |
| `VoltageX/Y/Z` | V，字符串 |
| `CurrentX/Y/Z` | A |
| `ActivePowerX/Y/Z` `TotalActivePower` | **W** |
| `TotalForwardEnergy` `TotalReverseEnergy` | kWh，三相电能之和 |
| `_source` | 固定 `bituo-dial` |
| `_dial` | Dial SN |
| `_rssi` | dBm |

**不含 PF 字段。**

## summary / mdata

`summary`：`dial_sn`、`time`（uptime 秒）、`meter_count`、`online_count`、每表 `sn/label/online/TotalActivePower(W)/_rssi`。

`mdata`：`dial_sn`、`fw_ver`、`ip`、已配置表 `sn/label`。

## cmd / cdata

向 `.../cmd` 发 `{"cmd":"get_info"}` 等，响应在 `.../cdata`，信封与 HTTP/GATT 相同（`ok/msg/event/d`）。命令表见 [api.md](api.md)。

实测：`docs/phase4_acceptance_20260903.md`，样本 `tools/captures/phase4_mqtt_20260903.log`。

系统页 MQTT 绿=已连接，黄 `(wait)`=TCP 暂断（BLE 共存常见，会自动重连）。
