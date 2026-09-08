# Phase 1 真机解密闭环记录（2026-09-02）

## 结论
真实测试表广播 AES-128-CCM 解密链路在 PC 端与 Dial 固件端**双端独立打通，明文逐字节一致**。

## 设备身份（已纠正早期误判）
- 用户测试表 BLE 地址：**7E:1E:1E:F8:F7:F3**（随机静态地址，手机配网后保持）
- 短包鉴别器 0x50c（=SN 最高 12bit，完整 12hex SN 未知，仅知前 3 位 50c）
- 早前看到的 7A:8B:29:AE:8A:AB（SDM01_3PN-C85024E851F1）是环境里**别人的表**，勿混
- 手机 App 配网后设备 step: 0→2，广播由 5B 短包切换为 26B 长包（符合 §9.1）

## 密钥
- broadcast_key = `<redacted-broadcast-key>`（手机 App 界面读出，16B）
- 仅 broadcast_key 即可无连接解密全部长广播；SN/PIN/LTK 为 GATT 加密业务通道所需，Phase1 不需要

## 双端验证证据
1. PC（bleak + cryptography）：tools/decrypt_live.py，20s 22 帧全部 tag 通过，phase 0/1/2 覆盖；
   样本：tools/samples/real_meter_7e1e1ef8f7f3_20260902.txt
2. Dial（NimBLE + mbedTLS）：meter_store_phase1_inject() 注入测试槽（编译期，Phase2 配置通道替换），
   开机即持续输出 `DECRYPT OK 7e:1e:1e:f8:f7:f3 ... decrypted plain15`；
   日志：tools/captures/boot_COM7_20260901_193642.log
3. 关键实现点：NimBLE addr.val 为小端序，测试槽 MAC 存储时整体反转（见 meter_store.c 注释）

## 明文观察（未经数据字典确认，禁止硬编）
- 每相位 15B，前 2B 为小端 u16，其余当前全 0
- phase0/2：u16 在 0x0892~0x089e（2194~2206），若 0.01V/LSB ≈ 219.4~220.6V，与市电 220V 吻合
- phase1：恒 0x0001（疑似 B 相未接的状态/零值）
- parse_plaintext() 保持 TODO，等《BLE 广播数据字典 v2.x》

## 待办
- [ ] Phase0 人工感官验收（屏幕四色/蜂鸣/旋钮，日志侧已全部正常）
- [ ] 数据字典到手后实现 parse_plaintext()，关闭 Phase1
- [ ] Phase2 启动时：删除 PHASE1_TEST_SLOT 测试槽与 app_main 中的注入调用
