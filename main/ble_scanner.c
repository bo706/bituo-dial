/**
 * @file    ble_scanner.c
 * @brief   NimBLE 被动扫描 + 广播过滤 + Nonce 构造 + AES-128-CCM 解密调用框架
 *          （任务书 5.2.1~5.2.5）
 * @note    扫描/过滤/Nonce/解密已打通；15B 明文已按安全通道文档 §9.3.1
 *          （编码版本 1）完整解析 V/I/P/FE/RE，分相轮换、快照合并。
 *          广播不含功率因数 PF，该字段不从本路径产生。
 * @version ESP-IDF v5.2.3 / NimBLE
 * @date    2026-09-01
 */
#include "ble_scanner.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"   /* esp_timer_get_time() */

/* NimBLE 头文件 */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "crypto.h"
#include "meter_store.h"
#include "ble_gatt_server.h"   /* Phase2：sync 后启动可连接广播 */

static const char *TAG = "ble_scan";

/* ── 扫描参数（单位 0.625ms）。Wi-Fi 已关省电，窗口加大以免漏电表广播。 */
static struct ble_gap_disc_params s_scan_params = {
    .itvl          = 0x50,   /* 扫描间隔 50ms */
    .window        = 0x40,   /* 扫描窗口 40ms，占空比 80% */
    .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
    .limited       = 0,
    .passive       = 1,      /* 被动扫描：不发 SCAN_REQ，不连接电表（已用 active
                               * 对照实验确认长度与 active/passive 无关） */
    .filter_duplicates = 0,  /* 不过滤重复：counter 每帧递增，必须全收 */
};

static uint8_t s_own_addr_type = 0;
static bool s_scanning = false;   /* Phase3：被动扫描是否已启动，供系统状态页查询 */

/* ======================================================================
 * Nonce 构造（任务书 5.2.2）——全项目最高频错误点
 * ====================================================================== */
void ble_scanner_build_nonce(uint32_t counter, uint8_t nonce[12])
{
    /* Nonce = 8 字节全 0 || BE32(counter)，共 12 字节 */
    memset(nonce, 0, 12);

    /* !!! 坑点提醒（任务书坑2）!!!
     * 广播里的 counter 是【小端】存储，而 CCM Nonce 高 4 字节要求
     * 【大端 BE32】。必须像下面这样逐字节移位手工填充：
     *   - 严禁 memcpy(nonce+8, buf+3, 4)：那会得到小端，MIC 必然失败，
     *     mbedtls_ccm_auth_decrypt 返回 -0x000D；
     *   - 严禁用 htonl 后直接 memcpy 之外想当然的写法，以下写法为唯一基准。
     */
    nonce[8]  = (uint8_t)((counter >> 24) & 0xFF);
    nonce[9]  = (uint8_t)((counter >> 16) & 0xFF);
    nonce[10] = (uint8_t)((counter >> 8)  & 0xFF);
    nonce[11] = (uint8_t)((counter)       & 0xFF);
}

bool ble_scanner_is_scanning(void)
{
    /* Phase3：系统状态页查询 BLE 被动扫描是否在跑（只读 bool，线程安全） */
    return s_scanning;
}

/* ======================================================================
 * 15B 明文解析（安全通道文档 §9.3.1，编码版本 ctrl.bit6-4 = 1）
 *
 * 定长 15 字节、全部小端，布局如下：
 *   [0:2]  uint16 无符号 /10   -> 电压 V
 *   [2:4]  uint16 无符号 /100  -> 电流 A
 *   [4:7]  int24  有符号 /1000 -> 有功功率 kW（可负，发电/反向）
 *   [7:11] uint32 无符号 /100  -> 正向有功电能 kWh
 *   [11:15]uint32 无符号 /100  -> 反向有功电能 kWh（SRS01A/B 恒 0）
 * ====================================================================== */
