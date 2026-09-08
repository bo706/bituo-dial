# Bituo Dial 系统架构与通讯流程

固件 **v1.1.0**（ESP-IDF v5.2.3 / LVGL v8.3）。本文描述截至 2026-09-07 的**已落地**链路：电表 BLE 广播 → Dial 解密与显示 → Wi-Fi MQTT/HTTP → EMQX → MQTTX / Home Assistant。细节协议见 [data_dict.md](data_dict.md)、[mqtt.md](mqtt.md)、[api.md](api.md)。

---

## 1. 角色

SPM / SDM 电表只发 AES-128-CCM 加密厂商广播，手机 App 才能读。Dial（M5Stack Dial，ESP32-S3）做成独立网关：被动扫描、本地解密、圆屏显示，再经 2.4 GHz Wi-Fi 北向上报，无需手机值守。

电表 BLE 地址是随机地址（RPA）。**入网与匹配只靠 12 位 SN + 32 hex `broadcast_key` 盲解（CCM Tag 过即命中），禁止按 MAC。** 设备标识用 Dial SN（如 `DIAL-0180B4`），Topic 与 JSON 里同样禁止用 MAC。

---

## 2. 系统全貌

```
  SPM01 / SPM02 / SDM01          仅 BLE 广播，无电表 Wi-Fi
           │  ~200 ms，26B 厂商数据
           ▼
     ┌─────────────────────────────────────────┐
     │  M5Dial  ESP32-S3（8MB Flash，无 PSRAM） │
     │  Core0: Wi-Fi + NimBLE                  │
     │  Core1: UI / sys / MQTT 周期任务        │
     │  扫描解密 → 圆屏三页 → HTTP:80 / MQTT   │
     └───────────────┬─────────────────────────┘
                     │ STA 2.4 GHz（例：Bituo-ext）
                     │  配网失败：SoftAP 192.168.4.1
                     ▼
              局域网 PC（Docker）
         ┌────────────┴────────────┐
         │  EMQX :1883             │
         │   ├─ MQTTX 订阅调试     │
         │   └─ Home Assistant     │
         │        曲线 / 能源页    │
         └─────────────────────────┘
```

本机联调：Dial STA `192.168.50.151`，Broker `192.168.50.32:1883`，HA `http://127.0.0.1:8123`。电脑关机后 Dial 仍可挂在路由器上，但 MQTT/HA 会停。

---

## 3. Dial 内部

| 模块 | 职责 |
| --- | --- |
| `ble_scanner` | NimBLE 被动扫描（itvl 50 ms / window 40 ms，不发 SCAN_REQ）；最多 16 槽 |
| `crypto` | AES-128-CCM，Nonce = `8×0x00 ‖ BE32(counter)` |
| `meter_store` | NVS 掉电保持 SN/key/量测快照 |
| `ui_task` | 概览 / 详情 / 系统；短按翻页，旋转选表 |
| `cmd_dispatch` | 8 条命令，GATT / HTTP / MQTT `cmd` 共用 |
| `http_server` | REST + `/dash` 仪表盘 + 配网页 |
| `mqtt_client` | 约 10 s 上报；Client ID = Dial SN |
| `sys_task` | STA、SoftAP 回落、mDNS `bituo-dial.local`；连上后关 Wi-Fi 省电，避免 BLE 共存把 STA 踢死 |

GPIO46 上电立即拉高（HOLD）。屏 GC9A01 SPI，MADCTL `0x48`（USB 在下）。Wi-Fi **仅 2.4 GHz**。

---

## 4. 南向：电表 → Dial

空中长包 26 字节：`FF FF | ctrl | counter(LE u32) | 15B 密文 | 4B Tag`。短包 5 字节是未配网状态，不解密。

`ctrl`：GATT 已装、编码版本（当前 1）、相位（三相每帧只带一相、轮换）、产品类别。明文 15 字节（全小端）：U(V)、I(A)、P(**kW**，int24 可负)、正向/反向电能(kWh)。**广播无 PF**，界面固定 `PF --`，禁止臆造。

开路时 I/P/E 为 0 是物理结果。UI：≤15 s 白，15–90 s 黄，>90 s 灰/离线。

配置命令（`get_info` / `setwifi` / `set_mqtt` / `add_meter` / `del_meter` / `list_meters` / `get_meters` / `restart`）信封统一：

```json
{"ok": true, "msg": "", "event": "<cmd>", "d": {}}
```

三条入口：GATT 服务 `A003`（PC：`tools/dial_config.py`）、HTTP `POST /config`、MQTT `bituo-dial/{DialSN}/cmd` → 响应 `cdata`。

---

## 5. 北向：Dial → Broker → 应用

Topic 前缀 `bituo-dial/{DialSN}/`：

| Topic | 方向 | 说明 |
| --- | --- | --- |
| `meters/{MeterSN}/data` | 上报 | 单表；功率字段为 **W**（内部 kW ×1000） |
| `summary` | 上报 | 在线表数与每表功率 |
| `mdata` | 上报 Retain | Dial 元数据 |
| `cmd` / `cdata` | 下发 / 应答 | 与 HTTP/GATT 同一套命令 |

HTTP 的 `get_meters` 功率仍是 **kW**，与 MQTT data 的 W 不同。不含 PF。

上层（本机）：

1. **EMQX** Docker，账号接入；Mosquitto 若占 1883 需停掉。
2. **MQTTX** 订 `bituo-dial/DIAL-0180B4/#` 看原始 JSON。
3. **Home Assistant** MQTT yaml 传感器。曲线页看功率/电压/电流/累计电能；**能源页看时段增量**（`TotalForwardEnergy`，`state_class: total_increasing`）。累计读数不变时能源柱为 0 是正常的。BituoPMD 与 Dial Topic 不兼容，不安装。

---

## 6. 一次完整数据路径

1. 电表加密广播 → Dial 用该表 `broadcast_key` CCM 解密，按相位合并进槽位。
2. 圆屏与 `GET /api/meters`、`GET /dash` 读同一快照。
3. STA 拿到 IP 后 MQTT 连接，约 10 s 发 `data` / `summary`。
4. EMQX 分发给 MQTTX 与 HA；HA Recorder 记历史；能源页用小时统计的 **变化量**。
5. 改 Wi-Fi/MQTT/加表：三条配置通道任一写入 NVS，`sys_task` / MQTT 客户端按事件重连。

TLS MQTT（8883）未接线。任务书 24 h 长测未满；Wi-Fi 与 BLE 共存导致的短暂 MQTT 重连仍可能发生，STA 不应再因误开 SoftAP 而必须拔 USB 才能恢复。
