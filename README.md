# Bituo Dial

M5Stack Dial（ESP32-S3）固件：被动扫描电表 BLE 加密广播，AES-128-CCM 解密后在圆屏显示，并经 Wi-Fi MQTT 北向上报。配置命令 GATT / HTTP / MQTT 共用同一套 JSON。

固件版本 **1.1.4**。

配套 Home Assistant 插件：[bo706/BituoPMD](https://github.com/bo706/BituoPMD) **v1.1.4+**（订阅 MQTT；官方 PMD 加不上 Dial）。

## 硬件（本机实测）

- ESP32-S3 rev v0.2，**8MB Flash，无 PSRAM**
- 圆屏 GC9A01 240×240，SPI：MOSI=5，SCK=6，CS=7，DC=4，RST=8，BL=9
- 编码器 A=40，B=41，按键=42（按下为低）；蜂鸣器 GPIO3
- **GPIO46 上电必须立刻拉高**，否则约 100ms 断电
- 屏幕正立：MADCTL `0x48`（USB 为下，橙色圈三角为上）
- Wi-Fi **仅 2.4 GHz**

## 文档

| 文件 | 内容 |
| --- | --- |
| [docs/产品技术说明-Dial-BLE网关.md](docs/产品技术说明-Dial-BLE网关.md) | 完整产品规格 / 功能 / 使用 |
| [docs/产品技术说明-Dial-BLE网关.html](docs/产品技术说明-Dial-BLE网关.html) | 精简打印版 |
| [docs/测试用例-Dial-BLE网关-初版.md](docs/测试用例-Dial-BLE网关-初版.md) | 初版测试用例 |
| [docs/json_messages.md](docs/json_messages.md) | JSON 报文（命令 + MQTT 遥测） |
| [docs/api.md](docs/api.md) | HTTP REST 与命令 JSON |
| [docs/mqtt.md](docs/mqtt.md) | Topic / Payload |
| [docs/data_dict.md](docs/data_dict.md) | 广播 26B + 明文 15B |
| [docs/architecture.md](docs/architecture.md) | 系统架构与通讯流程 |

不要把电表 `broadcast_key`、PIN、Wi-Fi/MQTT 密码、NVS 转储或真机交接文档提交进本仓库。

## 快速上手

1. 烧录后系统页长按 **Setup** → Wi-Fi scan，选 2.4G SSID，输入密码保存。
2. 系统页出现 STA IP。用 GATT / 网页 / `set_mqtt` 配置 MQTT（网页要先打开 LAN web）。
3. Setup → Add meter：扫附近电表，输入 32 hex `bcast_key`。匹配不靠蓝牙 MAC。
4. 局域网网页默认关。Setup → LAN web 打开后访问 `http://<ip>/`。

Dial SN 形如 `DIAL-xxxxxx`。无 STA 时可能开 SoftAP：`BitUo-Dial-XXXXXX` / 出厂密码 `12345678` / `192.168.4.1`。热点页同样要先开 LAN web。

## 编译与烧录

需要 ESP-IDF **v5.2.3**。按本机路径设置 `IDF_PATH` / `IDF_TOOLS_PATH` 和串口。

```bat
idf.py build
idf.py -p COMx flash
```

**不要 erase-flash**，除非你要清空 NVS 里的 Wi-Fi 和表列表。烧完请拔掉电脑 USB-JTAG，再当网关使用。

## 操作

- 短按：概览 → 详情 → 系统 → 概览
- 概览旋转：多表时改选中行；详情旋转：切表
- 详情长按：回概览；系统长按：Setup（Wi-Fi / 电表 / LAN web / 热点）
- 电表 ≤15s 白，15～90s 黄，>90s 离线灰并蜂鸣

## 工具

- `tools/dial_config.py` — BLE 配置
- `tools/test_decrypt.py` / `plain15_selftest.py` — CCM 与明文自测
- `tools/monitor_mqtt.py` — 订阅 `bituo-dial/#`
- `tools/soak_poll.py` — 采样 `/api/info`

## 已知限制

- 开路时电流/功率/电能为 0（物理现象）
- 广播无 PF
- BLE + Wi-Fi 共存时，电脑访问 Dial `:80` 常失败；日常以 MQTT 为准
- MQTT 行偶发变黄是共存导致的短暂重连
