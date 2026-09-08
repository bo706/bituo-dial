# Phase 5 状态（到 2026-09-04）

对照任务书 v1.1 §11.5。本轮能做的已做完；缺现场条件的不硬做。

| 项目 | 结果 |
| --- | --- |
| 文档 README / api.md / mqtt.md / data_dict.md | **PASS** |
| 掉电恢复（断电再上电，配置与电表恢复） | **PASS**（约 94s 采样时已 Wi-Fi/MQTT/`online_count:1`） |
| 连续运行 24h | **未完成**（仅 2.7h 短时 PASS，见 `phase5_soak_20260904.md`） |
| Wi-Fi 断线恢复 | **未测**（路由器不能只关 2.4G；关整机会断电脑与 Broker） |
| GitHub Release v1.1.0 | **未做**（仅本地 `git init`，无公司远程） |
| 代码 `-Wall` / Review / PR | **未做**（无 GitHub 流程） |

加分与现场挂起（不挡演示）：负载 I/P/E、第二块表、PF、HA、UI 录像。见 `pending_checklist.md`。

以后补 24h：`python tools/soak_poll.py`，电脑勿休眠。以后补断网：可接受电脑掉线时关整台路由 15s 再开。
