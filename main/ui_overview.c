/**
 * @file    ui_overview.c
 * @brief   页面1：列表概览（任务书 5.5.1）
 *          每行：#序号 + 名称 + 三相有效相平均电压 + 总功率；整行颜色表示
 *          在线(绿/白)/警告(黄)/离线(灰)，选中行蓝底高亮。旋钮移动
 *          选中行并滚动 3 行窗口（最多 16 表）；ui_task 短按进入该表详情。
 * @note    仅 ui_task 调用；数据来自 g_meter_queues 深度1队列（xQueuePeek，
 *          不取走）。实时数据字段中仅电压已校准，电流/功率待数据字典。
 * @version LVGL v8.3 / ESP-IDF v5.2.3
 * @date    2026-09-01  @modified 2026-09-03 Phase3
 */
#include "ui_overview.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "ui_task.h"        /* 圆屏尺寸与统一配色 */
#include "meter_store.h"

static const char *TAG = "ui_overview";

/* 选中蓝条固定宽度，必须小于圆在该 y 处的弦长，否则左右会被裁 */
#define UI_ROW_W            148
#define UI_ROW_H_TALL       40

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_title  = NULL;
static lv_obj_t *s_footer = NULL;
static lv_obj_t *s_row_box[UI_OVERVIEW_MAX_ROWS];
static lv_obj_t *s_rows[UI_OVERVIEW_MAX_ROWS];
static int       s_sel    = 0;
static int       s_win    = 0;   /* 窗口第一条可见表下标 */

/* 取一行要显示的名称：优先 label，为空回退 SN */
static const char *row_name(int i, char *buf, size_t n)
{
    const meter_config_t *cfg = &g_meter_configs[i];
    if (cfg->label[0] != '\0') {
        snprintf(buf, n, "%s", cfg->label);
    } else {
        snprintf(buf, n, "%s", cfg->sn);
    }
    return buf;
}

esp_err_t ui_overview_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_title, UI_C_ACCENT, 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_16, 0);
    ui_label_place(s_title, UI_SAFE_TOP);
    lv_label_set_text(s_title, "Meters");

    for (int i = 0; i < UI_OVERVIEW_MAX_ROWS; i++) {
        lv_obj_t *box = lv_obj_create(s_screen);
        s_row_box[i] = box;
        lv_obj_set_size(box, UI_ROW_W, UI_ROW_H_TALL);
        lv_obj_align(box, LV_ALIGN_TOP_MID, 0, UI_SAFE_TOP + UI_TITLE_H);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_set_style_radius(box, 8, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *r = lv_label_create(box);
        s_rows[i] = r;
        lv_obj_set_width(r, UI_ROW_W);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(r, UI_C_DIM, 0);
        lv_label_set_long_mode(r, LV_LABEL_LONG_WRAP);
        lv_obj_center(r);
    }

    s_footer = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_footer, UI_C_DIM, 0);
    lv_obj_set_style_text_font(s_footer, &lv_font_montserrat_14, 0);
    ui_label_place(s_footer, UI_FOOT_Y);
    lv_label_set_text(s_footer, "turn: page");

    ESP_LOGI(TAG, "overview page created");
    return ESP_OK;
}

lv_obj_t *ui_overview_screen(void)
{
    return s_screen;
}

void ui_overview_select_delta(int d)
{
    int n = g_meter_count;
    if (n <= 0) {
        s_sel = 0;
        s_win = 0;
        return;
    }
    s_sel += d;
    if (s_sel < 0) {
        s_sel = n - 1;
    } else if (s_sel >= n) {
        s_sel = 0;
    }
    int vis = n < UI_OVERVIEW_VISIBLE ? n : UI_OVERVIEW_VISIBLE;
    if (s_sel < s_win) {
        s_win = s_sel;
    } else if (s_sel >= s_win + vis) {
        s_win = s_sel - vis + 1;
    }
    int win_max = n > vis ? n - vis : 0;
    if (s_win < 0) {
        s_win = 0;
    } else if (s_win > win_max) {
        s_win = win_max;
    }
}

int ui_overview_selected(void)
{
    return s_sel;
}

void ui_overview_refresh(void)
{
    if (s_screen == NULL) {
        return;
    }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    int n = g_meter_count;
    if (s_sel >= n) {
        s_sel = (n > 0) ? n - 1 : 0;
    }
    int vis = n < UI_OVERVIEW_VISIBLE ? n : UI_OVERVIEW_VISIBLE;
    if (s_sel < s_win) {
        s_win = s_sel;
    } else if (s_sel >= s_win + vis) {
        s_win = s_sel - vis + 1;
    }
    int win_max = n > vis ? n - vis : 0;
    if (s_win < 0) {
        s_win = 0;
    } else if (s_win > win_max) {
        s_win = win_max;
    }

    lv_label_set_text_fmt(s_title, "Meters %d/%d",
                          meter_store_online_count(), n);
    if (n > 1) {
        lv_label_set_text(s_footer, "turn: list");
    } else {
        lv_label_set_text(s_footer, "turn: page");
    }

    const int row_h = UI_ROW_H_TALL;
    const int gap   = 4;
    int row_y0 = UI_SAFE_TOP + UI_TITLE_H;
    if (vis > 0) {
        int block_h = vis * row_h + (vis - 1) * gap;
        row_y0 = (UI_SCREEN_SIZE - block_h) / 2;
    }

    for (int i = 0; i < UI_OVERVIEW_MAX_ROWS; i++) {
        lv_obj_t *box = s_row_box[i];
        lv_obj_t *r = s_rows[i];
        if (i >= vis) {
            lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int idx = s_win + i;
        if (idx < 0 || idx >= n) {
            lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(box, row_h);
        lv_obj_align(box, LV_ALIGN_TOP_MID, 0, row_y0 + i * (row_h + gap));

        meter_data_t d;
        bool got = (xQueuePeek(g_meter_queues[idx], &d, 0) == pdTRUE);
        uint32_t age = got ? (now - d.timestamp) : 0xFFFFFFFFu;

        float vsum = 0.0f;
        int   vn   = 0;
        if (got) {
            for (int p = 0; p < 3; p++) {
                if (d.voltage[p] > 1.0f) {
                    vsum += d.voltage[p];
                    vn++;
                }
            }
        }
        float vavg = vn > 0 ? vsum / vn : 0.0f;

        char name[METER_LABEL_LEN];
        row_name(idx, name, sizeof(name));

        char txt[64];
        snprintf(txt, sizeof(txt), "%s\n%.1fV  %.2fkW",
                 name, vavg, got ? d.total_active_power : 0.0f);
        lv_label_set_text(r, txt);
        lv_obj_set_width(r, UI_ROW_W);
        lv_obj_center(r);

        lv_color_t col = UI_C_OFFLINE;
        if (got && d.valid && age <= UI_AGE_WARN_S) {
            col = UI_C_TEXT;
        } else if (got && age <= UI_AGE_OFFLINE_S) {
            col = UI_C_WARN;
        }
        lv_obj_set_style_text_color(r, col, 0);
        if (idx == s_sel) {
            lv_obj_set_style_bg_color(box, UI_C_SELECT, 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_60, 0);
        } else {
            lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        }
    }

    if (n == 0) {
        lv_obj_clear_flag(s_row_box[0], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_rows[0], UI_C_DIM, 0);
        lv_obj_set_style_bg_opa(s_row_box[0], LV_OPA_TRANSP, 0);
        lv_label_set_text(s_rows[0], "no meter");
        lv_obj_align(s_row_box[0], LV_ALIGN_CENTER, 0, 0);
    }
}
