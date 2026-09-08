/**
 * @file    bituo_mqtt.h
 * @brief   MQTT 北向：data / summary / mdata / cmd / cdata（任务书 5.9）
 * @note    头文件不叫 mqtt_client.h，避免与 IDF 组件同名冲突。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-03
 */
#ifndef MAIN_BITUO_MQTT_H_
#define MAIN_BITUO_MQTT_H_

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 汇总 Topic 默认周期（介绍页可配 5/10/30/60，此处取 10s） */
#define MQTT_SUMMARY_PERIOD_MS  10000

/**
 * @brief 按 NVS mqtt_* 启动/重启客户端；无 host 或尚未拿到 STA IP 则推迟
 */
esp_err_t mqtt_client_start(void);

/** @brief 是否已与 Broker 握手成功 */
bool mqtt_client_is_connected(void);

/** @brief 立刻发布一轮单表 data + summary（north_task 周期调用） */
void mqtt_publish_cycle(void);

/** @brief 创建 north_task（Core1 / prio3 / 栈 8192） */
esp_err_t north_task_start(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_BITUO_MQTT_H_ */
