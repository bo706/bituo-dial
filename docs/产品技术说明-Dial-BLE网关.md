# Bituo Dial（BLE 电表网关）产品技术说明

| 项 | 内容 |
|---|---|
| 产品 | Bituo Dial，基于 M5Stack Dial 的 BLE 电表网关 |
| 文档性质 | 产品技术说明（规格、功能、使用），供评审与测试 |
| 固件版本 | **1.1.4** |
| 编写日期 | 2026-09-24 |
| 硬件基线 | ESP32-S3 rev v0.2，8MB Flash，无 PSRAM（真机实测，不以任务书 16MB/PSRAM 为准） |
| 配套插件 | Home Assistant：fork [bo706/BituoPMD](https://github.com/bo706/BituoPMD) **v1.1.3+**（改的是 HA，不是本固件） |

本文描述**当前已落地**的产品能力。协议细节另见同目录 `architecture.md`、`api.md`、`mqtt.md`、`data_dict.md`、`json_messages.md`。

---

## 1. 产品是什么

Bituo 的 SPM / SDM 电表在 BLE 模式下只发 **AES-128-CCM 加密厂商广播**，平时要靠手机 App 才能看数。Dial 做成独立网关：

- **南向**：被动扫描附近电表广播，用该表的 `broadcast_key` 本地解密
- **本机**：圆屏三页显示电压 / 电流 / 功率 / 电能；Wi-Fi、MQTT、表名单掉电保存在 NVS
- **北向**：经 2.4 GHz Wi-Fi 用 **MQTT 主路径**上报；局域网 HTTP 为可选配置通道（出厂关闭）

电表本身**不必**改配到 Wi-Fi。一块 Dial 最多挂 **16** 块 BLE 表。识别电表只靠 **12 位 SN + 32 hex 广播密钥**（CCM Tag 校验通过即命中），**禁止按蓝牙 MAC**——电表地址是随机私有地址（RPA），会变。

和公司已有 **EW 电表**（表自己带 Wi-Fi、网页 `GET /data`）不是同一条产品线：EW 是「每块表一个 IP」；Dial 是「一块网关、后面多块 BLE 表」，北向标识用 Dial SN 与电表 SN。

```
  SPM01 / SPM02 / SDM01          仅 BLE 广播（表不走 Wi-Fi）
           │  约 200 ms，26 字节厂商数据
           ▼
     ┌──────────────────────────────────────┐
     │  Bituo Dial（M5Stack Dial / ESP32-S3）│
     │  解密 → 圆屏显示 → MQTT（主）/ 可选 HTTP │
     └──────────────────┬───────────────────┘
                        │ 2.4 GHz STA
           ┌────────────┼────────────┐
           ▼            ▼            ▼
        MQTT Broker   局域网网页    Home Assistant
        （约 10 s，     （默认关闭）  （BituoPMD fork
         data retain）               订阅 MQTT）
```

---

## 2. 技术规格

### 2.1 硬件（本机实测）

| 项 | 规格 |
|---|---|
| 模组 | ESP32-S3 rev v0.2 |
| Flash | **8MB**，双槽 OTA（ota_0 / ota_1 各 3MB） |
| PSRAM | **无**（内部 RAM + DMA 回退） |
| 屏幕 | GC9A01 圆屏 240×240，SPI，RGB565 |
| 屏引脚 | MOSI=5，SCK=6，CS=7，DC=4，RST=8，背光=9 |
| 编码器 | A=40，B=41，按键=42（按下为低） |
| 蜂鸣器 | GPIO3 |
| 电源保持 | **GPIO46 上电必须立刻拉高**，否则约 100 ms 断电 |
| 正立方向 | MADCTL `0x48`：USB 口为下，橙色圈三角为上 |
| 无线 | Wi-Fi **仅 2.4 GHz**；BLE 被动扫描 + 可连接 GATT |
| 设备名 | BLE 广播名 `BitUo-Dial`；SN 形如 `DIAL-0180B4`（MAC 后三字节） |
| mDNS | `bituo-dial.local`（打开局域网网页后才有 HTTP） |
| USB-JTAG | 烧录用。**当作网关运行时请拔掉电脑 USB**，否则会复位芯片并干扰 Wi-Fi |

### 2.2 软件栈

| 项 | 规格 |
|---|---|
| SDK | ESP-IDF v5.2.3 |
| UI | LVGL v8.3 |
| BLE | NimBLE 被动扫描：间隔 80 ms / 窗口 20 ms（占空比 **25%**），不发 SCAN_REQ；另以约 3.5 s 扫描 / 0.7 s Wi-Fi 时隙让 MQTT 出得去 |
| 加密 | AES-128-CCM（mbedTLS），Nonce = `8×0x00 ‖ BE32(counter)`，Tag 4 字节，无 AAD |
| JSON | cJSON；GATT / HTTP / MQTT 命令共用同一套信封 |
| 存储 | NVS 保存 Wi-Fi、MQTT、HTTP 开关、最多 16 槽电表配置；量测快照在内存队列（深度 1） |
| 分区 | nvs + otadata + ota_0/ota_1 + SPIFFS；网页以内嵌 `index.html` 提供 |

### 2.3 容量与刷新

| 项 | 值 |
|---|---|
| 最大电表数 | 16 |
| 概览屏可见行 | 最多 8 行窗口，旋钮滚动 |
| 广播匹配 | 对已配置密钥盲解，不按 MAC |
| MQTT 上报 | 约 **10 s** 一轮；单表 `data` 与 `summary` **Retain=1**，QoS 1；keepalive **30 s** |
| HTTP `/data` | 打开 LAN web 后按需读取；BLE+Wi-Fi 共存时电脑/HA **常常打不开**，不能当日常主路径 |
| 在线判定 | 距上次有效广播 ≤15 s 白色；15～90 s 黄色；**>90 s 灰色离线并蜂鸣两声** |
| 掉电恢复 | Wi-Fi / MQTT / 电表列表从 NVS 恢复 |

### 2.4 量测范围（来自电表 15 字节明文）

| 量 | 编码 | 单位 |
|---|---|---|
| 电压 | uint16 ÷ 10 | V |
| 电流 | uint16 ÷ 100 | A |
| 有功功率 | 有符号 int24 ÷ 1000 | **kW**（可负）；北向 MQTT / `GET /data` 再 ×1000 变成 **W** |
| 正向 / 反向电能 | uint32 ÷ 100 | kWh |
| 功率因数 PF | **广播不含此字段** | 界面固定 `PF --`，JSON 不作为实测 0 |

三相表每帧只带一相、按帧轮换；Dial 合并上一帧快照，只刷新本相。总有功 = 三相之和。

---

## 3. 完整功能

### 3.1 已实现

| 功能 | 说明 |
|---|---|
| BLE 被动扫描与解密 | 过滤 Bituo 26 字节长包；短包（未配网）不解密 |
| 多表槽位 | 最多 16；重复 SN 覆盖；可改显示名（厨房 / 空调支路） |
| 圆屏三页 | 概览 → 详情 → 系统，短按循环 |
| Setup | 系统页长按：扫 Wi-Fi、加表、删表、局域网网页开关、热点 |
| 离线告警 | 黄 / 灰着色；离线蜂鸣两声 |
| Wi-Fi STA | 2.4 GHz。无凭据时开 SoftAP；已保存凭据则 STA-only，GOT_IP 后关掉 SoftAP，避免 APSTA 锁信道 |
| SoftAP 回落 | 无凭据，或 STA **掉线起算约 60 s** 仍连不上：SSID `BitUo-Dial-XXXXXX`，出厂密码 `12345678`，IP `192.168.4.1` |
| MQTT / MQTTS | 周期遥测 + 远程命令；Client ID = Dial SN；Topic **不用 MAC** |
| 局域网 HTTP | **默认关闭**；打开后三标签网页 + REST + 电表同名路径 |
| GATT 配置 | 服务 `A003`，写 `C313` / 读 `C314` / Notify `C315`，明文 JSON，MTU 512 |
| 双槽 OTA | `POST /ota` 上传 `bituo-dial.bin`；失败或约 20 s 内崩溃回滚旧槽 |
| mDNS | 主机名 `bituo-dial` |
| Home Assistant | BituoPMD fork **订阅 MQTT**；首次可用 IP 探测一次 `/data`，之后不依赖网页 |

### 3.2 明确不做（本版本）

- 设备侧 HTTPS（443）
- 按 URL 拉包 OTA（`GET /ota?url=`）
- 继电器、电能清零、恢复出厂擦除
- 从广播编造 PF
- 把 BLE 表再配到电表自己的 Wi-Fi（那会离开 Dial）
- 官方 [script0803/BituoPMD](https://github.com/script0803/BituoPMD) 原版（只认 EW 扁平 JSON，认不出 Dial）

---

## 4. 圆屏怎么用

操作件：外圈旋钮 + 中心按键。短按约瞬时，长按阈值 **800 ms**。

| 操作 | 效果 |
|---|---|
| 短按 | 概览 → 详情 → 系统 → 概览 |
| 概览旋转 | 多表时改选中行（蓝条） |
| 详情旋转 | 切换当前表 |
| 详情长按 | 回概览 |
| 系统页长按 | 进入 Setup |
| Setup 旋转 / 短按 | 选菜单、确认 |
| Setup 长按 | 返回上一级或退出 |

**Setup 菜单**

1. **Wi-Fi scan**：扫附近 2.4G，选 SSID，旋钮拼密码后保存（写入 NVS 并按 STA-only 重连）
2. **Add meter**：扫附近 SPM/SDM，选表后输入 32 hex `bcast_key`（可从 App 导出；重新配网会变）
3. **Delete meter**：按 SN 删除槽位
4. **LAN web**：局域网网页开关，掉电保持；菜单显示 `[ON]` / `[OFF]`
5. **Hotspot**：手动开 SoftAP
6. **Back**

保存 Wi-Fi 后系统页应显示绿色 SSID 与 STA IP，**不应**再停在黄色 SoftAP。若保存后变成 SoftAP，多半是 APSTA 锁信道，复位一次或重新保存 Wi-Fi。

**三页内容**

- **概览**：每行名称 + 电压/功率；白=在线，黄=警告，灰=离线
- **详情**：三相 U/I、总功率（kW）、正反向电能、RSSI、距上次更新秒数、`PF --`
- **系统**：Wi-Fi / IP / MQTT 状态 / BLE 扫描 / 在线表数 / 固件 / 运行时长 / 堆  
  MQTT **绿** = TCP 已连上 Broker；**黄 `(wait)`** = 未连上或正在重连（BLE 与 Wi-Fi 共存时可能闪一下，应自动回来）

---

## 5. 使用方式（部署）

### 5.1 第一次上电

1. USB 供电（GPIO46 由固件拉高保持）。当作网关跑时烧完请拔掉电脑 USB。
2. 系统页确认固件 `v1.1.4`。无 Wi-Fi 凭据时可能已开 SoftAP。
3. 长按进入 Setup → **Wi-Fi scan**，选 **2.4 GHz** SSID，输入密码保存。
4. 系统页出现 STA IP（绿色 SSID）。
5. Setup → **Add meter**：扫到表后填该表 `broadcast_key`。不要给这些 BLE 表重新配 Wi-Fi。
6. 需要 MQTT：网页「配置」、GATT 或命令 `set_mqtt` 填写与 HA 相同的 Broker；系统页 MQTT 应变绿。
7. 需要网页调试：Setup 打开 **LAN web**（出厂默认关）。浏览器访问 `http://<STA-IP>/` 或 `http://bituo-dial.local/`。HA **日常不依赖**这个网页。

无 STA 时也可连热点 `BitUo-Dial-XXXXXX`，但**热点上网页同样要先打开 LAN web**。首次配网建议走圆屏扫 Wi-Fi。

### 5.2 局域网网页（打开 LAN web 之后）

三个标签：

| 标签 | 用途 |
|---|---|
| 状态数据 | Dial SN / IP / Wi-Fi / MQTT / 各表 V·I·P·E |
| 添加电表 | 扫描附近表，填写 SN + 密钥 + 回路名 |
| 配置 | Wi-Fi、MQTT/MQTTS、改名/删表、上传 bin 做 OTA、关 HTTP、重启 |

密码和 `bcast_key` **不会**出现在 GET 回显里。

ESP32-S3 同时扫 BLE 时，电脑 ping / 打开 `:80` **经常失败**（AP 隔离或射频共存）。这不代表 MQTT 坏了：圆屏 MQTT 绿灯时，Broker 上仍可看到 `bituo-dial/{DialSN}/#`。

### 5.3 MQTT 北向

- Client ID = Dial SN，例如 `DIAL-0180B4`
- 前缀：`bituo-dial/{DialSN}/`
- `tls=1` 为单向 MQTTS（无客户端证书）；未给端口时默认 8883
- `tls` 实验室可用 `insecure=1`（不校验自签证书）；生产应 `insecure=0`
- keepalive 30 s；测量 `data` 与 `summary`、名单 `mdata` 均 **Retain**

| Topic | 方向 | 说明 |
|---|---|---|
| `meters/{MeterSN}/data` | 上报 | 单表，约 10 s，功率 **W**，Retain |
| `summary` | 上报 | 在线表数与每表功率，Retain |
| `mdata` | 上报 | Dial 元数据与表名单（SN / 显示名），Retain |
| `cmd` | 下发 | 与 HTTP/GATT 相同 JSON 命令 |
| `cdata` | 应答 | 统一信封 `ok/msg/event/d` |

调试可订：`bituo-dial/{DialSN}/#`。

### 5.4 Home Assistant

官方 BituoPMD 只认 EW 电表扁平 JSON，**加不上 Dial**。使用 fork：[https://github.com/bo706/BituoPMD](https://github.com/bo706/BituoPMD) **v1.1.3 或更高**。

1. HA 已安装 MQTT 集成，Broker 与 Dial **同一台**。
2. HACS → 自定义仓库 `bo706/BituoPMD`（Integration）→ 安装带 **v** 的版本号（不要选一长串 commit）→ **重启 HA**。
3. 首次配对：Dial 打开 LAN web，且 HA 所在机器要能打开 `http://<Dial-IP>/data`。HA → 添加 BituoPMD → **Use IP to pair devices** → 填 Dial 的 STA IP。  
   配对成功后日常更新走 MQTT，不再轮询网页。
4. 应出现网关设备（在线表数）+ 每块 BLE 表（电压、电流、功率、电能、RSSI、Online）。显示名来自 Dial 上的 label（如 `SDM01-1030`）。
5. Dial 上增删表后，在 HA 里 **重载该 BituoPMD 条目**（或重启 HA），新表才会建实体。

不要对 Dial 条目使用：定位灯、恢复出厂、URL-OTA（Dial 没有这些 EW 接口）。  
插件只改 HA，**没有改 Dial 固件**。

### 5.5 固件升级

**网页 OTA（推荐，不插电脑 USB）**

1. 打开 LAN web
2. 配置页上传 **`bituo-dial.bin`**（不要选 bootloader）
3. 保持供电约 30 s，新固件确认存活后才取消回滚
4. 上传失败或中途断电：仍运行旧槽

**USB 烧录**

- 只写 app 分区，**不要 `erase_flash`**（会清 NVS 里的 Wi-Fi 和表列表）
- 烧完拔掉 USB-JTAG，再当网关用

---

## 6. 北向接口摘要

三条配置通道进同一套命令分发，信封：

```json
{"ok": true, "msg": "", "event": "<cmd>", "d": {}}
```

请求：`{"cmd":"<name>", ...}`。通道：GATT 写 C313、HTTP `POST /config` 或 `POST /save-config`、MQTT `.../cmd`。

| 命令 | 作用 |
|---|---|
| `get_info` | SN、版本、IP、Wi-Fi/MQTT/HTTP、表数、uptime |
| `setwifi` / `wifi` | 写 SSID/密码并重连（STA-only） |
| `set_mqtt` / `mqtt` / `mqtr` | Broker；可选 TLS |
| `set_http` | 局域网 HTTP 开关 |
| `scan_wifi` / `scan_ble` | 扫 AP / 附近电表 |
| `add_meter` | SN + `bcast_key`，可选 label |
| `set_meter_label` | 只改显示名 |
| `del_meter` / `list_meters` / `get_meters` | 删、列表、实时量测（功率 **kW**） |
| `restart` | 延时复位 |

HTTP 还提供电表风格别名（给现有上位机少改代码）：

| 路径 | 说明 |
|---|---|
| `GET /data` | 多表量测，帕斯卡字段，功率 **W**（与 MQTT `data` 相同） |
| `GET /model` `/status` `/snapshot` `/sn` | 型号、摘要、一次打包、纯文本 SN |
| `GET /restart` | 同重启 |
| `POST /ota` | 上传 bin |
| `GET /` | 三标签管理页 |

未设置 API token 时 HTTP 不校验；若设置了，请求头 `X-Api-Token` 或查询参数 `token=`。

---

## 7. 单位对照（评审时最容易混）

| 通道 | 功率单位 | 字段风格 |
|---|---|---|
| 广播明文 / 圆屏 / `GET /api/meters` | **kW** | `total_active_power` 等数字 |
| MQTT `.../data`、`summary`、`GET /data` | **W**（内部 kW ×1000） | `TotalActivePower` 等字符串 |
| Home Assistant（BituoPMD fork 1.1.3+） | **W** | 按 MQTT / Dial 信封解析，**不再 ×1000** |

电能：HTTP `get_meters` 为分相 kWh；MQTT / `/data` 的 `TotalForwardEnergy` 为三相之和 kWh。

---

## 8. 安全与默认策略

- 局域网 HTTP **出厂关闭**，需在圆屏打开；明文 80，不做设备 HTTPS
- GET 不回显 Wi-Fi/MQTT 密码和电表 `bcast_key`
- 电表匹配不依赖 MAC，避免 RPA 导致「丢表」
- Topic / JSON 设备标识只用 Dial SN 与电表 SN
- GATT 本阶段为明文 JSON（与 Basic 通道一致），无 J-PAKE
- MQTTS 为单向 TLS；实验室 `insecure=1`，生产应校验证书
- OTA 双槽 + 约 20 s 确认，防止坏包变砖
- 量产维修禁止无故 `erase_flash`

---

## 9. 已知限制

| 限制 | 说明 |
|---|---|
| 无 PF | 广播 15 字节没有功率因数；要 PF 需另做电表 GATT 读特征 |
| 开路为 0 | 二次侧空载时电流/功率/电能为 0，与 App 一致，不是解析错误 |
| 仅 2.4 GHz | 5 GHz AP 扫不到、连不上 |
| BLE + Wi-Fi 共存 | MQTT 可能短暂变黄后自动重连；电脑/HA 访问 Dial `:80` 常失败 |
| HTTP 不能当 HA 主路径 | 插件 1.1.3+ 日常走 MQTT；首次 IP 配对仍可能要通一次 `/data` |
| USB-JTAG | 插在电脑上会复位芯片、打断 STA；烧录后拔掉 |
| 满 16 表堆较小 | 无 PSRAM；16 表时堆大约二十多 KB 量级，不要再叠大 JSON |
| 官方 PMD | 不能直接加 Dial；要用 fork，或以后向官方提 PR |

---

## 10. 建议怎么看这份产品

1. **Dial 固件**是产品本体：扫 BLE、解密、显示、MQTT（及可选 HTTP）。
2. **BituoPMD fork** 是 HA 侧适配，让能源系统读 Dial；尚未合进官方仓库。
3. 若认可 HA 路径：从 fork 向官方 `script0803/BituoPMD` 提 PR；内部用则 HACS 一直指向 fork。
4. 电表继续留在 Dial 的 BLE 下，不要为了「跟 EW 一样」去给表配 Wi-Fi。
5. 验收：圆屏三页更新 → 系统页 MQTT 绿 → Broker 上 `bituo-dial/{DialSN}/meters/+/data` 有数 → HA 各表电压/功率约 10 秒变化。不要只凭浏览器打不开 Dial 网页判失败。
