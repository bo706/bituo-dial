/**
 * @file    ui_setup.c
 * @brief   圆屏设置：旋钮选择，短按确认，长按返回
 */
#include "ui_setup.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui_task.h"
#include "config.h"
#include "cmd_dispatch.h"
#include "sys_task.h"
#include "ble_scanner.h"
#include "meter_store.h"

static const char *TAG = "ui_setup";

typedef enum {
    ST_MENU = 0,
    ST_WIFI_LIST,
    ST_WIFI_KEY,
    ST_BLE_LIST,
    ST_BLE_KEY,
    ST_DEL_LIST,
} setup_st_t;

#define MENU_N  6
static const char *s_menu[MENU_N] = {
    "Wi-Fi scan",
    "Add meter",
    "Delete meter",
    "LAN web",
    "Hotspot",
    "Back",
};

static const char s_ascii[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    " !@#$%^&*-_.=+/?";
static const char s_hex[] = "0123456789ABCDEF";

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_line[6];
static lv_obj_t *s_foot;
static bool s_open;
static setup_st_t s_st;
static int s_sel;
static int s_wifi_n;
static char s_wifi_ssid[16][33];
static int s_ble_n;
static ble_nearby_t s_ble[BLE_NEARBY_MAX];
static int s_del_n;
static char s_del_sn[MAX_METERS][13];
static char s_buf[64];
static int s_chr;   /* charset index; last = SAVE */
static char s_pick_ssid[33];
static char s_pick_sn[13];

static void set_lines(const char *a, const char *b, const char *c,
                      const char *d, const char *e, const char *f)
{
    const char *v[6] = { a, b, c, d, e, f };
    for (int i = 0; i < 6; i++) {
        lv_label_set_text(s_line[i], (v[i] != NULL) ? v[i] : "");
        lv_obj_set_style_text_color(s_line[i],
                                    (i == 0) ? UI_C_ACCENT : UI_C_TEXT, 0);
    }
}

static void paint(void)
{
    if (s_scr == NULL) {
        return;
    }
    if (s_st == ST_MENU) {
        lv_label_set_text(s_title, "Setup");
        char rows[MENU_N][40];
        const char *p[6];
        for (int i = 0; i < MENU_N; i++) {
            if (i == 3) {
                snprintf(rows[i], sizeof(rows[i]), "%s%s  [%s]",
                         (i == s_sel) ? "> " : "  ", s_menu[i],
                         config_http_enabled() ? "ON" : "OFF");
            } else {
                snprintf(rows[i], sizeof(rows[i]), "%s%s",
                         (i == s_sel) ? "> " : "  ", s_menu[i]);
            }
            p[i] = rows[i];
        }
        set_lines(p[0], p[1], p[2], p[3], p[4], p[5]);
        lv_label_set_text(s_foot, "click  hold=exit");
        return;
    }
    if (s_st == ST_WIFI_LIST) {
        lv_label_set_text(s_title, "Wi-Fi");
        char rows[6][40];
        for (int i = 0; i < 6; i++) {
            rows[i][0] = '\0';
            int idx = s_sel - 2 + i;
            if (idx >= 0 && idx < s_wifi_n) {
                snprintf(rows[i], sizeof(rows[i]), "%s%s",
                         (idx == s_sel) ? "> " : "  ", s_wifi_ssid[idx]);
            }
        }
        set_lines(rows[0], rows[1], rows[2], rows[3], rows[4], rows[5]);
        lv_label_set_text(s_foot, s_wifi_n ? "click SSID" : "no AP");
        return;
    }
    if (s_st == ST_BLE_LIST) {
        lv_label_set_text(s_title, "Meters");
        char rows[6][40];
        for (int i = 0; i < 6; i++) {
            rows[i][0] = '\0';
            int idx = s_sel - 2 + i;
            if (idx >= 0 && idx < s_ble_n) {
                const char *nm = s_ble[idx].name[0] ? s_ble[idx].name : s_ble[idx].sn;
                snprintf(rows[i], sizeof(rows[i]), "%s%s",
                         (idx == s_sel) ? "> " : "  ", nm);
            }
        }
        set_lines(rows[0], rows[1], rows[2], rows[3], rows[4], rows[5]);
        lv_label_set_text(s_foot, s_ble_n ? "click meter" : "none nearby");
        return;
    }
    if (s_st == ST_DEL_LIST) {
        lv_label_set_text(s_title, "Delete");
        char rows[6][40];
        for (int i = 0; i < 6; i++) {
            rows[i][0] = '\0';
            int idx = s_sel - 2 + i;
            if (idx >= 0 && idx < s_del_n) {
                snprintf(rows[i], sizeof(rows[i]), "%s%s",
                         (idx == s_sel) ? "> " : "  ", s_del_sn[idx]);
            }
        }
        set_lines(rows[0], rows[1], rows[2], rows[3], rows[4], rows[5]);
        lv_label_set_text(s_foot, "click=delete");
        return;
    }
    if (s_st == ST_WIFI_KEY || s_st == ST_BLE_KEY) {
        const char *set = (s_st == ST_BLE_KEY) ? s_hex : s_ascii;
        int nset = (int)strlen(set);
        bool save = (s_chr >= nset);
        char cur[8];
        if (save) {
            snprintf(cur, sizeof(cur), "SAVE");
        } else {
            snprintf(cur, sizeof(cur), "%c", set[s_chr]);
        }
        lv_label_set_text(s_title, (s_st == ST_BLE_KEY) ? "Key hex" : "Password");
        char vis[sizeof(s_buf)];
        if (s_st == ST_WIFI_KEY) {
            int n = (int)strlen(s_buf);
            memset(vis, '*', (size_t)((n < 20) ? n : 20));
            vis[(n < 20) ? n : 20] = '\0';
            if (n == 0) {
                snprintf(vis, sizeof(vis), "(empty)");
            }
        } else {
            snprintf(vis, sizeof(vis), "%s", s_buf[0] ? s_buf : "(empty)");
        }
        set_lines(vis, "rotate char", cur, "click=add", "SAVE at end", "");
        lv_label_set_text(s_foot, "hold=bksp/back");
    }
}

static void run_cmd(const char *json)
{
    static char resp[BITUO_RESP_CAP];
    cmd_dispatch_exec(json, resp, sizeof(resp));
    ESP_LOGI(TAG, "cmd resp %s", resp);
}

static void do_scan_wifi(void)
{
    lv_label_set_text(s_title, "Scanning");
    lv_label_set_text(s_line[0], "Wi-Fi...");
    lv_refr_now(NULL);
    cJSON *aps = sys_wifi_scan_aps();
    s_wifi_n = 0;
    if (aps != NULL) {
        cJSON *it;
        cJSON_ArrayForEach(it, aps) {
            if (s_wifi_n >= 16) {
                break;
            }
            cJSON *js = cJSON_GetObjectItem(it, "ssid");
            if (js != NULL && cJSON_IsString(js) && js->valuestring[0]) {
                snprintf(s_wifi_ssid[s_wifi_n], sizeof(s_wifi_ssid[s_wifi_n]),
                         "%s", js->valuestring);
                s_wifi_n++;
            }
        }
        cJSON_Delete(aps);
    }
    s_sel = 0;
    s_st = ST_WIFI_LIST;
    paint();
}

static void do_scan_ble(void)
{
    lv_label_set_text(s_title, "Scanning");
    lv_label_set_text(s_line[0], "BLE...");
    lv_refr_now(NULL);
    /* 缓存靠被动扫描积累；停 1.2s 让列表刷新 */
    vTaskDelay(pdMS_TO_TICKS(1200));
    s_ble_n = ble_scanner_nearby_copy(s_ble, BLE_NEARBY_MAX);
    s_sel = 0;
    s_st = ST_BLE_LIST;
    paint();
}

static void load_del(void)
{
    s_del_n = 0;
    for (int i = 0; i < g_meter_count && s_del_n < MAX_METERS; i++) {
        snprintf(s_del_sn[s_del_n], sizeof(s_del_sn[s_del_n]), "%s",
                 g_meter_configs[i].sn);
        s_del_n++;
    }
    s_sel = 0;
    s_st = ST_DEL_LIST;
}

esp_err_t ui_setup_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_title, UI_C_ACCENT, 0);
    ui_label_place(s_title, UI_SAFE_TOP);

    int y = UI_SAFE_TOP + 26;
    for (int i = 0; i < 6; i++) {
        s_line[i] = lv_label_create(s_scr);
        lv_obj_set_style_text_font(s_line[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_line[i], UI_C_TEXT, 0);
        ui_label_place(s_line[i], y + i * 20);
        lv_label_set_long_mode(s_line[i], LV_LABEL_LONG_DOT);
    }
    s_foot = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_foot, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_foot, UI_C_DIM, 0);
    ui_label_place(s_foot, UI_FOOT_Y);
    ESP_LOGI(TAG, "setup page created");
    return ESP_OK;
}