static void parse_plaintext(const uint8_t plain[BITUO_PLAINTEXT_LEN],
                            meter_data_t *out, uint8_t phase)
{
    /* 每帧只带一相（ctrl.bit3-2：0=A/X,1=B/Y,2=C/Z），三相按帧轮换；
     * 调用方已 Peek 上一次合并快照做基底，这里只刷新本相，另两相保留，
     * 切勿整帧清零。phase 非法（=3）时直接返回，不污染任何一相。 */
    if (phase > 2U) {
        return;
    }

    /* [0:2] 电压：uint16 小端，0.1V/LSB */
    uint16_t v_raw = (uint16_t)(plain[0] | (plain[1] << 8));
    out->voltage[phase] = (float)v_raw / 10.0f;

    /* [2:4] 电流：uint16 小端，0.01A/LSB */
    uint16_t i_raw = (uint16_t)(plain[2] | (plain[3] << 8));
    out->current[phase] = (float)i_raw / 100.0f;

    /* [4:7] 有功功率：int24【有符号】小端，1W/LSB，字段单位 kW（÷1000）。
     * !!! 坑点提醒：必须做符号扩展，b2.bit7=1 表示负功率（反向送电）；
     *     若按 uint32 解析，负功率会变成约 16777kW 的巨大正值。!!! */
    int32_t p_raw = (int32_t)plain[4]
                  | ((int32_t)plain[5] << 8)
                  | ((int32_t)plain[6] << 16);
    if (p_raw & 0x00800000) {          /* 24 位最高位为 1 -> 负数 */
        p_raw -= 0x01000000;           /* 等价于符号扩展到 int32 */
    }
    out->active_power[phase] = (float)p_raw / 1000.0f;   /* W -> kW */

    /* [7:11] 正向有功电能：uint32 小端，0.01kWh/LSB */
    uint32_t fe_raw = (uint32_t)plain[7]
                    | ((uint32_t)plain[8]  << 8)
                    | ((uint32_t)plain[9]  << 16)
                    | ((uint32_t)plain[10] << 24);
    out->forward_energy[phase] = (float)fe_raw / 100.0f;

    /* [11:15] 反向有功电能：uint32 小端，0.01kWh/LSB */
    uint32_t re_raw = (uint32_t)plain[11]
                    | ((uint32_t)plain[12] << 8)
                    | ((uint32_t)plain[13] << 16)
                    | ((uint32_t)plain[14] << 24);
    out->reverse_energy[phase] = (float)re_raw / 100.0f;

    /* 三相总有功功率（kW）= 各相有功之和；本相刚更新，另两相为快照旧值。 */
    out->total_active_power = out->active_power[0]
                            + out->active_power[1]
                            + out->active_power[2];

    /* !!! 数据字典 §9.3.1 的 15B 中【没有功率因数 PF】字段，
     *     out->power_factor[] / overall_power_factor 保持原值（首帧 0），
     *     严禁臆造；PF 需走 GATT 读特征，属后续 Phase，不在广播解析内。!!! */
}

/* ======================================================================
 * 调试辅助：把一段字节流打成单行 hex 日志
 * ====================================================================== */
static void log_hex_line(const char *prefix, const uint8_t *b, size_t n)
{
    /* 每字节 "xx " 占 3 字符；前缀预留 32 字节，末尾 '\0'，宁大勿小
     * （曾因缓冲少 6 字符把 26B 截断显示成 24B，误导排查，见 git 记录） */
    char line[3 * 31 + 32];
    int pos = snprintf(line, sizeof(line), "%s", prefix);
    for (size_t i = 0; i < n && pos < (int)sizeof(line) - 4; i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", b[i]);
    }
    ESP_LOGI(TAG, "%s", line);
}

/* BLE 地址转字符串（NimBLE 广播地址为小端序，按 xx:xx:..:xx 输出） */
static void addr_to_str(const uint8_t a[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             a[5], a[4], a[3], a[2], a[1], a[0]);
}

/**
 * @brief 空配置观测模式限频：同一 BLE 地址 1s 内最多详细打印 1 帧，
 *        被抑制的帧计数在下一次打印时一并报出，避免日志洪水。
 * @return true=本帧允许打印
 */
