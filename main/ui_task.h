/**
 * @file    ui_task.h
 * @brief   LVGL 主循环、三页调度、编码器/按键事件处理（任务书 5.5 节）
 * @note    !!! 坑点提醒（任务书 坑3）：全工程只有 ui_task 允许调用 lv_* API，
 *          其他任务（ble/north/sys）只能更新 g_meter_queues 数据队列，
 *          由 ui_task 在自己的循环里 xQueuePeek 后刷新控件，严禁跨任务调 LVGL。
 * @version LVGL v8.3 / ESP-IDF v5.2.3
 * @date    2026-09-01  @modified 2026-09-03 Phase3
 */
#ifndef MAIN_UI_TASK_H_
#define MAIN_UI_TASK_H_

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 圆屏几何（GC9A01 240×240）──────────────────────────────────────
 * 半径 120：贴边的矩形会被四角裁掉。标题/页脚再往里收，正文左右各留 36px
 * （约等于内接正方形），文字水平居中。 */
#define UI_SCREEN_SIZE      240
#define UI_MARGIN           36
#define UI_SAFE_TOP         28
#define UI_FOOT_Y           196   /* 页脚整行须落在圆内；204 时长句换行会被裁 */
#define UI_CONTENT_W        (UI_SCREEN_SIZE - 2 * UI_MARGIN)
#define UI_TITLE_H          32
#define UI_FOOT_H           22

/** 圆屏安全区内水平居中放置标签（仅 ui_task 调用） */
static inline void ui_label_place(lv_obj_t *lab, int y)
{
    lv_obj_set_width(lab, UI_CONTENT_W);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_CLIP);  /* 圆屏禁止换行叠字 */
    lv_obj_align(lab, LV_ALIGN_TOP_MID, 0, y);
}

/* ── 三页标识 ───────────────────────────────────────────────────── */
typedef enum {
    UI_PAGE_OVERVIEW = 0,   /* 页面1：列表概览 */
    UI_PAGE_DETAIL   = 1,   /* 页面2：单表详情 */
    UI_PAGE_SYSINFO  = 2,   /* 页面3：系统状态 */
    UI_PAGE_NUM      = 3,
} ui_page_t;

/* ── 统一配色（工程深色风格，圆屏 OLED/TFT，避免大面积高饱和色）────── */
#define UI_C_BG         lv_color_hex(0x000000)   /* 背景黑            */
#define UI_C_TEXT       lv_color_hex(0xFFFFFF)   /* 主文字白          */
#define UI_C_DIM        lv_color_hex(0x9AA0A6)   /* 次要文字灰        */
#define UI_C_ONLINE     lv_color_hex(0x35D47A)   /* 在线绿            */
#define UI_C_WARN       lv_color_hex(0xF2C14E)   /* 警告黄            */
#define UI_C_OFFLINE    lv_color_hex(0x6B7075)   /* 离线灰            */

/* 收包间隔：Wi-Fi+自身广播时实测 SPM 可空窗 15~40s（RSSI 仍约 -35）。
 * 任务书 5/30s 会把漏扫当成离线；白≤15s，黄 15~90s，灰>90s。 */
#define UI_AGE_WARN_S       15
#define UI_AGE_OFFLINE_S    90
#define UI_C_SELECT     lv_color_hex(0x1F6FEB)   /* 选中行蓝          */
#define UI_C_ACCENT     lv_color_hex(0x4FC3F7)   /* 数值强调青        */

/* ── Phase3 收尾开关：定值/达标后改 0 即可，不必删代码 ─────────────── */
#ifndef BITUO_ORIENT_SELFTEST
#define BITUO_ORIENT_SELFTEST  0   /* 已定值 0x48（USB 朝下正立）；改 1 可再跑四档 */
#endif
#ifndef BITUO_FPS_PROBE
#define BITUO_FPS_PROBE        0   /* 探针已过 ≥20；改 1 可再测满屏刷新 */
#endif

/**
 * @brief LVGL 初始化：lv_init + 1ms tick 定时器 + 双帧缓冲 + 注册 GC9A01
 *        显示驱动 + 创建三个页面（任务书 5.5.2，由 app_main 在 display_init
 *        之后、encoder_init 之后调用）
 */
esp_err_t lvgl_init(void);

/**
 * @brief 创建 ui_task（Core1，prio5，栈8192），内部跑编码器采集、页面调度、
 *        离线检测、蜂鸣提示与 lv_timer_handler
 */
esp_err_t ui_task_start(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_UI_TASK_H_ */
