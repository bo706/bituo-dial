# Phase 2（配置管理）验收记录

- 日期：2026-09-02
- 设备：M5Dial（ESP32-S3 rev v0.2，8MB XMC Flash，无 PSRAM），串口 COM7
- BLE 地址：AC:A7:04:01:80:B6（public），设备名 BitUo-Dial，dial_sn=DIAL-0180B4
- 固件：ESP-IDF v5.2.3 + NimBLE + mbedTLS，fw_ver=1.1.0
- 自动化脚本：`tools/phase2_selftest.py`（单连接内顺序跑完，结束自动清理测试表）
- 真机验证脚本：`tools/verify_meter_key.py`（用指定 broadcast_key 空口盲解真实电表长包）

## 一、验收结果总览

- 配置命令自动化自检：**PASS=17 / FAIL=0**；手动 restart 掉电恢复通过。
- **真实电表在线解密（Phase2 最后一个真机项）：已闭环通过**（详见第五章）。

| 分组 | 用例 | 结果 |
|---|---|---|
| get_info | ok / dial_sn / fw_ver / ble_scan.running | PASS×4 |
| add_meter | 合法添加返回 index | PASS |
| add_meter | 重复 SN 覆盖（幂等） | PASS |
| 参数错误 | 非法 sn → `invalid sn (expect 12 hex)` | PASS |
| 参数错误 | 非法 key → `invalid key (expect 32 hex)` | PASS |
| 参数错误 | 非法 mac → `invalid mac (expect AA:BB:CC:DD:EE:FF)` | PASS |
| del_meter | 删除不存在 SN → `sn not found` | PASS |
| 边界 | 填满 16 槽后第 17 次 → `meter list full (max 16)` | PASS |
| setwifi/set_mqtt | 写入 ok，get_info 回读一致 | PASS×4 |
| get_meters | 16 块表长 JSON（约 2KB）多分片完整重组 | PASS |
| 清理 | 删除全部测试表后数量恢复初始值 | PASS |
| restart | 重启后 uptime 归零，Wi-Fi/MQTT 配置 NVS 保留 | PASS（手动回归） |
| 广播恢复 | 连接断开后 1~2s 自动恢复广播，可再次连接 | PASS（前序已验） |
| **真机在线解密** | 真实 SDM01 表 CCM 盲解命中、三相电压解析、get_meters 上报、restart 回归 | **PASS** |

## 二、本轮发现并修复的问题

### 1. add_meter 错误信息不区分字段（任务书 7.6）
- 现象：sn/key/mac 任一格式错误统一返回 `invalid key or mac or sn`，无法定位。
- 修复：`meter_store_add` 返回码细分——`-4 invalid sn`、`-5 invalid key`、`-6 invalid mac`
  （`-1 表满`、`-3 NVS 失败` 不变）；GATT 层据此返回精确提示。

### 2. 长 JSON 响应被截断导致命令超时（任务书坑 8 的延伸，关键）
- 现象：表数量多时 list_meters/get_meters 在约 500 字节处被截断，客户端永远拼不出
  完整 JSON，表现为等待响应超时。
- 根因有两层：
  1. 响应缓冲 `resp[512]` / `s_last_resp[512]` 容量不足，`snprintf` 截断成非法 JSON；
  2. 单条 Notify 有效载荷上限 = 协商 ATT_MTU − 3（MTU=512 时仅 509 字节），16 块表
     响应约 2KB，必须分片。
- 修复：
  - 新增 `BITUO_RESP_CAP=4096`，响应序列化缓冲改为**静态全局**（不得放 NimBLE host
    任务栈，否则数 KB 局部数组撑爆 host 栈）；
  - GATT 回调按 `ble_att_mtu(conn)-3` 切片，连续 `ble_gatts_notify_custom` 发送；
  - **片间禁止 `vTaskDelay`**：回调运行在 NimBLE host 任务，分片续发依赖该任务处理
    controller 事件，自我阻塞会整包卡死；连续提交后发不出的分片由 L2CAP tx_q 自动
    排队、在后续连接事件续发；
  - PC 端 `dial_config.py` / `phase2_selftest.py` 的 Notify 回调改为累积缓冲，直到
    `json.loads` 成功才作为一条完整响应，发新命令前清空残留。

### 3. 电表使用 RPA 随机私有地址，按固定 MAC 匹配必然失败（关键）
- 现象：同一台电表在不同阶段空口地址不同——手机连接时 `7E:1E:1E:F8:F7:F3`、
  待配网时 `47:0E:86:55:91:1E`、重新配网后 `7E:1E:1E:FA:42:1F`。
- 根因：该表 BLE 地址为可解析随机地址（RPA），每次配网/重启后 3 字节后缀变化，
  固件原逻辑 `meter_store_find_by_mac(addr)` 按源地址定槽位，永远匹配不上。
- 修复（依据导师《BLE_Crypto_API_Documentation》§9.3：持有 broadcast_key 即可无连接
  解密、与地址无关）：扫描回调改为**盲解匹配**——Nonce 只依赖 counter 构造一次，随后
  遍历所有 enabled 配置槽，用各自 broadcast_key 逐一做 AES-128-CCM 解密，**Tag 校验
  通过即命中**，全部失败才丢弃。最多 16 次 CCM 在 ESP32-S3 仅几十微秒；扫描回调与
  配置增删回调同属 NimBLE host 单任务，无需额外加锁。`meter_store_find_by_mac`
  作为导出工具函数保留（MAC 仍配置/存储，可供显示），但不再用于匹配。

