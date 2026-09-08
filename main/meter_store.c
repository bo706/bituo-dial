/**
 * @file    meter_store.c
 * @brief   电表配置/数据的内存分配与队列创建（任务书 5.3）
 * @note    零售版 M5Dial：g_meter_configs / g_meter_data 放 Octal PSRAM；
 *          每表一个深度 1 队列，配合 xQueueOverwrite 只保留最新帧。
 *          2026-09-01 实物核验：手上模块无 PSRAM（Octal/Quad 两种模式
 *          启动均报 PSRAM ID read error），故分配策略改为"优先 PSRAM、
 *          失败回退内部 RAM"，同一份代码在两种硬件上都可运行；
 *          16 个表槽结构体总量不足 2KB，回退内部 RAM 无容量压力。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#include "meter_store.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

#include "config.h"

static const char *TAG = "meter_store";

/* 全局存储定义（头文件 extern 声明） */
meter_config_t    *g_meter_configs = NULL;
meter_data_t      *g_meter_data = NULL;
QueueHandle_t      g_meter_queues[MAX_METERS] = {NULL};
SemaphoreHandle_t  g_meters_mutex = NULL;
int                g_meter_count = 0;

/**
 * @brief 带 PSRAM/内部 RAM 回退的 calloc
 * @note  !!! 坑点提醒（任务书坑6）!!!
 *         零售 M5Dial 需 CONFIG_SPIRAM=y + CONFIG_SPIRAM_MODE_OCT=y(80MHz)，
 *         此时 MALLOC_CAP_SPIRAM 成功、数组落在 PSRAM；
 *         若硬件无 PSRAM（本机实测）或配置缺失，MALLOC_CAP_SPIRAM 返回 NULL，
 *         自动回退 MALLOC_CAP_INTERNAL，保证框架可运行，日志如实标注落点。
 */
static void *calloc_with_psram_fallback(size_t n, size_t size, const char *what)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
    if (p != NULL) {
        ESP_LOGI(TAG, "%s allocated in PSRAM (%u bytes)", what, (unsigned)(n * size));
        return p;
    }
    p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p != NULL) {
        ESP_LOGW(TAG, "%s PSRAM unavailable, fallback to INTERNAL RAM (%u bytes)",
                 what, (unsigned)(n * size));
    }
    return p;
}

void meter_store_init(void)
{
    g_meter_configs = calloc_with_psram_fallback(MAX_METERS,
                                                 sizeof(meter_config_t), "meter_configs");
    g_meter_data = calloc_with_psram_fallback(MAX_METERS,
                                              sizeof(meter_data_t), "meter_data");
    if (g_meter_configs == NULL || g_meter_data == NULL) {
        ESP_LOGE(TAG, "alloc failed: PSRAM and internal RAM both exhausted");
        /* 框架阶段不 assert；正式版应 assert 并停机 */
        return;
    }

    g_meters_mutex = xSemaphoreCreateMutex();
    if (g_meters_mutex == NULL) {
        ESP_LOGE(TAG, "create g_meters_mutex failed");
        return;
    }

    for (int i = 0; i < MAX_METERS; i++) {
        /* 深度=1：xQueueOverwrite 永远只保留最新一帧，UI/北向不积压历史帧 */
        g_meter_queues[i] = xQueueCreate(1, sizeof(meter_data_t));
        if (g_meter_queues[i] == NULL) {
            ESP_LOGE(TAG, "create queue[%d] failed", i);
        }
    }

    ESP_LOGI(TAG, "meter_store_init ok: %d meter slots ready", MAX_METERS);
}

int meter_store_find_by_mac(const uint8_t mac[6])
{
    if (mac == NULL || g_meter_configs == NULL) {
        return -1;
    }

    /* TODO(Phase2): 配置增删后遍历改为哈希/排序表以降低 16 表扫描开销；
     * 当前线性遍历即可，访问期间按需 xSemaphoreTake(g_meters_mutex)。
     */
    for (int i = 0; i < g_meter_count; i++) {
        if (g_meter_configs[i].enabled &&
            memcmp(g_meter_configs[i].mac, mac, 6) == 0) {
            return i;
        }
    }
    return -1;
}

/* ======================================================================
 * Phase2：电表配置增删查 + NVS 持久化（任务书 5.3 / 5.10 / 7.3）
 * ====================================================================== */