static bool observe_allow_log(const uint8_t addr[6])
{
    static uint8_t  s_last_addr[6];
    static int64_t  s_last_us = 0;
    static uint32_t s_suppressed = 0;
    static bool     s_have_last = false;

    int64_t now_us = esp_timer_get_time();
    bool same = s_have_last && (memcmp(s_last_addr, addr, 6) == 0);

    if (same && (now_us - s_last_us) < 1000 * 1000) {
        s_suppressed++;
        return false;
    }
    if (s_suppressed > 0) {
        ESP_LOGI(TAG, "(observe) %lu frames suppressed by rate-limit",
                 (unsigned long)s_suppressed);
        s_suppressed = 0;
    }
    memcpy(s_last_addr, addr, 6);
    s_last_us = now_us;
    s_have_last = true;
    return true;
}

/* ======================================================================
 * 扫描事件回调（框架对应任务书 5.2.5；并按安全通道文档 §9.2/§9.3 扩展）
 *
 * 包形态：
 *   5B 短包（step=0）：FF FF + state + 12bit 鉴别器(LE)
 *   26B 长包（step≠0）：FF FF + ctrl + counter(LE,u32) + ct15 + tag4
 *   24B 兼容：部分主机栈会剥掉前 2 字节 Company ID，偏移整体 -2
 * ====================================================================== */
