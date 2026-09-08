# Phase 1 收尾：15 字节广播明文完整解析（§9.3.1）

- 日期：2026-09-03
- 固件：bituo-dial，ESP-IDF v5.2.3 / NimBLE / mbedTLS
- 依据：《BLE 安全通道 API 文档 V2》§9.3、§9.3.1（编码版本 `ctrl.bit6-4 = 1`）
- 真机：M5Dial（ESP32-S3，8MB XMC Flash，无 PSRAM），COM7；测试表 SDM01_3PN，SN `C85024E851F1`
- 结论：**Phase 1 明文解析闭环**。电压分相真机校准通过；电流/功率/电能按文档逐字段落地，
  并以主机侧 1000 组满量程 roundtrip（含负功率）离线证明公式正确。真机当前开路无负载，
  I/P/FE/RE 明文为 0 属物理现象，非解析问题。

---

## 1. 15 字节明文布局（§9.3.1，编码版本 1，全部小端）

| 明文偏移 | 长度 | 类型 | 倍率 | 物理量 | 单位 | 代码落点 |
| --- | --- | --- | --- | --- | --- | --- |
| 0–1 | 2 | uint16 无符号 | ÷10 | 电压 V | V | `voltage[phase]` |
| 2–3 | 2 | uint16 无符号 | ÷100 | 电流 I | A | `current[phase]` |
| 4–6 | 3 | **int24 有符号** | ÷1000 | 有功功率 P | **kW** | `active_power[phase]` |
| 7–10 | 4 | uint32 无符号 | ÷100 | 正向有功电能 FE | kWh | `forward_energy[phase]` |
| 11–14 | 4 | uint32 无符号 | ÷100 | 反向有功电能 RE | kWh | `reverse_energy[phase]` |

要点：

1. **int24 有符号**：`b0 | b1<<8 | b2<<16` 拼成 24 位，若 `b2.bit7=1` 则减 `0x1000000`
   做符号扩展。功率可正可负（发电/反向送电）；若误按无符号解析，负功率会变成约 16777kW。
2. **相位路由**：每帧只带一相，相位/回路索引在控制字节 `ctrl.bit3-2`
   （三相：0=A/X，1=B/Y，2=C/Z；单相固定 0）。沿用"Peek 上一帧合并快照、只刷新本相"，
   不整帧清零；三相总有功 `total_active_power = Σ active_power[0..2]`，单位 kW。
3. **功率因数 PF 不在广播内**：§9.3.1 的 15 字节没有 PF 字段，
   `power_factor[] / overall_power_factor` 不从广播产生（恒 0），UI 固定显示 `PF --`，
   不臆造；PF 需走 GATT 读特征，属后续阶段。
4. 无效/非有限数编码为 0；SRS01A/B 不广播 RE（恒 0）。

## 2. 控制字节 ctrl（§9.3）拆解

`ctrl = bit7 GATT已装 | bit6-4 编码版本 | bit3-2 相位索引 | bit1-0 产品类别`

- 真机样例 `0x93 / 0x97 / 0x9b`：bit7=1（GATT 已装）、版本=1、类别=3（三相 SDM01_3PN）、
  相位分别为 0/1/2，按帧轮换，与文档一致。
- 新增编码版本防御：版本 ≠ 1 时限频告警一次（仍按 v1 解析），避免未来新布局被静默错解。

## 3. 代码改动清单

| 文件 | 改动 |
| --- | --- |
| `main/ble_scanner.c` | `parse_plaintext` 由"仅电压"补全为 V/I/int24-P/FE/RE 全字段 + 三相总功率；ctrl 拆解出 ver/phase/cat/gatt；版本≠1 告警；新增 `PARSE phN: ...` 对拍日志；更新文件头注释 |
| `main/meter_store.h` | 字段单位注释统一：`active_power/total_active_power` 单位 **kW**，电流 A，电能 kWh；标注 PF 广播不含、恒 0 |
| `main/ui_detail.c` | 详情页电流分辨率 `%.1fA→%.2fA`；功率行改 `P %.2fkW  PF --`（不再显示恒 0 的总功率因数）；占位文本同步 |
| `main/ui_overview.c` | 概览行总功率 `%.0fW → %.2fkW` |
| `main/ble_gatt_server.c` | `get_meters` JSON 在 voltage_x/y/z 基础上补 current/active_power/forward_energy/reverse_energy 的 x/y/z 分相字段（响应缓冲 4096B，充足） |
| `tools/plain15_selftest.py` | 新增：15B 编解码主机侧自测，解码逻辑与 C 逐行同构 |

