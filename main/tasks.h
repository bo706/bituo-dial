/**
 * @file    tasks.h
 * @brief   FreeRTOS 任务创建与全局事件组定义（任务书 5.4 节）
 * @note    ui/config/north/sys 四个应用任务全部钉在 Core 1，
 *          Core 0 留给 Wi-Fi / NimBLE 协议栈。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01  @modified 2026-09-03 Phase4
 */
#ifndef MAIN_TASKS_H_
#define MAIN_TASKS_H_

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

extern EventGroupHandle_t g_sys_events;

#define SYS_EVENT_WIFI_CONFIG_CHANGED  BIT0
#define SYS_EVENT_MQTT_CONFIG_CHANGED  BIT1
#define SYS_EVENT_METER_LIST_CHANGED   BIT2
#define SYS_EVENT_WIFI_CONNECTED       BIT3

esp_err_t tasks_start(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_TASKS_H_ */
