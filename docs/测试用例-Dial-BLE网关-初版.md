# Bituo Dial（BLE 电表网关）测试用例 · 初版

| 项 | 内容 |
|---|---|
| 依据 | [产品技术说明-Dial-BLE网关.md](产品技术说明-Dial-BLE网关.md) |
| 被测对象 | Dial 固件 **1.1.4**；HA 配套 [bo706/BituoPMD](https://github.com/bo706/BituoPMD) **1.1.4** |
| 文档性质 | 初版功能 / 异常 / 边界 / 安全 / 稳定性测试用例 |
| 日期 | 2026-09-17 |

电脑须与 Dial 同一 2.4 GHz 局域网。HTTP 相关用例用 `curl.exe`（不要用 PowerShell 的 `curl` 别名）。密钥、Wi-Fi/MQTT 密码不要写进本表。

---

## 0. 测试约定

| 优先级 | 含义 |
|---|---|
| P0 | 主路径，不过不能交付 |
| P1 | 完整功能，本轮应测 |
| P2 | 边界 / 长测 / 破坏性，有条件再测 |

结果填：`通过` / `失败` / `阻塞` / `未测`。失败记现象、时间、圆屏页、IP。

**铁律**

1. **不要** `erase_flash`（会清 Wi-Fi 和表列表）。
2. **不要**把 BLE 表改配到电表自己的 Wi-Fi（会离开 Dial）。
3. HA 里 **不要**对 Dial 条目点定位灯、恢复出厂、URL-OTA。
4. OTA 只上传 `bituo-dial.bin`，不要选 bootloader。
5. 开路时 I/P/E 为 **0** 算通过（与 App 一致）。
6. 详情页 `PF --` 算通过；MQTT `data` 无 PF 字段算通过。
7. MQTT 系统页偶发黄 `(wait)` 后能自行变绿，不算失败。

---

## 1. 功能

| ID | 测试类型 | 测试场景 | 测试步骤 | 测试期望 | 优先级 | 结果 |
|---|---|---|---|---|---|---|
| TC-BOOT-01 | 功能 | 冷启动供电保持 | 1. USB 供电<br>2. 观察圆屏约 1s 内是否亮起并停留<br>3. 短按到系统页看固件 | 持续供电，不闪一下即灭；固件 `v1.1.0`；SN 形如 `DIAL-xxxxxx` | P0 | |
| TC-BOOT-02 | 功能 | 圆屏正立 | 1. 橙色圈三角朝上、USB 朝下握持<br>2. 短按看完三页文字 | 三页文字正立、不颠倒 | P1 | |
| TC-UI-01 | 功能 | 短按翻页 | 1. 从概览起连续短按三次 | 概览 → 详情 → 系统 → 概览 | P0 | |
| TC-UI-02 | 功能 | 多表旋钮 | 1. 确认至少 2 块已加表<br>2. 概览旋转，看蓝条是否换行<br>3. 进详情再旋转 | 选中行与表对应；详情数据换表，不串数据 | P0 | |
| TC-UI-03 | 功能 | Setup 进出 | 1. 到系统页，中心键按住超过约 0.8s<br>2. 旋转查看菜单<br>3. 再长按退出 | 进入 Setup（Wi-Fi scan / Add meter / Delete meter / LAN web / Hotspot / Back）；LAN web 显示 `[ON]` 或 `[OFF]`；能退回系统页 | P0 | |
| TC-WIFI-01 | 功能 | 圆屏连 2.4G | 1. Setup → Wi-Fi scan<br>2. 选 2.4G SSID，输入密码保存<br>3. 看系统页 | 出现 STA IP；Wi-Fi 为已连接。系统页出现 STA IP | P0 | |
| TC-WIFI-03 | 功能 | 打开局域网网页 | 1. Setup 打开 LAN web 为 `[ON]`<br>2. 浏览器打开 `http://<STA-IP>/` | 出现三标签页：状态数据 / 添加电表 / 配置 | P0 | |
| TC-HTTP-06 | 功能 | 网页三标签可用 | 1. 打开 `http://<STA-IP>/`<br>2. 点三个标签<br>3. 「添加电表」点扫描附近电表 | 状态页有各表卡片；添加页能列出附近 SPM/SDM；配置页有 Wi-Fi、MQTT、OTA、关 HTTP、重启。本条不点重启/OTA | P1 | |
| TC-METER-01 | 功能 | 解密后圆屏显示 | 1. 概览/详情查看已加表<br>2. 与手机 App 同表对电压<br>3. 有负载时再对电流、功率 | 电压约 220V，相对 App 误差约 &lt;1%；三相表三相电压合理（不是只有一相长期为 0） | P0 | |
| TC-METER-04 | 功能 | 改回路名 | 1. 网页配置页改某表 label（如厨房）<br>2. 看圆屏概览 | 显示新名称；SN 不变；该表仍在线 | P1 | |
| TC-METER-06 | 功能 | 开路读数 | 1. 表二次侧空载<br>2. 看详情 I/P/E 与电压 | I/P/E 为 0，电压仍约 220V；与 App「电流 0」一致 | P0 | |
| TC-HTTP-02 | 功能 | `GET /data` | 1. LAN web 为 ON<br>2. `curl.exe http://<IP>/data` | `ok:true`，`event:data`；`d.dial_sn` 正确；`d.meters` 为数组且每块已加表一条；功率字段为 **W**（如 `"4.0"`，不是 `"0.004"`） | P0 | |
| TC-HTTP-03 | 功能 | `GET /api/meters` | 1. `curl.exe http://<IP>/api/meters` | `total_active_power` 为 **kW** 数字；同一时刻约等于 `/data` 的 W ÷ 1000 | P0 | |
| TC-HTTP-05 | 功能 | 电表同名 HTTP 路径 | 1. GET `/model`<br>2. GET `/status`<br>3. GET `/snapshot`<br>4. GET `/sn` | `/sn` 为纯文本一行，含 `bituo-dial` 与 Dial SN；其余为 JSON 信封；`/status` 含 Wi-Fi / MQTT / 在线表数 | P1 | |
| TC-MQTT-01 | 功能 | 周期遥测 | 1. 确认系统页 MQTT 曾为绿<br>2. 订阅 `bituo-dial/{DialSN}/#`<br>3. 等 20～30s | 出现 `meters/{MeterSN}/data`、`summary`、`mdata`；间隔大约 10s | P0 | |
| TC-MQTT-04 | 功能 | MQTT 远程命令 | 1. 向 `.../cmd` 发 `{"cmd":"get_info"}`<br>2. 看 `cdata` | `ok:true`，`event:get_info`，含 `fw_ver`、`ip`。本条不发 `setwifi` | P1 | |
| TC-HA-01 | 功能 | HA fork 按 IP 接入 | 1. 确认 HA 已装 bo706/BituoPMD（非仅官方仓库）<br>2. Dial LAN web 为 ON<br>3. 设备页看网关与三块表 | 网关显示在线表数；三块表实体可用；电压/功率约 10s 刷新（开路功率可为 0） | P0 | |
| TC-GATT-01 | 功能 | GATT 配置通道 | 1. 手机/PC 扫 BLE 名 `BitUo-Dial`<br>2. 连接服务 A003，向 C313 写 `{"cmd":"get_info"}` | 收到 `ok:true` 的 JSON 信封；与 HTTP `get_info` 字段一致。无工具则本条未测 | P1 | |

---

## 2. 异常

| ID | 测试类型 | 测试场景 | 测试步骤 | 测试期望 | 优先级 | 结果 |
|---|---|---|---|---|---|---|
| TC-WIFI-02 | 异常 | 仅 2.4G | 1. Setup → Wi-Fi scan<br>2. 看列表是否把 5G 当可连项 | 不能依赖 5G 上网；5G SSID 不出现或不作为必连项 | P1 | |
| TC-WIFI-05 | 异常 | Wi-Fi 密码错误 | 1. Setup 选正确 2.4G SSID<br>2. 故意填错密码保存<br>3. 观察系统页约 60s<br>4. **测完改回正确密码** | 不能稳定拿到原 STA IP；可回落 SoftAP 或保持未连接；不把错误密码显示为已成功。测完必须恢复原网 | P1 | |
| TC-HTTP-01 | 异常 | LAN web 关闭 | 1. Setup 把 LAN web 设为 `[OFF]`<br>2. `curl.exe -m 3 http://<IP>/api/info`<br>3. **测完再打开**（后续 HTTP/HA 需要） | 超时或拒绝，不返回 JSON | P0 | |
| TC-METER-02 | 异常 | 错误广播密钥 | 1. 网页或 Setup 加附近表，密钥故意填错 32 hex<br>2. 观察该槽是否出现真实量测<br>3. 删除该错误槽 | 不能稳定显示该表真实数据；删槽后不影响其它已加表 | P1 | |
| TC-METER-03 | 异常 | 删表 | 1. 仅删**测试槽**（不要误删现场三块，除非能立刻加回）<br>2. 看概览与 `GET /config/meters` | 该行消失；列表无该 SN | P1 | |
| TC-METER-05 | 异常 | 表离线 | 1. 将一块已加表远离或短暂断电（不要改表 Wi-Fi 配网）<br>2. 观察颜色：≤15s 白，15～90s 黄，>90s 灰<br>3. 恢复广播 | 变灰时蜂鸣两声；恢复后重新在线。漏扫造成的短暂发黄不算失败 | P1 | |
| TC-MQTT-05 | 异常 | BLE/Wi-Fi 共存闪黄 | 1. 观察系统页 MQTT 行 3～5 分钟 | 若出现黄 `(wait)`，应自动变回绿且遥测恢复。偶发闪黄通过；长期红/一直 wait 失败 | P1 | |
| TC-HA-02 | 异常 | 官方 PMD 加 Dial | 1. 若环境仍有官方 script0803/BituoPMD<br>2. 用「按 IP 添加」填 Dial IP | 不能按 EW 扁平 JSON 正确拆出三块 BLE 表 | P1 | |
| TC-NEG-01 | 异常 | 不提供 HTTPS | 1. 浏览器访问 `https://<IP>/` | 设备侧无 HTTPS；LAN web 打开时明文 80 可用即可 | P2 | |
| TC-HTTP-07 | 异常 | 未知命令 | 1. LAN web ON<br>2. `POST /config` body `{"cmd":"not_a_cmd"}` | `ok:false`，`msg` 含 `unknown cmd`；设备不重启、不改配置 | P1 | |

---

## 3. 边界

| ID | 测试类型 | 测试场景 | 测试步骤 | 测试期望 | 优先级 | 结果 |
|---|---|---|---|---|---|---|
| TC-METER-07 | 边界 | 重复 SN 覆盖 | 1. 对已存在 SN 再 `add_meter`，密钥相同、label 不同<br>2. 看列表槽位数与名称 | 槽位数不增加；名称更新；该表仍在线 | P1 | |
| TC-NEG-02 | 边界 | 最多 16 槽 | 1. 在已有表之外加测试槽直到第 17 块<br>2. 看返回<br>3. 删掉测试槽 | 第 17 块拒绝（如 `meter list full`）；前 16 块仍在 | P2 | |
| TC-HTTP-08 | 边界 | 非法 SN 查询 | 1. `GET /api/meters/DEAD0000BEEF` | JSON `ok:false`，`msg` 为 `sn not found`；其它表不受影响 | P1 | |

---

## 4. 安全

| ID | 测试类型 | 测试场景 | 测试步骤 | 测试期望 | 优先级 | 结果 |
|---|---|---|---|---|---|---|
| TC-HTTP-04 | 安全 | GET 不回显密钥 | 1. `GET /config`<br>2. `GET /config/meters` | 有 SSID、MQTT host/port、表 SN/label；**无** Wi-Fi 密码、MQTT 密码、`bcast_key` | P0 | |
| TC-MQTT-02 | 安全 | 标识不用 MAC | 1. 看 MQTT Topic 与 JSON | 前缀 `bituo-dial/DIAL-xxxxxx/`；表用 12 位 SN；不把蓝牙 MAC 当主键 | P0 | |
| TC-MQTT-03 | 安全 | 遥测单位与无 PF | 1. 打开一条 `.../data` | 功率为 **W** 字符串；**没有** PF 字段；有 `_source=bituo-dial` | P0 | |
| TC-UI-04 | 安全 | 界面不编造 PF | 1. 打开任一在线表详情页 | 显示 `PF --`，不是编造的 0.xx | P0 | |

---

## 5. 稳定性 / 集成 / 一致性

| ID | 测试类型 | 测试场景 | 测试步骤 | 测试期望 | 优先级 | 结果 |
|---|---|---|---|---|---|---|
| TC-WIFI-04 | 稳定性 | SoftAP 回落 | 1. 无凭据或让 STA 约 60s 连不上（不要 erase_flash）<br>2. 手机扫热点<br>3. **测完恢复原 2.4G STA** | 热点 `BitUo-Dial-XXXXXX`，出厂密码 `12345678`，IP `192.168.4.1`；热点上网页仍须先开 LAN web | P1 | |
| TC-WIFI-06 | 稳定性 | Wi-Fi 断线恢复 | 1. 记下当前 STA IP<br>2. 关闭 2.4G 路由约 15～60s 再开（接受电脑也会短暂掉线）<br>3. 看系统页 | 自动重新拿到 IP；已加表仍在；MQTT 可恢复。本条现场条件不足可未测 | P2 | |
| TC-PWR-01 | 稳定性 | 掉电恢复 | 1. 记下 SSID、IP、三块表 label<br>2. 拔电约 10s 再插上<br>3. 等重新拿到 IP | 仍连原 Wi-Fi；三块表仍在；无需重新加表 | P0 | |
| TC-YZ-01 | 一致性 | 圆屏 / HTTP / MQTT / HA 单位 | 1. 同一时刻看圆屏总功率（kW）<br>2. `GET /data` 的 `TotalActivePower`（W）<br>3. MQTT `data` 同字段<br>4. HA 实体 | `/data`、MQTT、HA 同为 W 且数量级一致；圆屏 kW ≈ W÷1000。HA 若比圆屏大约 1000 倍则失败 | P0 | |
| TC-HA-03 | 集成 | HA 不再 ×1000 | 1. 对比圆屏 kW 与 HA 功率实体 | HA ≈ `/data` 的 W；误把 Dial 当 EW 而再 ×1000 则失败 | P0 | |
| TC-OTA-01 | 集成 | 正常 OTA（可选） | 1. LAN web → 配置 → 上传 `bituo-dial.bin`<br>2. 保持供电 ≥30s | 重启后仍可运行、能扫表；失败则留在旧槽 | P2 | |
| TC-OTA-02 | 异常 | 中断 OTA（可选） | 1. 上传过程中断开浏览器或拔网（尽量不断电） | 仍运行旧固件，不变砖 | P2 | |
| TC-SOAK-01 | 稳定性 | 连续运行 24h | 1. 连续上电 24h<br>2. LAN web ON 时可每 5 分钟访问 `/api/info` | 无死机、无反复看门狗复位。此前仅约 2.7h，本条默认未测 | P2 | |

---

## 6. 记录栏

| 测试人 | 日期 | 固件 | P0 结果 | 备注 |
|---|---|---|---|---|
| | | 1.1.0 | | |