## 4. 真机验证（日志 tools/captures/boot_COM7_20260903_115825.log）

- 编译 0 错误（16 目标全过），bin `0x101790`；烧录 COM7 成功；运行 12s 无 task_wdt、
  无重启、无 LVGL 断言，三页正常创建，编码器 diag 正常。
- 解密连续命中（counter 715→748），ctrl 拆解正确，三相轮换：

```
DECRYPT OK ... ctrl=0x93(ver=1 phase=0 cat=3 gatt=1) counter=715 rssi=-28
decrypted plain15: 9e 08 00 00 00 00 00 00 00 00 00 00 00 00 00
PARSE ph0: V=220.6 I=0.00 P=0.000kW FE=0.00 RE=0.00kWh | totalP=0.000kW
... ctrl=0x9b phase=2 ... 9c 08 ... PARSE ph2: V=220.4 ...
... ctrl=0x97 phase=1 ... 9d 08 ... PARSE ph1: V=220.5 ...
```

- 电压核对：明文 `9e 08` 小端 = 0x089E = 2206 → 220.6V；三相均在 219.6–220.6V，
  与手机 APP 同表 220V 级读数一致。
- I/P/FE/RE：明文第 2–14 字节全 0（测试表当前开路、未带负载），解析输出 0，
  与手机 APP"电流 0"一致。**需在表二次侧带实际负载时才能看到非零电流/功率/电能**，
  届时直接对照 `PARSE` 日志即可，解析代码无需再改。

## 5. 离线自测（tools/plain15_selftest.py，Python313，仅标准库）

真机开路无法制造非零 I/P，故用与 C 同构的公式做离线验证，结果全部 PASS：

```
[1] doc example FC08 -> V=230.0 (expect 230.0)  PASS
[2] real capture 9e08 -> V=220.6 (expect 220.6)  PASS
[3] V=230.0 I=5.01 P=-1.234kW FE=1234.56 RE=0.02 PASS；int24(-1234)=2e fb ff PASS
[4] 1000 random roundtrips (full range, signed P): PASS
[5] boundary P max 8388.607kW / P min -8388.608kW: PASS
ALL PASSED
```

覆盖：文档原例、真机抓包、非零全字段、**负功率 int24 补码**、满量程 1000 组随机 roundtrip、
功率正负饱和边界。

## 6. Phase 1 验收对照（任务书）

| 验收项 | 状态 | 证据 |
| --- | --- | --- |
| 被动扫描收到 26B 长包并按 key 盲解命中（不绑 RPA 地址） | 通过 | DECRYPT OK 连续，counter 递增 |
| Nonce = 8×00 ‖ BE32(counter)，手工大小端、禁 memcpy | 通过 | CCM Tag 持续通过 |
| 15B 明文按数据字典解析分相 V/I/P/FE/RE | 通过 | §9.3.1 全字段落地 + 离线 roundtrip |
| 分相轮换、快照合并、三相总功率 | 通过 | phase 0/1/2 轮换，totalP 求和 |
| 串口打印解密/解析结果 | 通过 | DECRYPT OK / plain15 / PARSE 三类日志 |
| 不臆造字典外字段 | 通过 | PF 明确显示 `--`、代码注释标注 |

## 7. 遗留与下一步

1. **带负载复测（需现场，非代码问题）**：给测试表二次侧接已知负载，用 `PARSE` 日志核对
   非零 I/P/FE/RE；负功率需反向送电场景，离线已验证 int24 符号扩展。
2. PF 若业务需要，走 GATT 读特征通道获取，不在广播解析范围内。
3. Phase 1 数据面已完整，可继续 Phase 4（SoftAP 配网 / MQTT 上报）；
   `get_meters` 已具备全字段 JSON，供上报与 PC 端直接使用。
4. 屏幕显示方向（GC9A01 MADCTL）为独立显示问题，与本次数据解析无关，另行一次性自检定死。
