/**
 * @file    ble_scanner.h
 * @brief   NimBLE 被动扫描：监听电表 26B AES-128-CCM 加密厂商广播
 *          （任务书 5.2：itvl=0x50/window=0x28/passive，不发 SCAN_REQ）
 * @version ESP-IDF v5.2.3 / NimBLE
 * @date    2026-09-01
 */
#ifndef MAIN_BLE_SCANNER_H_
#define MAIN_BLE_SCANNER_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 广播包常量（任务书 5.2.1） */
#define BITUO_ADV_MFG_LEN      26    /* 厂商数据总长度 */
#define BITUO_ADV_COUNTER_OFF  3     /* counter 小端起始偏移 */
#define BITUO_ADV_CIPHER_OFF   7     /* 15B 密文起始偏移 */
#define BITUO_ADV_TAG_OFF      22    /* 4B CCM Tag 起始偏移 */

/**
 * @brief 初始化 NimBLE 协议栈与扫描参数（不开始扫描）
 */
esp_err_t ble_scanner_init(void);

/**
 * @brief 启动被动扫描（host 同步后在 sync 回调中自动开始）
 */
esp_err_t ble_scanner_start(void);

/**
 * @brief 由 32 位广播计数器构造 12B CCM Nonce：8B 全 0 || BE32(counter)
 * @param counter 广播包偏移 3..6 的小端计数器（已转成主机序）
 * @param nonce   输出 12B，调用方保证空间
 */
void ble_scanner_build_nonce(uint32_t counter, uint8_t nonce[12]);

/**
 * @brief 被动扫描是否已启动（Phase3 系统状态页用，只读）
 */
bool ble_scanner_is_scanning(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_BLE_SCANNER_H_ */
