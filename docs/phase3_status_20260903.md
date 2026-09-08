# Phase 3（UI 与交互）阶段实现与验证记录

日期：2026-09-03
固件：ESP-IDF v5.2.3 + LVGL v8.3（components/lvgl 本地源码）+ NimBLE + mbedTLS
硬件：ESP32-S3 rev v0.2 / 8MB XMC Flash / **无 PSRAM** / GC9A01 240×240 圆屏 / COM7

---

## 1. 本阶段交付的代码

| 文件 | 作用 | 状态 |
| --- | --- | --- |
| `main/ui_task.h/.c` | LVGL 初始化（lv_init、1ms tick、双 DMA 帧缓冲、disp_drv 注册 gc9a01_flush、建三页）；ui_task（Core1/prio5/栈8192）主循环：编码器采集、三页调度、短按/长按状态机、离线检测、上/下线蜂鸣、lv_timer_handler | 已实现 |
| `main/ui_overview.h/.c` | 页面1 列表概览：标题 `Meters 在线/总数`，最多 8 行（名称+有效相平均电压+总功率），整行颜色表在线/警告/离线，选中行蓝底；旋钮移动选中 | 已实现 |
| `main/ui_detail.h/.c` | 页面2 单表详情：名称+序号、三相 U/I、总功率/功率因数、RSSI、updated Xs ago；旋钮上/下切表 | 已实现 |
| `main/ui_sysinfo.h/.c` | 页面3 系统状态：Wi-Fi/SSID/IP、MQTT、BLE 扫描开关、在线/总数、固件版本、剩余堆、运行时长；长按触发 SoftAP（Phase4 落地） | 已实现 |
| `main/ble_scanner.c/.h` | 新增 `ble_scanner_is_scanning()` 只读查询，供系统状态页显示扫描状态 | 已实现 |
| `main/tasks.c` | `tasks_start()` 内创建 ui_task（Core1） | 已接入 |
| `main/app_main.c` | 正式流程补 `buzzer_init()`；关闭 Phase0 四色自检（`PHASE0_SELFTEST_ENABLE=0`，避免裸驱与 LVGL 抢屏） | 已修改 |
| `sdkconfig.defaults` / `sdkconfig` | 开 `CONFIG_LV_COLOR_16_SWAP=y`、Montserrat 16/20 字体、`CONFIG_FREERTOS_HZ=1000` | 已修改 |

### 交互逻辑（单按键自洽方案）

M5Dial 仅有一个可下压的旋转编码器（BTN=GPIO42，任务书所称“侧键”即此键），故采用上下文相关的一套映射，使“短按三页循环 + 各页专属动作”同时成立：

- 概览页：短按 = 进入选中表详情；旋钮 = 移动选中行。
- 详情页：短按 = 进入系统状态页；长按 = 返回概览；旋钮 = 上/下切表。
- 系统页：短按 = 回到概览（三页构成循环）；长按 = 进入 SoftAP 配网（Phase4）。
- 按键消抖 15ms，长按阈值 800ms；短按/长按均有蜂鸣反馈。

---

## 2. 本阶段排查并修复的关键问题（重要经验）

### 2.1 FreeRTOS 节拍 100Hz 导致 ui_task 饿死 IDLE1、触发任务看门狗

- 现象：首版烧录后约第 10 秒出现 `task_wdt: IDLE1 (CPU 1)` 告警，backtrace 落在 `ui_task.c` 的 `vTaskDelay`；告警一次后自愈，未复位。
- 根因：工程 `CONFIG_FREERTOS_HZ=100`（1 tick=10ms），主循环写的是 `vTaskDelay(pdMS_TO_TICKS(5))`，`pdMS_TO_TICKS(5)=0`。**`vTaskDelay(0)` 等价于一次 yield**，而 Core1 上没有其他就绪任务，ui_task 立即继续运行，100% 占满 Core1，IDLE1 连续 10s 得不到调度而触发看门狗。
- 修复：
  1. `CONFIG_FREERTOS_HZ=1000`（1 tick=1ms，同时满足任务书“编码器 1~5ms 轮询”的精度要求）；
  2. 主循环延时加下限保护：`vTaskDelay(wait>0 ? wait : 1)`，双保险。
- 结果：重新烧录后连续抓取 18s 启动+运行日志，**无 task_wdt、无重启、无栈溢出、无 LVGL 断言**。

### 2.2 RGB565 字节序

- 开 `CONFIG_LV_COLOR_16_SWAP=y`，由 LVGL 统一做高低字节交换（ESP32 小端 vs SPI MSB-first/GC9A01 高字节在前），否则红蓝通道错位、画面发暗。

### 2.3 无 PSRAM 的帧缓冲取舍

- 任务书示例用 `MALLOC_CAP_SPIRAM`，本机无 PSRAM，改为 `MALLOC_CAP_DMA` 内部 RAM（SPI DMA 要求），失败再退 `MALLOC_CAP_INTERNAL`。双缓冲各 240×20×2≈9.6KB，共约 19.2KB，内部 RAM 充足。

---

## 3. 真机已验证项（COM7，2026-09-03 10:31 日志）

- 编译：0 error / 0 致命警告，固件 0x1010f0 字节，app 分区占用 33%。
- 启动序列正常：GPIO46 保持 → meter_store（内部 RAM 回退）→ NVS 载入 1 块表 → display_init → lvgl_init（三页 created、双缓冲 4800px/buf）→ encoder/buzzer init → ui_task 在 **Core1/prio5** 运行。
- 稳定性：18s 内无看门狗、无重启、无内存异常。
- 数据链路未受影响：`DECRYPT OK idx=0` 持续，counter 连续递增（13325→13376），三相 ctrl 相位轮换正常，RSSI -33~-40dBm。
- 默认进入概览页，数据每 200ms 从深度 1 队列 `xQueuePeek` 刷新（远优于“UI 轮询 ≤1s”的要求）。

