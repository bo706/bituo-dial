# BLE 广播数据字典（Dial 实现）

依据：《BLE 安全通道 API 文档 V2》§9.3 / §9.3.1。编码版本 `ctrl.bit6-4 = 1`。  
真机校准与离线 roundtrip 见 `docs/phase1_plaintext_closeout_20260903.md`。

电表 BLE 地址是 RPA，会变。**匹配只靠 `broadcast_key` 盲解（CCM Tag 过即命中），禁止按 MAC。**

## 1. 空中 26 字节长包

| 偏移 | 长度 | 内容 |
| --- | --- | --- |
| 0–1 | 2 | Company ID `FF FF`（Windows bleak 常剥掉，固件对 24B 有补回） |
| 2 | 1 | `ctrl` |
| 3–6 | 4 | `counter` 小端 uint32 |
| 7–21 | 15 | AES-128-CCM 密文 |
| 22–25 | 4 | CCM Tag |

未配网短包 5 字节：`FF FF <state> <discLo> <discHi>`，Dial 不解密。

### ctrl

`bit7 GATT已装 | bit6-4 编码版本 | bit3-2 相位 | bit1-0 产品类别`

- 版本当前为 1；≠1 时固件告警仍按 v1 解析。
- 三相相位：0=X/A，1=Y/B，2=C/Z，**每帧只带一相、按帧轮换**。
- 类别 3 = 三相（如 SDM01_3PN）。

### CCM

- Key：`broadcast_key` 16 字节（配网后从 APP 导出，重新配网会变）。
- Nonce 12 字节：`8 × 0x00 ‖ BE32(counter)`。空中 counter 是小端，填 nonce **必须大端**，禁止 `memcpy`/`htonl`。
- 无 AAD，Tag 4 字节。

## 2. 15 字节明文（全部小端）

| 明文偏移 | 长度 | 类型 | 倍率 | 物理量 | 单位 |
| --- | --- | --- | --- | --- | --- |
| 0–1 | 2 | uint16 | ÷10 | 电压 | V |
| 2–3 | 2 | uint16 | ÷100 | 电流 | A |
| 4–6 | 3 | **int24 有符号** | ÷1000 | 有功功率 | **kW**（可负） |
| 7–10 | 4 | uint32 | ÷100 | 正向有功电能 | kWh |
| 11–14 | 4 | uint32 | ÷100 | 反向有功电能 | kWh |

int24：`b0 | b1<<8 | b2<<16`，若 `b2.bit7=1` 则减 `0x1000000`。

三相总有功 = 三相 `active_power` 之和（kW）。合并策略：Peek 上一帧快照，只刷新本相，禁止整帧清零。

## 3. 广播里没有的字段

**功率因数 PF 不在 §9.3.1。** 固件不从广播编造；UI 显示 `PF --`；JSON 里 `overall_power_factor` 恒 0 表示未测，不是实测零。PF 需电表 GATT 读特征（未做）。

开路时明文后 13 字节为 0，I/P/E 显示 0 与 APP 一致。
