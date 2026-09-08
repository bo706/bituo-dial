/**
 * @file    sys_task.h
 * @brief   Wi-Fi STA/SoftAP 管理、指数退避重连、mDNS（任务书 5.6 / 5.9）
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01  @modified 2026-09-03 Phase4
 */
#ifndef MAIN_SYS_TASK_H_
#define MAIN_SYS_TASK_H_

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYS_SOFTAP_SSID_PREFIX  "BitUo-Dial-"
#define SYS_SOFTAP_PASSWORD     "12345678"
#define SYS_SOFTAP_IP           "192.168.4.1"

esp_err_t sys_wifi_connect(void);
esp_err_t sys_softap_start(void);

/** STA 是否已拿到 IPv4 */
bool sys_wifi_sta_got_ip(void);

/** SoftAP 是否已启动 */
bool sys_wifi_ap_started(void);

/** 优先 STA IP，否则 AP IP；都没有则空串 */
void sys_wifi_ip_str(char *buf, size_t cap);

esp_err_t sys_mdns_start(void);
esp_err_t sys_task_start(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_SYS_TASK_H_ */
