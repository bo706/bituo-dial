/**
 * @file    ui_overview.h
 * @brief   页面1：列表概览（任务书 5.5.1）
 *          每行：标签/SN + 三相平均电压 + 总功率 + 在线状态。
 *          圆屏同时最多显示 3 行；最多 16 表靠旋钮滚动窗口。
 *          旋钮上下移动选中行，在 ui_task 内短按进入该表详情页。
 * @note    仅 ui_task 调用本接口（任务书坑3）。
 * @version LVGL v8.3 / ESP-IDF v5.2.3
 * @date    2026-09-01  @modified 2026-09-03 Phase3
 */
#ifndef MAIN_UI_OVERVIEW_H_
#define MAIN_UI_OVERVIEW_H_

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_OVERVIEW_MAX_ROWS    8     /* 预创建行控件 */
#define UI_OVERVIEW_VISIBLE     3     /* 圆屏同时最多 3 行；16 表靠旋钮滚动窗口 */

esp_err_t ui_overview_create(void);          /* 创建页面 screen 与控件 */
lv_obj_t *ui_overview_screen(void);          /* 返回本页 screen 供切换 */
void      ui_overview_refresh(void);         /* 周期刷新各行数据（仅 ui_task） */
void      ui_overview_select_delta(int d);   /* 旋钮：选中行上/下移动（可滚动窗口） */
int       ui_overview_selected(void);        /* 当前选中的电表下标 */

#ifdef __cplusplus
}
#endif
#endif /* MAIN_UI_OVERVIEW_H_ */
