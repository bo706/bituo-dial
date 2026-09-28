/**
 * @file    ble_gatt_server.h
 * @brief   明文 BLE GATT 配置通道服务端（任务书 5.7 节）
 * @note    开发阶段为明文 JSON 通道（与 Basic/Basic+ 一致），
 *          不做 EC J-PAKE 握手（商业化阶段由公司内部接入，不在本任务范围）。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#ifndef MAIN_BLE_GATT_SERVER_H_
#define MAIN_BLE_GATT_SERVER_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 服务/特征 UUID（任务书 5.7.1） */
#define BITUO_GATT_SVC_UUID        "0000a003-0000-1000-8000-00805f9b34fb"
#define BITUO_GATT_CHAR_WRITE_UUID "0000c313-0000-1000-8000-00805f9b34fb" /* 客户端->Dial */
#define BITUO_GATT_CHAR_READ_UUID  "0000c314-0000-1000-8000-00805f9b34fb"
#define BITUO_GATT_CHAR_NOTIFY_UUID "0000c315-0000-1000-8000-00805f9b34fb" /* Dial->客户端 */

/* 协商 MTU，避免长 JSON 被默认 23 字节 MTU 分包（任务书坑8） */
#define BITUO_GATT_PREFERRED_MTU   512

/**
 * @brief 初始化并广播明文 GATT 配置服务（Phase 2 实现）
 * @note  必须在 nimble_port_init 之后、nimble_port_freertos_init（host 启动）之前调用。
 */
esp_err_t ble_gatt_server_start(void);

/**
 * @brief host sync 后启动可连接广播（由 ble_scanner 的 sync_cb 调用）
 * @note  广播与被动扫描共存；广播数据含 flags + 完整设备名 "BitUo-Dial"。
 */
esp_err_t ble_gatt_server_adv_start(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_BLE_GATT_SERVER_H_ */