/* NVS Key 前缀与 meter_config_t 紧凑性校验 */
#define METER_NVS_COUNT_KEY  "meter_count"
#define METER_NVS_ENTRY_FMT  "meter_%d"
/* meter_config_t: sn[13]+mac[6]+key[16]+label[32]+enabled(1) = 68 字节，
 * 所有成员对齐 1，无填充；若未来加字段导致对齐填充，NVS blob 会变大，
 * load 时按实际大小校验，不匹配的旧条目自动跳过。 */
_Static_assert(sizeof(meter_config_t) == 68, "meter_config_t size changed, review NVS blob");

/* ── 内部辅助：十六进制字符校验与转换 ── */
static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_hex_str(const char *s, int expect_len)
{
    if (s == NULL) return false;
    int n = 0;
    while (s[n] != '\0') {
        if (hex_val(s[n]) < 0) return false;
        n++;
    }
    return n == expect_len;
}

/**
 * @brief 解析显示序 MAC 字符串 "AA:BB:CC:DD:EE:FF"，输出 NimBLE 小端存储序
 * @note  !!! 字节序坑 !!! NimBLE event->disc.addr.val[0] 对应显示 MAC 最后一字节，
 *        meter_store_find_by_mac 用 memcmp 与 addr.val 直接比，因此存储必须反转。
 */
static bool parse_mac_str(const char *s, uint8_t out[6])
{
    if (s == NULL || out == NULL) return false;
    unsigned int d[6];
    int n = sscanf(s, "%x:%x:%x:%x:%x:%x",
                   &d[0], &d[1], &d[2], &d[3], &d[4], &d[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (d[i] > 0xFF) return false;
        out[i] = (uint8_t)d[5 - i];   /* 显示序 -> 存储小端序 */
    }
    return true;
}

static bool parse_key_hex(const char *s, uint8_t out[16])
{
    if (!is_hex_str(s, 32)) return false;
    for (int i = 0; i < 16; i++) {
        out[i] = (uint8_t)((hex_val(s[2 * i]) << 4) | hex_val(s[2 * i + 1]));
    }
    return true;
}

int meter_store_find_by_sn(const char *sn)
{
    if (sn == NULL || g_meter_configs == NULL) return -1;
    for (int i = 0; i < g_meter_count; i++) {
        if (g_meter_configs[i].enabled &&
            strncmp(g_meter_configs[i].sn, sn, METER_SN_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

int meter_store_add(const char *sn, const char *mac_str,
                    const char *key_hex, const char *label)
{
    /* 参数校验（任务书 7.6 错误码：invalid sn / invalid key / invalid mac，分别返回） */
    if (!is_hex_str(sn, 12)) {
        ESP_LOGW(TAG, "add: invalid sn '%s' (expect 12 hex)", sn ? sn : "(null)");
        return -4;   /* invalid sn */
    }
    if (!is_hex_str(key_hex, 32)) {
        ESP_LOGW(TAG, "add: invalid bcast_key (expect 32 hex)");
        return -5;   /* invalid key */
    }
    uint8_t mac[6];
    if (!parse_mac_str(mac_str, mac)) {
        ESP_LOGW(TAG, "add: invalid mac '%s'", mac_str ? mac_str : "(null)");
        return -6;   /* invalid mac */
    }

    xSemaphoreTake(g_meters_mutex, portMAX_DELAY);

    /* SN 已存在则覆盖更新（幂等），否则新增 */
    int idx = meter_store_find_by_sn(sn);
    if (idx < 0) {
        if (g_meter_count >= MAX_METERS) {
            xSemaphoreGive(g_meters_mutex);
            ESP_LOGW(TAG, "add: meter list full (max %d)", MAX_METERS);
            return -1;   /* 任务书 7.6: "meter list full" */
        }
        idx = g_meter_count++;
    }

    meter_config_t *slot = &g_meter_configs[idx];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->sn, METER_SN_LEN, "%s", sn);
    memcpy(slot->mac, mac, 6);
    parse_key_hex(key_hex, slot->broadcast_key);   /* 已校验，必成功 */
    snprintf(slot->label, METER_LABEL_LEN, "%s",
             (label && label[0]) ? label : sn);
    slot->enabled = true;

    /* 对应实时数据槽清零 */
    if (g_meter_data != NULL) {
        memset(&g_meter_data[idx], 0, sizeof(meter_data_t));
    }

    esp_err_t rc = meter_store_save_nvs();   /* 仅配置变更时写一次 NVS */
    xSemaphoreGive(g_meters_mutex);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "add: nvs save failed: %s", esp_err_to_name(rc));
        return -3;   /* 任务书 7.6: "nvs write failed" */
    }
    ESP_LOGI(TAG, "add: meter sn=%s idx=%d mac=%s label='%s'",
             sn, idx, mac_str, slot->label);
    return idx;
}

int meter_store_del_by_sn(const char *sn)
{
    xSemaphoreTake(g_meters_mutex, portMAX_DELAY);
    int idx = meter_store_find_by_sn(sn);
    if (idx < 0) {
        xSemaphoreGive(g_meters_mutex);
        ESP_LOGW(TAG, "del: sn '%s' not found", sn ? sn : "(null)");
        return -1;   /* 任务书 7.6: "sn not found" */
    }
    /* 后续条目前移保持紧凑，末尾清零 */
    for (int i = idx; i < g_meter_count - 1; i++) {
        g_meter_configs[i] = g_meter_configs[i + 1];
        if (g_meter_data != NULL) {
            g_meter_data[i] = g_meter_data[i + 1];
        }
    }
    g_meter_count--;
    memset(&g_meter_configs[g_meter_count], 0, sizeof(meter_config_t));
    if (g_meter_data != NULL) {
        memset(&g_meter_data[g_meter_count], 0, sizeof(meter_data_t));
    }
    if (g_meter_queues[g_meter_count] != NULL) {
        xQueueReset(g_meter_queues[g_meter_count]);
    }

    esp_err_t rc = meter_store_save_nvs();
    xSemaphoreGive(g_meters_mutex);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "del: nvs save failed: %s", esp_err_to_name(rc));
        return -2;
    }
    ESP_LOGI(TAG, "del: meter sn=%s removed, remaining=%d", sn, g_meter_count);
    return 0;
}

