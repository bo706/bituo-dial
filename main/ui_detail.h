/**
 * @file    ui_detail.h
 * @brief   页面2：单表详情（任务书 5.5.1）
 *          顶部名称，三相 U/I，总功率/功率因数，RSSI 与“X 秒前”；
 *          旋钮切换上一表/下一表，ui_task 长按返回概览。
 * @note    仅 ui_task 调用。
 * @version LVGL v8.3 / ESP-IDF v5.2.3
 * @date    2026-09-01  @modified 2026-09-03 Phase3
 */
#ifndef MAIN_UI_DETAIL_H_
#define MAIN_UI_DETAIL_H_

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_detail_create(void);
lv_obj_t *ui_detail_screen(void);
void      ui_detail_refresh(void);
void      ui_detail_show(int idx);   /* 直接显示指定下标电表 */
void      ui_detail_step(int d);     /* 旋钮：上一表(d=-1)/下一表(d=+1) */

#ifdef __cplusplus
}
#endif
#endif /* MAIN_UI_DETAIL_H_ */
