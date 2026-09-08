/**
 * @file    tasks.c
 * @brief   FreeRTOS 任务调度：ui_task / config_task / north_task / sys_task
 *          （任务书 5.4 节任务表与 Core 1 铁律）
 * @note    占位框架，当前仅创建事件组；各任务在对应 Phase 用
 *          xTaskCreatePinnedToCore 钉到 Core 1 后启用。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#include "tasks.h"

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

#include "ui_task.h"
#include "sys_task.h"
#include "bituo_mqtt.h"

static const char *TAG = "tasks";

EventGroupHandle_t g_sys_events = NULL;

/* ── 任务规划（任务书 5.4，全部钉 Core 1）─────────────────────────────
 * ui_task     : Core1, prio 5, stack 8192 —— LVGL 渲染/编码器（Phase3）
 * config_task : Core1, prio 4, stack 6144 —— 明文 GATT 配置通道（Phase2）
 * north_task  : Core1, prio 3, stack 6144 —— MQTT + HTTP Server（Phase4）
 * sys_task    : Core1, prio 2, stack 4096 —— Wi-Fi/SoftAP/看门狗（Phase2）
 * NimBLE 协议栈线程由 nimble_port_freertos_init 自动创建在 Core 0
 * ─────────────────────────────────────────────────────────────────── */

esp_err_t tasks_start(void)
{
    ESP_LOGI(TAG, "tasks_start: create system event group");

    if (g_sys_events == NULL) {
        g_sys_events = xEventGroupCreate();
        if (g_sys_events == NULL) {
            ESP_LOGE(TAG, "failed to create g_sys_events");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Phase3：ui_task（Core1/prio5/栈8192）
     * Phase4：sys_task（Wi-Fi/mDNS）、north_task（MQTT 周期上报）
     * !!! 坑点提醒：最后一个参数必须是 1（Core1），严禁传 0 或 tskNO_AFFINITY !!! */
    esp_err_t err = ui_task_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ui_task_start failed: %s", esp_err_to_name(err));
        return err;
    }
    err = sys_task_start();
    if (err != ESP_OK) {
        return err;
    }
    err = north_task_start();
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}