bool meter_store_is_online(int idx)
{
    meter_data_t d;
    if (idx < 0 || idx >= g_meter_count || g_meter_queues[idx] == NULL) {
        return false;
    }
    if (xQueuePeek(g_meter_queues[idx], &d, 0) != pdTRUE) {
        return false;
    }
    return d.valid;
}

int meter_store_online_count(void)
{
    int n = 0;
    for (int i = 0; i < g_meter_count; i++) {
        if (meter_store_is_online(i)) {
            n++;
        }
    }
    return n;
}

esp_err_t meter_store_load_nvs(void)
{
    if (g_meter_configs == NULL) return ESP_ERR_INVALID_STATE;

    nvs_handle_t h;
    esp_err_t rc = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &h);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "load_nvs: namespace open %s, start with empty meter list",
                 esp_err_to_name(rc));
        g_meter_count = 0;
        return ESP_OK;
    }

    uint8_t count = 0;
    rc = nvs_get_u8(h, METER_NVS_COUNT_KEY, &count);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "load_nvs: no meter_count key, empty list");
        g_meter_count = 0;
        nvs_close(h);
        return ESP_OK;
    }
    if (rc != ESP_OK) { nvs_close(h); return rc; }
    if (count > MAX_METERS) count = MAX_METERS;

    int loaded = 0;
    for (uint8_t i = 0; i < count; i++) {
        char key[24];
        snprintf(key, sizeof(key), METER_NVS_ENTRY_FMT, i);
        size_t blob_len = 0;
        rc = nvs_get_blob(h, key, NULL, &blob_len);
        if (rc != ESP_OK || blob_len != sizeof(meter_config_t)) {
            /* 版本不匹配或条目损坏：跳过，不崩溃，日志如实标注 */
            ESP_LOGW(TAG, "load_nvs: meter_%d blob len=%u (expect %u), skip",
                     i, (unsigned)blob_len, (unsigned)sizeof(meter_config_t));
            continue;
        }
        rc = nvs_get_blob(h, key, &g_meter_configs[loaded], &blob_len);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "load_nvs: meter_%d read failed: %s, skip",
                     i, esp_err_to_name(rc));
            continue;
        }
        g_meter_configs[loaded].enabled = true;   /* 防御：NVS 里 enabled 必为 true */
        loaded++;
    }
    g_meter_count = loaded;
    nvs_close(h);
    ESP_LOGI(TAG, "load_nvs: %d meter(s) loaded from NVS", g_meter_count);
    return ESP_OK;
}