bool ui_setup_is_open(void)
{
    return s_open;
}

void ui_setup_open(void)
{
    if (s_scr == NULL) {
        return;
    }
    s_open = true;
    s_st = ST_MENU;
    s_sel = 0;
    s_buf[0] = '\0';
    lv_scr_load(s_scr);
    paint();
}

void ui_setup_close(void)
{
    s_open = false;
}

void ui_setup_rotate(int dir)
{
    if (!s_open || dir == 0) {
        return;
    }
    int n = 1;
    int chr_max = 0;
    if (s_st == ST_MENU) {
        n = MENU_N;
    } else if (s_st == ST_WIFI_LIST) {
        n = s_wifi_n > 0 ? s_wifi_n : 1;
    } else if (s_st == ST_BLE_LIST) {
        n = s_ble_n > 0 ? s_ble_n : 1;
    } else if (s_st == ST_DEL_LIST) {
        n = s_del_n > 0 ? s_del_n : 1;
    } else if (s_st == ST_WIFI_KEY) {
        chr_max = (int)strlen(s_ascii); /* +1 SAVE */
        s_chr += (dir > 0) ? 1 : -1;
        if (s_chr < 0) {
            s_chr = chr_max;
        }
        if (s_chr > chr_max) {
            s_chr = 0;
        }
        paint();
        return;
    } else if (s_st == ST_BLE_KEY) {
        chr_max = (int)strlen(s_hex);
        s_chr += (dir > 0) ? 1 : -1;
        if (s_chr < 0) {
            s_chr = chr_max;
        }
        if (s_chr > chr_max) {
            s_chr = 0;
        }
        paint();
        return;
    }
    s_sel += (dir > 0) ? 1 : -1;
    if (s_sel < 0) {
        s_sel = n - 1;
    }
    if (s_sel >= n) {
        s_sel = 0;
    }
    paint();
}

