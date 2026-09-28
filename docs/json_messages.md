# Bituo Dial JSON 报文手册

固件 **v1.1.0**。GATT、HTTP、MQTT `cmd`/`cdata` 共用同一套命令 JSON。MQTT 周期遥测（`data` / `summary` / `mdata`）是另一套字段风格。本文按**固件实际发出/接收**的字段编写。不含电表 `broadcast_key`、Wi-Fi/MQTT 密码。

约定：`{DialSN}` 例 `DIAL-0180B4`；`{MeterSN}` 12 位大写 hex。功率：**HTTP `get_meters` 为 kW**，**MQTT `data`/`summary` 为 W**。广播无 PF，MQTT 不带 PF 字段；HTTP 里 `overall_power_factor` 恒为 `0` 表示未测，不是实测零。

---

## 1. 统一命令信封

请求（GATT 写 C313、HTTP `POST /config`、MQTT `bituo-dial/{DialSN}/cmd`）：

```json
{"cmd":"<name>", "...可选参数"}
```

响应（GATT Notify C315 / 读 C314、HTTP 响应体、MQTT `bituo-dial/{DialSN}/cdata`）：

```json
{"ok": true, "msg": "", "event": "<cmd>", "d": {}}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `ok` | bool | 命令是否成功 |
| `msg` | string | 空或错误/提示短句 |
| `event` | string | 回显 `cmd` 名 |
| `d` | object | 成功时的数据；失败时常为 `{}` |

未知命令：`{"ok":false,"msg":"unknown cmd","event":"<cmd>","d":{}}`。HTTP body 过大：`msg` = `body too large`。设了 API token 时 HTTP 需头 `X-Api-Token`，否则 403。

GATT：服务 UUID `A003`；写 `C313`；读上次响应 `C314`；Notify `C315`。MTU 协商 512。明文 JSON，无 J-PAKE。

---

## 2. 命令

### 2.1 `get_info`

请求：`{"cmd":"get_info"}`  
HTTP：`GET /api/info`

```json
{
  "ok": true,
  "msg": "",
  "event": "get_info",
  "d": {
    "dial_sn": "DIAL-0180B4",
    "fw_ver": "1.1.0",
    "hw_ver": "M5Dial-r1",
    "ip": "192.168.1.100",
    "wifi": { "ssid": "YourSSID", "rssi": -60, "connected": true },
    "mqtt": { "host": "192.168.1.10", "port": 1883, "tls": 0, "insecure": 1, "connected": true },
    "http": { "enabled": false },
    "ble_scan": { "running": true, "meter_count": 3, "online_count": 3 },
    "uptime_s": 2192
  }
}
```

`wifi.connected`：STA 已拿到 IPv4。`http.enabled`：局域网 HTTP 是否在听 80。`ble_scan.online_count`：队列 `valid=true` 的表数。

### 2.2 `setwifi`

```json
{"cmd":"setwifi","ssid":"YourSSID","pass":"<wifi-password>"}
```

电表别名：`{"cmd":"wifi","ssid":"...","pass":"..."}` 等同 `setwifi`。  
HTTP：`POST /config/wifi`，body 可省略 `cmd`（服务端注入）。  
成功：`ok=true`，`msg`=`wifi connecting`，`d={}`。失败：`invalid ssid` / `nvs write failed`。

### 2.3 `set_mqtt`

```json
{"cmd":"set_mqtt","host":"192.168.1.10","port":1883,"user":"mqtt_user","pass":"<broker-password>","tls":0,"insecure":1}
```

`host` 必填。未给 `port` 时：`tls=0` 默认 1883，`tls=1` 默认 8883。`tls=1` 为单向 MQTTS（无客户端证书）。`insecure` 缺省 **1**（不校验自签证书，实验室）；生产应 `insecure=0` 并用公开 CA 包。  
电表别名：`{"cmd":"mqtt",...}` 或 `{"cmd":"mqtr",...}` 等同 `set_mqtt`。  
HTTP：`POST /config/mqtt`。成功 `msg`=`mqtt saved`。

### 2.3b `set_http`

```json
{"cmd":"set_http","enable":1}
```

NVS `http_lan`，默认 0。`enable` 0/1。成功后约 400ms 启停 httpd。关 HTTP 后当前网页会断。HTTP：`POST /config/http`。

### 2.3c `scan_wifi`

请求：`{"cmd":"scan_wifi"}`  
HTTP：`GET /api/scan/wifi`  
`d.aps[]`：`ssid`、`rssi`、`auth`（是否加密）。最多 16 条；扫描时短暂停 BLE。

### 2.3d `scan_ble`

请求：`{"cmd":"scan_ble"}`  
HTTP：`GET /api/scan/ble`  
`d.meters[]`：`name`、`sn`（能从名称解析的 12 hex，否则 `""`）、`rssi`、`adv`（`short`/`long`）、`configured`。不按 MAC。短包可能没有完整 SN。

### 2.4 `add_meter`

```json
{
  "cmd": "add_meter",
  "sn": "112233445566",
  "mac": "00:00:00:00:00:00",
  "bcast_key": "<32-hex-broadcast_key>",
  "label": "SPM02"
}
```

`sn` 12 hex；`bcast_key` 32 hex；`mac` 形如 `AA:BB:CC:DD:EE:FF`（匹配不靠 MAC，可填占位）；`label` 可选。最多 16 槽，重复 SN 覆盖。  
HTTP：`POST /config/meters`。

成功：

```json
{"ok":true,"msg":"meter added","event":"add_meter","d":{"sn":"112233445566","index":0}}
```

失败 `msg`：`meter list full (max 16)` / `invalid sn (expect 12 hex)` / `invalid key (expect 32 hex)` / `invalid mac (expect AA:BB:CC:DD:EE:FF)` / `nvs write failed`。

### 2.4b `set_meter_label`

仅改回路名称，不覆盖广播密钥。

```json
{"cmd":"set_meter_label","sn":"112233445566","label":"厨房"}
```

HTTP：`POST /config`（body 自带 `cmd`）。成功 `msg`=`label saved`。失败：`sn not found` / `invalid sn (expect 12 hex)` / `nvs write failed`。`label` 为空时回退为 SN。

### 2.5 `del_meter`

请求：`{"cmd":"del_meter","sn":"112233445566"}`  
HTTP：`DELETE /config/meters/{sn}`  
成功 `msg`=`meter deleted`；失败 `sn not found`。

### 2.6 `list_meters`

请求：`{"cmd":"list_meters"}`  
HTTP：`GET /config/meters`

```json
{
  "ok": true,
  "msg": "",
  "event": "list_meters",
  "d": {
    "meters": [
      {
        "sn": "112233445566",
        "label": "SPM02",
        "mac": "00:00:00:00:00:00",
        "online": true
      }
    ]
  }
}
```

不含 `bcast_key`。

### 2.7 `get_meters`（功率单位 **kW**）

请求：`{"cmd":"get_meters"}`  
HTTP：`GET /api/meters`  
数值字段为 JSON number（固件用 raw 避免 float 长尾）。**不含 MQTT 那种 `VoltageX` 字符串。**

```json
{
  "ok": true,
  "msg": "",
  "event": "get_meters",
  "d": {
    "dial_sn": "DIAL-0180B4",
    "time": 2192,
    "meters": [
      {
        "sn": "112233445566",
        "label": "SPM02",
        "rssi": -42,
        "counter": 1545,
        "timestamp": 3556,
        "voltage_x": 222.0,
        "voltage_y": 221.7,
        "voltage_z": 221.6,
        "current_x": 0.000,
        "current_y": 0.000,
        "current_z": 0.000,
        "active_power_x": 0.000,
        "active_power_y": 0.000,
        "active_power_z": 0.000,
        "forward_energy_x": 0.00,
        "forward_energy_y": 0.00,
        "forward_energy_z": 0.00,
        "reverse_energy_x": 0.00,
        "reverse_energy_y": 0.00,
        "reverse_energy_z": 0.00,
        "total_active_power": 0.000,
        "overall_power_factor": 0,
        "valid": true
      }
    ]
  }
}
```

| 字段 | 单位 | 说明 |
| --- | --- | --- |
| `voltage_*` | V | 三相；单相表未刷新相为 0 |
| `current_*` | A | |
| `active_power_*` / `total_active_power` | **kW** | 可负 |
| `forward_energy_*` / `reverse_energy_*` | kWh | 分相；MQTT 的 Total* 是三相之和 |
| `counter` | — | 广播计数器 |
| `timestamp` | ms | 相对开机 |
| `valid` | bool | 最近广播有效 |
| `overall_power_factor` | — | **恒 0，未测** |

`GET /api/meters/{sn}` 成功时把该表对象放在 `d.meter`（不是数组）。不存在：

```json
{"ok":false,"msg":"sn not found","event":"get_meters","d":{}}
```

### 2.8 `restart`

请求：`{"cmd":"restart"}`  
HTTP：`POST /sys/restart`  
响应：`{"ok":true,"msg":"restarting","event":"restart","d":{}}`，约 500 ms 后复位。

### 2.9 局域网 OTA（HTTP，不是 cmd）

`POST /ota`，body 为 `bituo-dial.bin`（`application/octet-stream`）。成功：`{"ok":true,"msg":"ota ok, rebooting","event":"ota","d":{}}`，约 1.5 s 后重启。  
写入非活动槽；校验失败或上传中断则 `esp_ota_abort`，启动分区不变。新固件启动后约 20 s 调用 `mark_app_valid`；此前崩溃或断电，bootloader 回滚旧槽。网页配置页可上传。须先打开 LAN web。

`get_info.d.ota`：`running` / `boot` / `next` / `state` / `busy`。

---

## 3. HTTP 仅有的配置查询

`GET /config`（不是八条 cmd 之一），`event`=`get_config`。**不含密码。**

```json
{
  "ok": true,
  "msg": "",
  "event": "get_config",
  "d": {
    "wifi_ssid": "YourSSID",
    "mqtt_host": "192.168.1.10",
    "mqtt_port": 1883,
    "mqtt_tls": 0,
    "mqtt_insecure": 1,
    "http_lan": 0,
    "label": "",
    "meter_count": 3
  }
}
```

REST 对照：`GET /` 状态+配置页（HTTP 默认关，需屏幕打开 LAN web）。`POST /config` 与 `POST /save-config` 的 body 就是第 2 节请求 JSON（明文 80 也接受表单 `plain=`）。`GET /api/scan/wifi`、`GET /api/scan/ble` 对应 `scan_*`。

### 3.1 电表同名 HTTP（需求 5）

| 路径 | event | 说明 |
| --- | --- | --- |
| `POST /save-config` | 与所执行 `cmd` 相同 | 同 `POST /config` |
| `GET /data` | `data` | `d.meters[]` 为 MQTT 单表 `data` 同字段（功率 **W** 字符串） |
| `GET /model` | `model` | `d` 同 `get_info.d` |
| `GET /status` | `status` | `wifi_connected` / `mqtt_connected` / `http_enabled` / `meter_count` / `online_count` / `ip` |
| `GET /snapshot` | `snapshot` | `d.data`、`d.model`、`d.status` |
| `GET /sn` | （纯文本） | `bituo-dial {DialSN}` |
| `GET /restart` | `restart` | 同 `POST /sys/restart` |
| `GET /signin` | `signin` | 开放模式 `msg=open` |

`GET /api/meters` 仍为 **kW** 数字字段，给网页用。

---

## 4. MQTT 周期遥测

Client ID = `{DialSN}`。前缀 `bituo-dial/{DialSN}/`。周期约 **10 s**。电压/电流/功率/电能为 **字符串**（`cJSON_AddString`）。

### 4.1 `meters/{MeterSN}/data`（功率 **W**）

QoS 0，不 Retain。无 PF。

```json
{
  "sn": "45605F097760",
  "label": "SDM01-4560",
  "online": true,
  "VoltageX": "224.1",
  "VoltageY": "224.3",
  "VoltageZ": "224.3",
  "CurrentX": "0.000",
  "CurrentY": "0.030",
  "CurrentZ": "0.000",
  "ActivePowerX": "0.0",
  "ActivePowerY": "4.0",
  "ActivePowerZ": "0.0",
  "TotalActivePower": "4.0",
  "TotalForwardEnergy": "1.03",
  "TotalReverseEnergy": "0.36",
  "_source": "bituo-dial",
  "_dial": "DIAL-0180B4",
  "_rssi": -57
}
```

| 字段 | 类型 | 单位 |
| --- | --- | --- |
| `VoltageX/Y/Z` | string | V，`%.1f` |
| `CurrentX/Y/Z` | string | A，`%.3f` |
| `ActivePowerX/Y/Z` `TotalActivePower` | string | **W**（内部 kW×1000，`%.1f`） |
| `TotalForwardEnergy` `TotalReverseEnergy` | string | kWh，三相之和，`%.2f` |
| `_source` | string | 固定 `bituo-dial` |
| `_dial` | string | Dial SN |
| `_rssi` | number | dBm |
| `online` | bool | 与 `valid` 同源 |

Home Assistant 能源页读 `TotalForwardEnergy`（累计 kWh 的**增量**）。

### 4.2 `summary`

QoS 0。`time` = 开机秒。

```json
{
  "dial_sn": "DIAL-0180B4",
  "time": 2192,
  "meter_count": 3,
  "online_count": 3,
  "meters": [
    {
      "sn": "112233445566",
      "label": "SPM02",
      "online": true,
      "TotalActivePower": "0.0",
      "_rssi": -42
    }
  ]
}
```

`TotalActivePower` 为字符串，单位 **W**。

### 4.3 `mdata`（Retain=1，QoS 1）

连上 Broker 时发一次，之后随周期更新。

```json
{
  "dial_sn": "DIAL-0180B4",
  "fw_ver": "1.1.0",
  "ip": "192.168.1.100",
  "meters": [
    { "sn": "112233445566", "label": "SPM02" }
  ]
}
```

### 4.4 `cmd` / `cdata`

订 `cmd` QoS 1。payload 同第 2 节请求；应答发到 `cdata`，同统一信封。例：发 `{"cmd":"get_info"}`，在 `cdata` 收到 2.1 的响应。

调试订阅：`bituo-dial/DIAL-0180B4/#`。

---

## 5. HTTP `get_meters` 与 MQTT `data` 对照

| 含义 | HTTP / GATT `get_meters` | MQTT `.../data` |
| --- | --- | --- |
| 电压 | `voltage_x` number V | `VoltageX` string V |
| 电流 | `current_x` number A | `CurrentX` string A |
| 分相功率 | `active_power_x` **kW** | `ActivePowerX` **W** |
| 总功率 | `total_active_power` **kW** | `TotalActivePower` **W** |
| 电能 | 分相 `forward_energy_x` kWh | 三相和 `TotalForwardEnergy` kWh |
| 在线 | `valid` | `online` |
| RSSI | `rssi` | `_rssi` |
| PF | `overall_power_factor`=0（未测） | **字段不存在** |

---

## 6. 通道一览

| 通道 | 请求 | 响应 |
| --- | --- | --- |
| GATT A003 | 写 C313 | Notify C315，可读 C314 |
| HTTP | 见第 2、3 节路径 | 同一信封 |
| MQTT | `.../cmd` | `.../cdata` |
| MQTT 遥测 | （设备主动） | `data` / `summary` / `mdata` |

BLE 电表广播本身是 26 字节二进制，不是 JSON；解密后的明文见 [data_dict.md](data_dict.md)。