---

## 4. 对照任务书 11.3 验收标准的状态

| 验收项 | 标准 | 当前状态 |
| --- | --- | --- |
| 数据刷新延迟 | ≤2s（UI 轮询≤1s） | **已达成**：UI 轮询 200ms，电表广播约 200~600ms |
| 离线检测 | 停广播后 5s 内灰色、30s 后离线 | 逻辑已实现（5~30s 黄、>30s 置 valid=false 并写回队列）；待“关电表广播”真机复现一次 |
| 蜂鸣器 | 上线一声(100ms)、离线两声(响100-停100-响100)，不误报 | 逻辑已实现并加首轮基线防误报；待真机复现上/下线各一次 |
| 帧率 | ≥20FPS（lv_fps_counter） | 渲染/刷新链路已通；40MHz SPI 全屏 115.2KB 理论传输约 23ms，简单 label 界面综合满帧预计 22~28FPS，**真机实测待编码器 FPC 修复后用 lv_fps_counter 读数确认**（不虚构实测值） |
| 编码器 | 旋转/按键无卡顿，响应<100ms | **已用日志闭环**：原始 A/B 翻转正常（静止 A=B=1、转动每秒数次且 A/B 计数相等），正交解码每格输出 `rotate delta=±1`、符号随转向翻转，解码→分发在同一 5ms 循环内；按键短按翻页真机可用。**单表时选中项循环恒为 0，故屏幕无可见移动属正常，多表滚动待接入第 2 块表后看画面**（硬件/FPC 经证实正常，无需拆机） |

---

## 5. 遗留项与下一步

1. **编码器（已排除硬件问题，无需拆机）**：2026-09-03 复测用原始电平诊断证实 A=40/B=41/BTN=42 全通，旋转解码、按键翻页均正常。仅“列表滚动/详情切表”的画面效果因当前只配置 1 块表而不可见，接入第 2 块表后自然可见；若需提前录演示视频，可加一个默认关闭的演示注入开关。
2. **15 字节明文数据字典（待导师）**：仅电压字段（plain[0:2] 小端 0.1V/LSB）已真机校准；详情页电流/功率/功率因数当前如实显示 0，字典到位后在 `ble_scanner.c:parse_plaintext` 补解析即可，UI 无需改结构。
3. **系统状态页联网字段**：Wi-Fi/MQTT 数值随 Phase4 填充，当前未配置时如实显示 not set / idle。
4. **UI 演示录像**：任务书 Phase3 交付物，待 FPC 修复、交互可操作后录制。
5. 一次性脚本 `tools/_patch_scanner.py` 可在收尾时清理。

---

## 6. 2026-09-03 真机复测：屏幕方向、行截断、编码器三项修复

### 6.1 屏幕方向（2026-09-03 自检定值，作废此前 0x08/0xC8「已修」结论）
- 约定：「上」= USB 口一侧，「下」= 橙色圈三角一侧。
- 四档真机（均保留 BGR=0x08）：
  - idx0 `0x08`：文字颠倒，箭头朝下（三角）
  - idx1 `0x48`（MX|BGR）：文字正，箭头朝下
  - idx2 `0x88`（MY|BGR）：文字正，箭头朝上（USB）
  - idx3 `0xC8`（MY|MX|BGR）：文字颠倒，箭头朝上
- 定值：`GC9A01_MADCTL_DEFAULT = 0x88`。旧记录把 0xC8 当正立、或在 0x08↔0xC8 间横跳，均已证伪，不要改回去。
- 日志：`tools/captures/boot_COM7_20260903_143634.log` 等多次复位自检。
- 二次烧录确认（2026-09-03 15:12）：USB 朝上时概览/详情/系统三页正立；详情 `(1/1)`；离线蜂鸣与设计对上。

### 6.2 概览行名称被截断（已修）
- 现象：`SDM01-test`（9 字符）被显示成 `SDM01-te`。
- 根因：`ui_overview.c` 行格式误用 `%-8.8s`，精度 `.8` 把名称硬截到 8 字符。
- 修复：改为 `"%s%s %.1fV %.0fW"` 自然紧凑格式，名称不再截断（圆屏行宽 220px 足够）。

### 6.3 旋转“无反应”排查（结论：硬件正常，单表故无可见位移）
- 加了两层诊断：`encoder_diag_consume()` 统计 A/B 原始电平翻转，ui_task 每秒打印；delta 非零时打印 `rotate delta=±1 page=N meters=M`。
- 静止：`idle A=1 B=1, toggles 0/0`（上拉默认高，无抖动）。
- 转动：A/B 每秒各翻转数次且计数相等；反转连续 delta=-1、正转连续 delta=+1，方向正确。
- 官方多来源（M5Stack 文档/ESPHome/ESPBoards/M5Unified）一致确认 ENC_A=40、ENC_B=41、BTN=42，引脚配置无误。
- 结论：GPIO/FPC/正交解码/分发全部正常；当前仅 1 块表，概览选中与详情切表在单项内循环恒为 0，所以画面不动，属数据规模问题，非缺陷。
- 两次复测日志：`tools/captures/boot_COM7_20260903_112126.log`（原始 toggles）、`boot_COM7_20260903_112702.log`（rotate delta 正/反向）。