void ui_setup_short_press(void)
{
    if (!s_open) {
        return;
    }
    if (s_st == ST_MENU) {
        if (s_sel == 0) {
            do_scan_wifi();
        } else if (s_sel == 1) {
            do_scan_ble();
        } else if (s_sel == 2) {
            load_del();
            paint();
        } else if (s_sel == 3) {
            uint8_t en = config_http_enabled() ? 0 : 1;
            char js[48];
            snprintf(js, sizeof(js), "{\"cmd\":\"set_http\",\"enable\":%u}", (unsigned)en);
            run_cmd(js);
            paint();
        } else if (s_sel == 4) {
            sys_softap_start();
            lv_label_set_text(s_line[5], "hotspot on");
        } else {
            ui_setup_close();
        }
        return;
    }
    if (s_st == ST_WIFI_LIST && s_wifi_n > 0 && s_sel < s_wifi_n) {
        snprintf(s_pick_ssid, sizeof(s_pick_ssid), "%s", s_wifi_ssid[s_sel]);
        s_buf[0] = '\0';
        s_chr = 0;
        s_st = ST_WIFI_KEY;
        paint();
        return;
    }
    if (s_st == ST_BLE_LIST && s_ble_n > 0 && s_sel < s_ble_n) {
        if (s_ble[s_sel].sn[0] == '\0') {
            lv_label_set_text(s_foot, "no SN; use web");
            return;
        }
        snprintf(s_pick_sn, sizeof(s_pick_sn), "%s", s_ble[s_sel].sn);
        s_buf[0] = '\0';
        s_chr = 0;
        s_st = ST_BLE_KEY;
        paint();
        return;
    }
    if (s_st == ST_DEL_LIST && s_del_n > 0 && s_sel < s_del_n) {
        char js[80];
        snprintf(js, sizeof(js), "{\"cmd\":\"del_meter\",\"sn\":\"%s\"}", s_del_sn[s_sel]);
        run_cmd(js);
        load_del();
        if (s_del_n == 0) {
            s_st = ST_MENU;
            s_sel = 0;
        }
        paint();
        return;
    }
    if (s_st == ST_WIFI_KEY) {
        int nset = (int)strlen(s_ascii);
        if (s_chr >= nset) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "cmd", "setwifi");
            cJSON_AddStringToObject(o, "ssid", s_pick_ssid);
            cJSON_AddStringToObject(o, "pass", s_buf);
            char *js = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            if (js != NULL) {
                run_cmd(js);
                cJSON_free(js);
            }
            s_st = ST_MENU;
            s_sel = 0;
            paint();
            return;
        }
        size_t n = strlen(s_buf);
        if (n + 1 < sizeof(s_buf)) {
            s_buf[n] = s_ascii[s_chr];
            s_buf[n + 1] = '\0';
        }
        paint();
        return;
    }
    if (s_st == ST_BLE_KEY) {
        int nset = (int)strlen(s_hex);
        if (s_chr >= nset) {
            if (s_pick_sn[0] == '\0' || strlen(s_buf) != 32) {
                lv_label_set_text(s_foot, "need SN+32 hex");
                return;
            }
            char js[160];
            snprintf(js, sizeof(js),
                     "{\"cmd\":\"add_meter\",\"sn\":\"%s\",\"bcast_key\":\"%s\"}",
                     s_pick_sn, s_buf);
            run_cmd(js);
            s_st = ST_MENU;
            s_sel = 0;
            paint();
            return;
        }
        size_t n = strlen(s_buf);
        if (n < 32 && n + 1 < sizeof(s_buf)) {
            s_buf[n] = s_hex[s_chr];
            s_buf[n + 1] = '\0';
        }
        paint();
    }
}

bool ui_setup_long_press(void)
{
    if (!s_open) {
        return false;
    }
    if (s_st == ST_WIFI_KEY || s_st == ST_BLE_KEY) {
        size_t n = strlen(s_buf);
        if (n > 0) {
            s_buf[n - 1] = '\0';
            paint();
            return true;
        }
        s_st = (s_st == ST_WIFI_KEY) ? ST_WIFI_LIST : ST_BLE_LIST;
        s_sel = 0;
        paint();
        return true;
    }
    if (s_st != ST_MENU) {
        s_st = ST_MENU;
        s_sel = 0;
        paint();
        return true;
    }
    ui_setup_close();
    return true;
}

void ui_setup_refresh(void)
{
    if (s_open) {
        paint();
    }
}
