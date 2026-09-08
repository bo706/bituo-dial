# Bituo Dial

M5Stack Dial（ESP32-S3）固件：被动扫描电表 BLE 加密广播，AES-128-CCM 解密后在圆屏显示，并经 Wi-Fi MQTT/HTTP 北向上报。配置命令 GATT / HTTP / MQTT 共用同一套 JSON。

固件版本 **1.1.0**。开源定位见实习任务书 v1.1。

## 硬件（本机实测，覆盖任务书笔误）

- ESP32-S3 rev v0.2，**8MB Flash，无 PSRAM**
- 圆屏 GC9A01 240×240，SPI：MOSI=5，SCK=6，CS=7，DC=4，RST=8，BL=9
- 编码器 A=40，B=41，按键=42（按下为低）；蜂鸣器 GPIO3
- **GPIO46 上电必须立刻拉高**，否则约 100ms 断电
- 屏幕正立：MADCTL `0x88`（USB 为上，橙色圈三角为下）
- Wi-Fi **仅 2.4 GHz**

## 文档

| 文件 | 内容 |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | 系统架构与通讯流程（约 2 页） |
| [docs/json_messages.md](docs/json_messages.md) | 全部 JSON 报文（命令 + MQTT 遥测） |
| [docs/api.md](docs/api.md) | HTTP REST 与 8 条 cmd |
| [docs/mqtt.md](docs/mqtt.md) | Topic / Payload |
| [docs/data_dict.md](docs/data_dict.md) | 广播 26B + 明文 15B |
| [docs/HANDOFF_交接_20260903.md](docs/HANDOFF_交接_20260903.md) | 真机事实与编译命令 |

## 快速上手

1. 烧录（见下方）。上电后无 Wi-Fi 会开 SoftAP：`BitUo-Dial-XXXXXX` / `12345678`，浏览器 `http://192.168.4.1/`。
2. 填写 **2.4G** SSID/密码，Save Wi-Fi。系统页出现 STA IP。
3. MQTT Host 填 Broker 的局域网 IPv4、端口 1883，Save MQTT。绿字为已连接。
4. 电表用 `add_meter`（SN + `broadcast_key`，匹配不靠 MAC）。PC：`tools/dial_config.py`。

默认 Dial SN 形如 `DIAL-0180B4`。页面 `http://<ip>/` 或 `http://bituo-dial.local/`。

## 编译与烧录（Windows PowerShell）

需要 ESP-IDF **v5.2.3**。PowerShell 不支持 `&&`，用 `cmd /c`。按本机路径改 `IDF_TOOLS_PATH` / IDF / COM 口。

```bat
cmd /c "set PYTHONUTF8=1&& set PYTHONIOENCODING=utf-8&& set IDF_TOOLS_PATH=D:\Espressif&& set PATH=C:\Users\kangx\AppData\Local\Programs\Python\Python313;%PATH%&& C:\Users\kangx\esp\esp-idf\export.bat >nul 2>&1 && cd /d C:\Users\kangx\Doubao\chats\2026-09-01\new-chat-2\bituo-dial && idf.py build"
```

烧录把末尾改成 `idf.py -p COM7 flash`（口以设备管理器为准）。**不要 erase-flash**，除非你要清空 NVS 里的 Wi-Fi/表列表。

## 操作

- 短按：概览 → 详情 → 系统 → 概览
- 概览旋转：多表时改选中行；详情旋转：切表
- 详情长按：回概览；系统长按：SoftAP
- 电表 >5s 无广播黄/灰警告，>30s 离线并蜂鸣

## 工具

- `tools/dial_config.py` — BLE 配置
- `tools/test_decrypt.py` / `plain15_selftest.py` — CCM 与明文自测
- `tools/monitor_mqtt.py` — 订阅 `bituo-dial/#`
- `tools/soak_poll.py` — Phase 5 长测采样 `/api/info`

## 已知限制

- 开路时电流/功率/电能为 0（物理现象）
- 广播无 PF
- Home Assistant / BituoPMD 未做
- MQTT 行偶发变黄是 BLE/Wi-Fi 共存导致的短暂重连