static int ble_scan_callback(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }

    struct ble_hs_adv_fields fields;
    /* !!! 版本注意：NimBLE 广播报告结构体字段名是 length_data（不是 data_len，
     *     后者是新版改名，v5.2.3 内 NimBLE 用 length_data，写错直接编译失败）!!! */
    if (ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data) != 0) {
        return 0;
    }

    if (fields.mfg_data == NULL) {
        return 0;
    }

    const uint8_t *mfg = fields.mfg_data;
    size_t mfg_len = fields.mfg_data_len;
    const uint8_t *addr = event->disc.addr.val;
    char addr_s[18];
    addr_to_str(addr, addr_s);

    /* ── 形态判定与偏移表（NimBLE 的 mfg_data 通常含 FF FF 两字节）── */
    size_t off_ctrl, off_ctr, off_ct, off_tag;
    if (mfg_len == BITUO_ADV_MFG_LEN && mfg[0] == 0xFF && mfg[1] == 0xFF) {
        off_ctrl = 2; off_ctr = 3; off_ct = 7; off_tag = 22;     /* 标准 26B */
    } else if (mfg_len == BITUO_ADV_MFG_LEN - 2) {
        /* 理论防御：若某主机栈/固件变体剥掉 Company ID 两字节（24B），
         * 偏移整体前移 2。实测（2026-09-01 真机+PC bleak 对拍）本设备
         * NimBLE 上报的 mfg_data 含 FF FF、标准 26B，正常不会走到这里；
         * 若真走到，先打印原始 AD 结构核对，不要直接相信是设备变体。 */
        off_ctrl = 0; off_ctr = 1; off_ct = 5; off_tag = 20;
    } else if (mfg_len == 5 && mfg[0] == 0xFF && mfg[1] == 0xFF) {
        /* ── 5B 短包（安全通道文档 §9.2，step=0 待配网电表）── */
        uint8_t  state = mfg[2];
        uint16_t disc  = (uint16_t)(mfg[3] | (mfg[4] << 8));
        disc &= 0x0FFFu;   /* 仅低 12bit 有效：= SN(48bit) 的最高 12bit */
        if (g_meter_count == 0) {
            if (observe_allow_log(addr)) {
                ESP_LOGI(TAG, "(observe) SHORT5 %s rssi=%d state=%u disc=0x%03x",
                         addr_s, event->disc.rssi, state, disc);
                log_hex_line("(observe) short hex: ", mfg, mfg_len);
            }
        }
        return 0;
    } else {
        return 0;   /* 非本协议厂商数据 */
    }

    /* counter 为小端 uint32，手工转主机序 */
    uint32_t counter = (uint32_t)mfg[off_ctr]        |
                       ((uint32_t)mfg[off_ctr + 1] << 8) |
                       ((uint32_t)mfg[off_ctr + 2] << 16)|
                       ((uint32_t)mfg[off_ctr + 3] << 24);

    /* 构造 BE32 Nonce（禁止 memcpy，见函数内警告）。Nonce 只依赖广播
     * counter，与源 BLE 地址无关，因此对所有候选表只构造一次。 */
    uint8_t nonce[BITUO_CCM_NONCE_LEN];
    ble_scanner_build_nonce(counter, nonce);

    /* !!! 坑点提醒（RPA 随机私有地址，关键）!!!
     * 该型电表 BLE 地址是随机私有地址，每次重新配网或重启后都会变
     * （实测同一表先后为 7E:1E:.. 与 47:0E:..），因此【绝不能】按 MAC 匹配
     * ——配置时记下的地址下一帧就失效，find_by_mac 永远落空。
     * 正确做法（安全通道文档 9.3：持有 broadcast_key 即可无连接解密，与地址
     * 无关）：对每一块已配置电表，用其 broadcast_key 逐一尝试 AES-128-CCM
     * 解密（密文 15B、Tag 4B），谁的 Tag 校验通过，这一帧就属于谁。
     * 最多 16 次解密，ESP32-S3 上总耗时几十微秒，相对 200ms 周期可忽略。
     * 注：扫描回调与 add/del 配置回调同属 NimBLE host 单任务，遍历期间配置
     * 不会被并发修改，无需额外加锁。 */
    uint8_t plaintext[BITUO_PLAINTEXT_LEN];
    int idx = -1;
    for (int i = 0; i < g_meter_count; i++) {
        if (!g_meter_configs[i].enabled) {
            continue;
        }
        int try_rc = aes_ccm_decrypt(mfg + off_ct, BITUO_PLAINTEXT_LEN,
                                     mfg + off_tag, BITUO_CCM_TAG_LEN,
                                     g_meter_configs[i].broadcast_key,
                                     nonce, plaintext);
        if (try_rc == 0) {
            idx = i;   /* Tag 校验通过，命中该表 */
            break;
        }
    }

    if (idx < 0) {
        /* 所有已配置 key 均无法通过 Tag 校验：非本网关配置的表，或 key 不符。
         * 一台表都未配置时进入观测模式打印原始长包，便于空口排查。 */
        if (g_meter_count == 0) {
            if (observe_allow_log(addr)) {
                ESP_LOGI(TAG, "(observe) LONG%u %s rssi=%d ctrl=0x%02x counter=%lu",
                         (unsigned)mfg_len, addr_s, event->disc.rssi,
                         mfg[off_ctrl], (unsigned long)counter);
                log_hex_line("(observe) long hex: ", mfg, mfg_len);
            }
        }
        return 0;
    }

    /* 解密命中：打印控制字节拆解与 15B 明文 hex 供对拍 */
    uint8_t ctrl_byte = mfg[off_ctrl];
    uint8_t phase_f   = (uint8_t)((ctrl_byte >> 2) & 0x3);  /* bit3-2 相位/回路 */
    uint8_t cat_f     = (uint8_t)(ctrl_byte & 0x3);         /* bit1-0 产品类别 */
    uint8_t enc_ver   = (uint8_t)((ctrl_byte >> 4) & 0x7);  /* bit6-4 编码版本 */
    uint8_t gatt_inst = (uint8_t)((ctrl_byte >> 7) & 0x1);  /* bit7   GATT 是否已装 */

    /* 编码版本防御：本解析仅实现 §9.3.1 的版本 1 布局；遇到未知版本只告警一次，
     * 仍按 v1 解析（后续若出新布局，需在此分流，不能静默错解）。 */
    if (enc_ver != 1U) {
        static bool s_warned_bad_ver = false;
        if (!s_warned_bad_ver) {
            s_warned_bad_ver = true;
            ESP_LOGW(TAG, "unknown plaintext encoding ver=%u (parser implements v1 only)",
                     (unsigned)enc_ver);
        }
    }

    /* 每表最多 1 秒打一行：回调里打 UART 会拖住 NimBLE，漏掉别的表。 */
    static uint32_t s_log_s[MAX_METERS];
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    bool do_log = (idx >= 0 && idx < MAX_METERS && now_s != s_log_s[idx]);
    if (do_log) {
        s_log_s[idx] = now_s;
        ESP_LOGI(TAG, "DECRYPT OK %s idx=%d ctrl=0x%02x(ver=%u phase=%u cat=%u gatt=%u) counter=%lu rssi=%d",
                 addr_s, idx, ctrl_byte, (unsigned)enc_ver, (unsigned)phase_f,
                 (unsigned)cat_f, (unsigned)gatt_inst,
                 (unsigned long)counter, event->disc.rssi);
        log_hex_line("decrypted plain15: ", plaintext, BITUO_PLAINTEXT_LEN);
    }

    /* 组装实时数据并覆盖写入该表深度1队列（只留最新“合并快照”）。
     * !!! 坑点提醒：三相按帧轮换、每帧只带一相，必须先 Peek 上一次快照做基底、
     * 只刷新本帧相位；若每帧 memset 清零，另两相会在 0/实测值之间反复跳变。 */
    meter_data_t data;
    if (xQueuePeek(g_meter_queues[idx], &data, 0) != pdTRUE) {
        memset(&data, 0, sizeof(data));             /* 首帧：无历史快照 */
    }
    parse_plaintext(plaintext, &data, phase_f);      /* 仅更新当前相位字段 */
    data.rssi      = event->disc.rssi;
    data.timestamp = (uint32_t)(esp_timer_get_time() / 1000000);
    data.counter   = counter;
    data.valid     = true;
    memcpy(data.sn, g_meter_configs[idx].sn, METER_SN_LEN);

    /* Phase1 收尾对拍日志：本相 V/I/P/FE/RE + 三相总功率（kW）。
     * 三相按帧轮换，连续看 3 帧即可覆盖 A/B(C) 全部相位。 */
    if (do_log) {
        ESP_LOGI(TAG, "PARSE ph%u: V=%.1f I=%.2f P=%.3fkW FE=%.2f RE=%.2fkWh | totalP=%.3fkW",
                 (unsigned)phase_f,
                 data.voltage[phase_f], data.current[phase_f],
                 data.active_power[phase_f],
                 data.forward_energy[phase_f], data.reverse_energy[phase_f],
                 data.total_active_power);
    }

    if (g_meter_queues[idx] != NULL) {
        xQueueOverwrite(g_meter_queues[idx], &data);
    }

    return 0;
}