esp_err_t meter_store_save_nvs(void)
{
    /* 调用方应已持有 g_meters_mutex；本函数不重复加锁以避免递归 */
    nvs_handle_t h;
    esp_err_t rc = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (rc != ESP_OK) return rc;

    rc = nvs_set_u8(h, METER_NVS_COUNT_KEY, (uint8_t)g_meter_count);
    for (int i = 0; i < g_meter_count && rc == ESP_OK; i++) {
        char key[24];
        snprintf(key, sizeof(key), METER_NVS_ENTRY_FMT, i);
        rc = nvs_set_blob(h, key, &g_meter_configs[i], sizeof(meter_config_t));
    }
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);
    return rc;
}

/* ======================================================================
 * Phase1 真机验收测试槽（2026-09-02 手机 App 配网后实测获得）
 * ----------------------------------------------------------------------
 * !!! 临时实现：Phase2 配置通道（GATT/SoftAP + NVS 持久化）上线后删除 !!!
 * ====================================================================== */
/* Phase1 真机测试槽：已由 NVS 正式配置取代（2026-09-02 真实表闭环），关闭。
 * 如需空口调试可临时改回 1。 */
#define PHASE1_TEST_SLOT   0

#if PHASE1_TEST_SLOT
/* 测试表实测信息（PC bleak 双端对拍验证，22 帧 CCM tag 全部通过） */
#define PHASE1_TEST_MAC_STR    "7e:1e:1e:f8:f7:f3"   /* 显示序 BLE 随机静态地址 */
#define PHASE1_TEST_SN         "50c?"                /* 仅知前 3 hex=鉴别器 0x50c，完整 SN 待补 */
#define PHASE1_TEST_LABEL      "Phase1 test meter"
/* broadcast_key：手机配网 App 界面读出，16 字节 */
/* GitHub 快照已去掉真机密钥；本地调试请自行填入。 */
static const uint8_t PHASE1_TEST_KEY[16] = {0};

void meter_store_phase1_inject(void)
{
    if (g_meter_configs == NULL) {
        ESP_LOGE(TAG, "phase1 inject: store not initialized");
        return;
    }
    /* NVS 已载入真实配置时，测试槽自动让位，避免覆盖用户配置 */
    if (g_meter_count > 0) {
        ESP_LOGI(TAG, "phase1 inject skipped: NVS already has %d meter(s)",
                 g_meter_count);
        return;
    }

    meter_config_t *slot = &g_meter_configs[0];
    memset(slot, 0, sizeof(*slot));

    /* 解析"显示序"MAC：7e:1e:1e:f8:f7:f3 -> disp[6] */
    uint8_t disp[6];
    if (sscanf(PHASE1_TEST_MAC_STR, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &disp[0], &disp[1], &disp[2], &disp[3], &disp[4], &disp[5]) != 6) {
        ESP_LOGE(TAG, "phase1 inject: bad MAC string");
        return;
    }

    /* !!! 坑点提醒（字节序）!!!
     * NimBLE 上报的 event->disc.addr.val 是【小端存储序】：
     *   addr.val[0] 对应显示 MAC 的最后一字节。
     * 而 meter_store_find_by_mac() 用 memcmp 与 addr.val 直接比较，
     * 因此存储时必须把显示序整体反转；直接存显示序会永远匹配不上。 */
    for (int i = 0; i < 6; i++) {
        slot->mac[i] = disp[5 - i];
    }

    memcpy(slot->broadcast_key, PHASE1_TEST_KEY, 16);
    snprintf(slot->sn, METER_SN_LEN, "%s", PHASE1_TEST_SN);
    snprintf(slot->label, METER_LABEL_LEN, "%s", PHASE1_TEST_LABEL);
    slot->enabled = true;

    g_meter_count = 1;   /* 全局仅这一张测试表 */
    ESP_LOGI(TAG, "phase1 test slot injected: mac=%s key=%02x%02x...%02x%02x",
             PHASE1_TEST_MAC_STR,
             PHASE1_TEST_KEY[0], PHASE1_TEST_KEY[1],
             PHASE1_TEST_KEY[14], PHASE1_TEST_KEY[15]);
}
#else
void meter_store_phase1_inject(void) { /* 测试槽关闭时为空实现 */ }
#endif
