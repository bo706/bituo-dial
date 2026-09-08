/**
 * @file    ui_detail.c
 * @brief   页面2：单表详情（任务书 5.5.1）
 *          顶部名称与序号，三相电压/电流，总功率与功率因数，RSSI 与最后更新
 *          时间（X 秒前）；旋钮上/下切表，长按返回概览（在 ui_task 处理）。
 * @note    仅 ui_task 调用；V/I/P/FE/RE 已按安全通道文档 §9.3.1 解析上屏，
 *          功率单位 kW；广播字典不含 PF，PF 固定显示 --。数据来自 g_meter_queues
 *          深度1队列（xQueuePeek）。
 * @version LVGL v8.3 / ESP-IDF v5.2.3
 * @date    2026-09-01  @modified 2026-09-03 Phase3
 */
#include "ui_detail.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "ui_task.h"
#include "meter_store.h"

static const char *TAG = "ui_detail";

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_name;
static lv_obj_t *s_phase[3];     /* 三相 U/I 行 */
static lv_obj_t *s_power;
static lv_obj_t *s_e_fwd;
static lv_obj_t *s_e_rev;
static lv_obj_t *s_meta;         /* RSSI + 更新时间，一行以免叠字 */
static lv_obj_t *s_footer;
static int       s_idx = 0;

/* LVGL 自带 sprintf 默认不开浮点（LV_SPRINTF_USE_FLOAT=0），
 * lv_label_set_text_fmt("%f") 会把 %.1fV 印成 "fV"。浮点一律走 newlib snprintf。 */
static void label_set_snprintf(lv_obj_t *lab, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(lab, buf);
}

esp_err_t ui_detail_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_name = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_name, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_name, UI_C_ACCENT, 0);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_CLIP);
    ui_label_place(s_name, UI_SAFE_TOP);

    static const char ptag[3] = { 'X', 'Y', 'Z' };
    int y = UI_SAFE_TOP + 24;
    for (int p = 0; p < 3; p++) {
        s_phase[p] = lv_label_create(s_screen);
        lv_obj_set_style_text_font(s_phase[p], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(s_phase[p], UI_C_TEXT, 0);
        ui_label_place(s_phase[p], y);
        lv_label_set_text_fmt(s_phase[p], "%c  --.-V  -.--A", ptag[p]);
        y += 20;
    }

    s_power = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_power, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_power, UI_C_DIM, 0);
    ui_label_place(s_power, y + 4);
    lv_label_set_text(s_power, "P --.--kW  PF --");

    s_e_fwd = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_e_fwd, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_e_fwd, UI_C_DIM, 0);
    ui_label_place(s_e_fwd, y + 22);
    lv_label_set_text(s_e_fwd, "E+ --.--kWh");

    s_e_rev = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_e_rev, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_e_rev, UI_C_DIM, 0);
    ui_label_place(s_e_rev, y + 40);
    lv_label_set_text(s_e_rev, "E- --.--kWh");

    s_meta = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_meta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_meta, UI_C_DIM, 0);
    ui_label_place(s_meta, y + 58);
    lv_label_set_text(s_meta, "--dBm  --s");

    s_footer = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_footer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_footer, UI_C_DIM, 0);
    ui_label_place(s_footer, UI_FOOT_Y);
    lv_label_set_text(s_footer, "turn: page");

    ESP_LOGI(TAG, "detail page created");
    return ESP_OK;
}

lv_obj_t *ui_detail_screen(void)
{
    return s_screen;
}

void ui_detail_show(int idx)
{
    s_idx = idx;
    ui_detail_refresh();
}

void ui_detail_step(int d)
{
    if (g_meter_count <= 0) {
        s_idx = 0;
        return;
    }
    s_idx += d;
    if (s_idx < 0) {
        s_idx = g_meter_count - 1;
    } else if (s_idx >= g_meter_count) {
        s_idx = 0;
    }
}

void ui_detail_refresh(void)
{
    if (s_screen == NULL) {
        return;
    }

    if (g_meter_count <= 0) {
        lv_label_set_text(s_name, "no meter");
        return;
    }
    if (s_idx >= g_meter_count) {
        s_idx = g_meter_count - 1;
    }

    const meter_config_t *cfg = &g_meter_configs[s_idx];
    lv_label_set_text_fmt(s_name, "%s (%d/%d)",
                          cfg->label[0] ? cfg->label : cfg->sn,
                          s_idx + 1, g_meter_count);

    meter_data_t d;
    bool got = (xQueuePeek(g_meter_queues[s_idx], &d, 0) == pdTRUE);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t age = got ? (now - d.timestamp) : 0xFFFFFFFFu;

    static const char ptag[3] = { 'X', 'Y', 'Z' };
    for (int p = 0; p < 3; p++) {
        if (got) {
            label_set_snprintf(s_phase[p], "%c  %.1fV  %.2fA", ptag[p],
                               d.voltage[p], d.current[p]);
        } else {
            lv_label_set_text_fmt(s_phase[p], "%c  --.-V  -.--A", ptag[p]);
        }
    }

    if (got) {
        float fe = d.forward_energy[0] + d.forward_energy[1] + d.forward_energy[2];
        float re = d.reverse_energy[0] + d.reverse_energy[1] + d.reverse_energy[2];
        label_set_snprintf(s_power, "P %.2fkW  PF --", d.total_active_power);
        label_set_snprintf(s_e_fwd, "E+ %.2fkWh", fe);
        label_set_snprintf(s_e_rev, "E- %.2fkWh", re);
        label_set_snprintf(s_meta, "%ddBm  %lus", (int)d.rssi, (unsigned long)age);
    } else {
        lv_label_set_text(s_power, "P --.--kW  PF --");
        lv_label_set_text(s_e_fwd, "E+ --.--kWh");
        lv_label_set_text(s_e_rev, "E- --.--kWh");
        lv_label_set_text(s_meta, "no data");
    }
    if (g_meter_count > 1) {
        lv_label_set_text(s_footer, "turn: meter");
    } else {
        lv_label_set_text(s_footer, "turn: page");
    }

    /* 状态着色：<=WARN 在线白；WARN~OFFLINE 警告黄；再久/无数据 离线灰 */
    lv_color_t col = UI_C_OFFLINE;
    if (got && d.valid && age <= UI_AGE_WARN_S) {
        col = UI_C_TEXT;
    } else if (got && age <= UI_AGE_OFFLINE_S) {
        col = UI_C_WARN;
    }
    for (int p = 0; p < 3; p++) {
        lv_obj_set_style_text_color(s_phase[p], col, 0);
    }
}
