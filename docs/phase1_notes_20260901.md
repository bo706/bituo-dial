# Phase 1 开发记录（2026-09-01）

## 1. 外部资料消化结论

| 资料 | 性质 | 对项目的作用 |
|---|---|---|
| BLE_Crypto_API_Documentation.md | 安全通道 API V2（V2.0 兼容 V1.x） | 回答任务书 12.3「broadcast_key 获取方法」：PIN(8位数字)+SN → PBKDF2-HMAC-SHA256(10000 轮，salt=SN‖ver‖"PBKDF2") → EC J-PAKE(secp256r1,SHA-256) → HKDF 得 LTK → 再 HKDF(info="...broadcast") 得 broadcast_key(16B) |
| ble_secure_client.py | 厂商金标准参考客户端（51KB，全协议实现） | 已归档 `tools/reference/`，作为 PC 对拍与后续 Dial 侧 GATT 客户端实现的唯一基准 |
| BLE_Business_Commands_Protocol_ez.md | 业务命令协议（0xC313/0xC315，JSON+CCM 信封） | Phase 2 配置通道/公司固件交互参考；Dial 自身 GATT 仍按任务书走明文 |
| BLE_API_Documentation_ez.md | 设备对外 Wi-Fi/LoRa/MQTT/Modbus 接口 | Phase 4 北向对接参考 |
| C6_V1.bin / C6_V3.1.1.bin / SXM_PRO_G1_V3.1.1_P1.bin | **电表侧固件**（约 1.66MB），不是 Dial 固件 | 不可烧录 M5Dial（Dial 为 ESP32-S3）；仅在需要给电表升级/反编译仲裁时使用 |
| BT PowerLink APP（第 4 部分） | 官方配网 App | **Phase 1 不需要**：ble_secure_client.py 已等价覆盖；apk 反编译仅作后手仲裁 |

## 2. 仍缺的外部输入（阻塞项）

1. **《BLE 广播数据字典 v2.x》正式版**：API 文档 §9.3 只定义 26B 包结构，15B 明文内部字段明确写「以厂商数据字典为准」；任务书 5.2.3 三相布局仅为示意。`parse_plaintext()` 在拿到字典前保持 TODO。
2. **目标电表的 SN（12hex）+ 机身 8 位 PIN**（走完整 J-PAKE 握手派生 broadcast_key），或导师直接给 broadcast_key（32 hex）。
3. 现场设备 `45:6A:75:3B:B4:C4` 的归属与型号确认（随机静态地址，相位槽 0/1/2 轮换，产品类别=3）。

## 3. 本轮完成与验证证据

- crypto.c：mbedtls_ccm_auth_decrypt 封装 + `crypto_selftest()` 固定向量（正样本匹配 + 篡改 tag 负样本拒绝），开机日志 `crypto: selftest OK`。
- tools/test_decrypt.py：同源于固件向量的 PC 自测，5 项全 PASS（含「小端 nonce 必失败」反向验证坑 2）；支持单帧 26B 解密与短包鉴别器校验。
- ble_scanner.c：NimBLE 被动扫描（itvl=0x50/window=0x28/passive=1/filter_dup=0）；
  5B 短包（state+12bit 鉴别器）与 26B 长包解析；未配置电表时 observe 模式限频打印原始帧；
  Nonce 手工 BE32 拼装（严禁 memcpy）；解密成功打印 15B 明文 hex。
- tools/ble_scan_dump.py：PC bleak 空口抓包（passive，兼容 company id 剥除）。
- 真机证据（tools/captures/boot_COM7_20260901_181110.log）：
  selftest OK → passive scan started → 收到 45:6a:75:3b:b4:c4 完整 26B，
  counter 连续递增；PC bleak 同抓对拍一致。

## 4. 排障记录：「24B 截断」伪问题

现象：passive 模式下 observe 打印的厂商数据只有 24B，PC 端却是 26B。
排查：临时切 active 并打印 controller 原始 AD（total=31 = Flags3 + 长度1 + type/data27），
证明空口数据完整；根因是**日志行缓冲区开小**（每字节 "xx " 占 3 字符，
前缀+26×3 超出缓冲被 snprintf 截断 2 字节），与协议栈、active/passive 均无关。
教训：任何 hex dump 缓冲按 `3×N + 前缀 + 末尾零` 宁大勿小。已恢复 passive=1。

## 5. 下一步顺序

1. 拿到 SN+PIN 或 broadcast_key → PC 端先解密样本帧（test_decrypt.py / ble_secure_client.py）。
2. 拿到数据字典 v2.x → 填 parse_plaintext()，设备端串口打印电压电流。
3. Phase 0 人工验收（屏幕四色/蜂鸣器/旋钮）仍待执行。
4. 之后进入 Phase 2：Wi-Fi/SoftAP、Dial 明文 GATT 配置通道、add_meter 录入 key。
