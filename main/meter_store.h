/**
 * @file    meter_store.h
 * @brief   电表配置与实时数据结构、PSRAM 存储与每表深度1队列
 *          （严格对应任务书 5.3 节）
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#ifndef MAIN_METER_STORE_H_
#define MAIN_METER_STORE_H_

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_METERS      16
#define METER_SN_LEN    13
#define METER_LABEL_LEN 32

/* 电表静态配置（NVS 持久化，运行期放 PSRAM） */
typedef struct {
    char    sn[METER_SN_LEN];
    uint8_t mac[6];
    uint8_t broadcast_key[16];
    char    label[METER_LABEL_LEN];
    bool    enabled;
} meter_config_t;

/* 单表实时数据（由 BLE 回调 xQueueOverwrite 持续刷新） */
typedef struct {
    char     sn[METER_SN_LEN];
    uint32_t timestamp;
    uint32_t counter;
    int8_t   rssi;
    float    voltage[3];         /* X/Y/Z(A/B/C)，单位 V（§9.3.1 uint16/10） */
    float    current[3];         /* X/Y/Z(A/B/C)，单位 A（§9.3.1 uint16/100） */
    float    active_power[3];    /* X/Y/Z(A/B/C)，单位 kW（§9.3.1 int24/1000，可负） */
    float    forward_energy[3];  /* 正向有功电能，单位 kWh（uint32/100）；0=暂无 */
    float    reverse_energy[3];  /* 反向有功电能，单位 kWh（uint32/100）；SRS01A/B 恒0 */
    float    power_factor[3];    /* 功率因数：广播 §9.3.1 不含此字段，恒 0，不从广播产生 */
    float    total_active_power; /* 三相总有功功率，单位 kW（各相之和） */
    float    overall_power_factor; /* 总功率因数：广播不含，恒 0 */
    bool     valid;
} meter_data_t;

extern meter_config_t    *g_meter_configs;   /* PSRAM */
extern meter_data_t      *g_meter_data;      /* PSRAM */
extern QueueHandle_t      g_meter_queues[MAX_METERS];
extern SemaphoreHandle_t  g_meters_mutex;
extern int                g_meter_count;

/**
 * @brief 分配 PSRAM 配置/数据数组、互斥锁与每表深度 1 队列
 */
void meter_store_init(void);

/**
 * @brief 按 BLE 广播 MAC 查找已配置电表下标
 * @return 0..MAX_METERS-1 命中；-1 未配置（扫描回调直接丢弃该帧）
 * @note  传入的 mac[6] 是 NimBLE 小端存储序（addr.val），存储时已按此序保存。
 */
int meter_store_find_by_mac(const uint8_t mac[6]);

/** @brief 按 SN 字符串查找已配置电表下标；-1 未找到 */
int meter_store_find_by_sn(const char *sn);

/**
 * @brief 新增或更新一条电表配置（SN 已存在则覆盖更新，幂等）
 * @param sn       12 位十六进制 SN（如 "50701B597664"）
 * @param mac_str  显示序 MAC 字符串（如 "AA:BB:CC:DD:EE:FF"），内部反转存储
 * @param key_hex  32 位十六进制 broadcast_key
 * @param label    显示标签（可为空，默认用 SN）
 * @return >=0 配置槽下标；-1 表列表已满(max16)；-3 NVS 写入失败；
 *         -4 invalid sn(非12hex)；-5 invalid key(非32hex)；-6 invalid mac
 */
int meter_store_add(const char *sn, const char *mac_str,
                    const char *key_hex, const char *label);

/**
 * @brief 按 SN 删除一条电表配置（后续条目前移保持紧凑）
 * @return 0 成功；-1 SN 不存在；-2 NVS 写入失败
 */
int meter_store_del_by_sn(const char *sn);

/**
 * @brief 槽位是否在线：深度 1 队列里最新帧 valid=true
 * @note  广播只写队列、不写 g_meter_data；>30s 离线由 ui_task 把队列 valid 置 false。
 *         从未收到帧（队列空）视为离线。
 */
bool meter_store_is_online(int idx);

/** @brief 当前在线电表数量（与 MQTT data.online / get_meters.valid 同源） */
int meter_store_online_count(void);

/**
 * @brief 从 NVS 载入电表列表（config_load 内部调用；NVS 空则 count=0）
 * @note  blob 大小 = sizeof(meter_config_t)；版本不匹配的条目跳过并告警。
 */
esp_err_t meter_store_load_nvs(void);

/**
 * @brief 把当前电表列表写入 NVS（仅 add/del 配置变更路径调用，严禁高频写）
 */
esp_err_t meter_store_save_nvs(void);

/**
 * @brief Phase1 临时：NVS 为空时注入一条编译期测试电表槽（真实 MAC+key）
 * @note  仅用于 Phase1 真机解密验收；Phase2 用 GATT 配置通道录入真实表后，
 *         NVS 非空时本函数自动跳过；最终交付前删除本函数及其调用。
 *         MAC 以"显示序"字符串传入，内部完成 NimBLE 小端 addr 序转换。
 */
void meter_store_phase1_inject(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_METER_STORE_H_ */