### 4. 三相按帧轮换广播，整帧清零会导致另两相电压跳零
- 现象：`parse_plaintext` 落地后，get_meters 三相中总有两相在 0 与实测值间反复跳变。
- 根因：三相表每帧只携带**一个相位**的量测（相位由控制字节 `ctrl.bit2-3` 指定，
  0=X/A、1=Y/B、2=Z/C，循环轮换）；原代码每帧 `memset` 清零再只填当前相，深度 1
  队列里永远只剩“当前相”。
- 修复：命中后先 `xQueuePeek` 取出上一次合并快照作基底，`parse_plaintext` 只刷新
  `voltage[phase]`，其余两相沿用历史值，再 `xQueueOverwrite`。

## 三、当前 NVS 状态（2026-09-02 真机闭环后）

- 已执行一次 `erase-flash`，原 selftest 假 Wi-Fi/MQTT（SelftestAP / mqtt.selftest.io）已清除。
- 电表列表：**仅 1 块真实电表**（NVS 持久化，restart 验证保留）：
  - sn = `C85024E851F1`（12 位十六进制，= 设备名 SDM01_3PN-C85024E851F1 后缀）
  - label = `SDM01-test`；mac 字段记录配网当时地址（盲解不依赖它，RPA 变了也不影响）
  - broadcast_key = `<redacted-broadcast-key>`（重新配网后派生，见第五章）
- Wi-Fi/MQTT：空，Phase4 配真实网络时写入。

## 四、字段解析校准状态（parse_plaintext）

- **已校准（真机实测 + 手机 APP 对拍）**：分相电压 = 明文 `plain[0:2]` 小端、0.1V/LSB，
  按 `ctrl.bit2-3` 写入 `voltage[0..2]`。实测 A/C 相 221~224.6V 合理波动、B 相未加压
  读数 0.1V，与手机 BT PowerLink APP 显示的 X 相 222.0V 同量级吻合。
- **未校准（保持 0，严禁臆造）**：电流、分相/总有功功率、功率因数、正反向电能的偏移、
  字节序、有符号性与缩放，仍待导师《BLE 广播数据字典 v2.x》到位后逐项补全。

## 五、真实电表在线解密闭环过程（关键经验沉淀）

### 5.1 电表安全阶段 step 与广播形态
- step=0（等待 J-PAKE 首轮，即未配网/绑定被清）：只广播 **5 字节短包**
  `FF FF <state> <discLo> <discHi>`，其中 state=0x00 表待配网，disc 小端 12bit =
  SN(12hex) 最高 12bit（即前 3 个 hex 位）。
- step≠0（已配网）：广播 **26 字节加密长包**（FF FF + ctrl + counter[4,小端] +
  15B CCM 密文 + 4B Tag），约 200ms~秒级周期，三相相位槽轮换。
- **教训**：在手机系统蓝牙里“取消配对/忽略此设备”会清掉电表 LTK、把它打回 step=0
  只发短包；正常断开只需退出 APP / 关手机蓝牙，**不要点忽略**。

### 5.2 broadcast_key 会随重新配网变化
- 取消配对前手机读到旧 key `<redacted-broadcast-key>`；重新走 J-PAKE 配网
  （PIN=`<redacted>`）后 APP 显示新 key `<redacted-broadcast-key>`。
- 结论：broadcast_key 由 device_nonce||client_nonce+LTK 经 HKDF 派生，**重新配网会
  产生新 nonce、可能更换 key**，必须以配网后 APP 实际显示为准，不能沿用历史值。

### 5.3 空口抓包的平台差异：Company ID 被后端剥除
- Windows bleak 拿到的厂商数据常把 `FF FF` 公司标识剥掉，26B 长包只剩 24B。
- PC 脚本（verify_meter_key / ble_scan_dump）需在 24B 前补回 `FF FF`；固件侧
  NimBLE 已内置 `mfg_len==24` 的防御分支，两端都要兼容。

### 5.4 端到端验证结果
1. PC 端 verify_meter_key 用新 key 空口盲解：连续 10 帧 Tag 全通过，A/C 相 224V 量级；
2. add-meter 写入 Dial 后，串口 `DECRYPT OK idx=0 ctrl=..(phase/cat) counter=.. rssi=..`，
   明文与 PC 端逐字节一致；
3. get_meters（BLE 上报）：valid=true，voltage_x/y/z = 221.6 / 0.1 / 221.9V，counter 递增；
4. 远程 restart 后 NVS 配置保留，20 秒内自动恢复解密（X=222.2 / Z=221.9V），掉电回归通过；
5. RSSI 在 -26~-79 间波动均能稳定解密，盲解鲁棒性满足要求。

## 六、遗留项

1. 15 字节明文《BLE 广播数据字典 v2.x》仍待导师提供：电压字段已实测落地，其余量测
   字段（电流/功率/功率因数/电能）在拿到字典前保持 0，parse_plaintext 已留显式注释。
2. HTTP `/api/info` 通道待 Phase4 Wi-Fi/SoftAP 落地后从 PC 实测。
3. `PHASE1_TEST_SLOT` 已关闭（=0）并 erase-flash 清掉被误持久化的测试槽；
   `PHASE0_SELFTEST_ENABLE` 暂保留为 1（开机四色+蜂鸣自检，曾用于定位黑屏，Phase3
   正式开机 UI 落地时再关）。
4. 旋钮/按键 FPC 重插（Phase3 UI 前修好即可，可延后）。
5. 一次性补丁脚本 `tools/_patch_scanner.py` 可清理。
