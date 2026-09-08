/**
 * @file    ui_sysinfo.h
 * @brief   页面3：系统状态（任务书 5.5.1）
 *          Wi-Fi 状态+SSID+IP、MQTT 状态、BLE 扫描状态+在线/总数、固件版本、
 *          运行时长与剩余堆；长按由 ui_task 触发 ui_sysinfo_long_press()
 *          进入 SoftAP 配网（Phase4 完整落地）。
 * @note    仅 ui_task 调用。
 * @version LVGL v8.3 / ESP-IDF v5.2.3
 * @date    2026-09-01  @modified 2026-09-03 Phase3
 */
#ifndef MAIN_UI_SYSINFO_H_
#define MAIN_UI_SYSINFO_H_

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_sysinfo_create(void);
lv_obj_t *ui_sysinfo_screen(void);
void      ui_sysinfo_refresh(void);
void      ui_sysinfo_long_press(void);   /* 长按：进入 SoftAP 配网模式 */

#ifdef __cplusplus
}
#endif
#endif /* MAIN_UI_SYSINFO_H_ */