/* ======================================================================
 * NimBLE host 同步回调：协议栈就绪后启动被动扫描
 * ====================================================================== */
static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }

    rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER,
                      &s_scan_params, ble_scan_callback, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc start rc=%d", rc);
    } else {
        s_scanning = true;
        ESP_LOGI(TAG, "passive scan started (itvl=0x%x window=0x%x)",
                 s_scan_params.itvl, s_scan_params.window);
    }

    /* Phase2：同时启动可连接广播，供 PC 端 dial_config.py 发现并连接配置 */
    ble_gatt_server_adv_start();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();        /* 该函数内部循环，NimBLE 线程跑在 Core0 */
    nimble_port_freertos_deinit();
}

/* ======================================================================
 * 对外初始化/启动接口
 * ====================================================================== */
esp_err_t ble_scanner_init(void)
{
    ESP_LOGI(TAG, "ble_scanner_init: NimBLE passive scanner");

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(ret));
        return ret;
    }

    /* GAP/GATT 基础服务（GATT Server 在 Phase2 于 ble_gatt_server.c 注册） */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("BitUo-Dial");

    ble_hs_cfg.sync_cb = ble_on_sync;
    /* TODO: reset_cb 中做地址重新推断；Phase2 在此合并 GATT 服务注册 */

    /* TODO(坑5): Wi-Fi 启动后调用
     * esp_coex_preference_set(ESP_COEX_PREFER_BALANCE); 平衡 BLE/Wi-Fi 共存 */

    return ESP_OK;
}

esp_err_t ble_scanner_start(void)
{
    /* NimBLE host 任务由 FreeRTOS 承载；协议栈线程归属 Core0 */
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "nimble host task launched");
    return ESP_OK;
}
