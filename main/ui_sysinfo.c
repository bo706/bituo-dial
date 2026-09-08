/**
 * @file    ui_sysinfo.c
 * @brief   页面3：系统状态（任务书 5.5.1）
 *          Wi-Fi 状态/SSID/IP、MQTT 配置状态、BLE 扫描状态、在线/总表数、
 *          固件版本、运行时长与剩余堆；长按进入 SoftAP 配网（Phase4 完整）。
 * @note    仅 ui_task 调用。Wi-Fi/MQTT 完整联网在 Phase4，当前未配置时如实
 *          显示 not set / idle，不臆造连接状态。
 * @version LVGL v8.3 / ESP-IDF v5.2.3
 * @date    2026-09-01  @modified 2026-09-03 Phase3
 */
#include "ui_sysinfo.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "ui_task.h"
#include "config.h"
#include "meter_store.h"
#include "ble_scanner.h"
#include "sys_task.h"
#include "bituo_mqtt.h"

static const char *TAG = "ui_sysinfo";

#define UI_FW_VER           "v1.1.0"

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_title;
static lv_obj_t *s_wifi;
static lv_obj_t *s_ip;
static lv_obj_t *s_mqtt;
static lv_obj_t *s_ble;
static lv_obj_t *s_meters;
static lv_obj_t *s_fw;
static lv_obj_t *s_run;
static lv_obj_t *s_footer;

esp_err_t ui_sysinfo_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_title, UI_C_ACCENT, 0);
    ui_label_place(s_title, UI_SAFE_TOP);
    lv_label_set_text(s_title, "System");

    int y = UI_SAFE_TOP + 26;
    const int dy = 20;
    s_wifi   = lv_label_create(s_screen);
    s_ip     = lv_label_create(s_screen);
    s_mqtt   = lv_label_create(s_screen);
    s_ble    = lv_label_create(s_screen);
    s_meters = lv_label_create(s_screen);
    s_fw     = lv_label_create(s_screen);
    s_run    = lv_label_create(s_screen);
    lv_obj_t *labs[] = { s_wifi, s_ip, s_mqtt, s_ble, s_meters, s_fw, s_run };
    for (size_t i = 0; i < sizeof(labs) / sizeof(labs[0]); i++) {
        lv_obj_set_style_text_font(labs[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(labs[i], UI_C_TEXT, 0);
        ui_label_place(labs[i], y + (int)i * dy);
        lv_label_set_long_mode(labs[i], LV_LABEL_LONG_DOT);
    }

    s_footer = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_footer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_footer, UI_C_DIM, 0);
    ui_label_place(s_footer, UI_FOOT_Y);
    lv_label_set_text(s_footer, "hold: SoftAP");

    ESP_LOGI(TAG, "sysinfo page created");
    return ESP_OK;
}

lv_obj_t *ui_sysinfo_screen(void)
{
    return s_screen;
}

void ui_sysinfo_long_press(void)
{
    /* 任务书 5.5.1：系统状态页长按侧键进入 SoftAP 配网。
     * sys_softap_start 在 Phase4 完整落地；此处先调用（当前为占位会打日志）。 */
    ESP_LOGI(TAG, "long press -> request SoftAP provisioning");
    sys_softap_start();
}

void ui_sysinfo_refresh(void)
{
    if (s_screen == NULL) {
        return;
    }

    /* ── Wi-Fi：STA 拿到 IP 为已连接；否则若 SoftAP 已开则显示热点 ── */
    bool got_ip = sys_wifi_sta_got_ip();
    bool ap_on = sys_wifi_ap_started();
    char ipbuf[16] = "";
    sys_wifi_ip_str(ipbuf, sizeof(ipbuf));

    if (got_ip) {
        lv_label_set_text_fmt(s_wifi, "WiFi: %s", g_wifi_cred.ssid);
        lv_obj_set_style_text_color(s_wifi, UI_C_ONLINE, 0);
    } else if (ap_on) {
        lv_label_set_text(s_wifi, "WiFi: SoftAP");
        lv_obj_set_style_text_color(s_wifi, UI_C_WARN, 0);
    } else if (g_wifi_cred.ssid[0] == '\0') {
        lv_label_set_text(s_wifi, "WiFi: not set");
        lv_obj_set_style_text_color(s_wifi, UI_C_WARN, 0);
    } else {
        lv_label_set_text_fmt(s_wifi, "WiFi: %s (link..)", g_wifi_cred.ssid);
        lv_obj_set_style_text_color(s_wifi, UI_C_WARN, 0);
    }

    if (ipbuf[0] != '\0') {
        lv_label_set_text_fmt(s_ip, "IP: %s", ipbuf);
    } else {
        lv_label_set_text(s_ip, "IP: --");
    }

    /* ── MQTT：未配置 / 等待 / 已连接 ── */
    if (g_mqtt_cfg.host[0] == '\0') {
        lv_label_set_text(s_mqtt, "MQTT: not set");
        lv_obj_set_style_text_color(s_mqtt, UI_C_DIM, 0);
    } else if (mqtt_client_is_connected()) {
        lv_label_set_text_fmt(s_mqtt, "MQTT: %s", g_mqtt_cfg.host);
        lv_obj_set_style_text_color(s_mqtt, UI_C_ONLINE, 0);
    } else {
        lv_label_set_text_fmt(s_mqtt, "MQTT: %s (wait)", g_mqtt_cfg.host);
        lv_obj_set_style_text_color(s_mqtt, UI_C_WARN, 0);
    }

    /* ── BLE 扫描 + 在线/总表数 ── */
    lv_label_set_text_fmt(s_ble, "BLE scan: %s",
                          ble_scanner_is_scanning() ? "ON" : "OFF");
    lv_label_set_text_fmt(s_meters, "Meters online: %d/%d",
                          meter_store_online_count(), g_meter_count);

    lv_label_set_text_fmt(s_fw, "FW %s   heap %luKB", UI_FW_VER,
                          (unsigned long)(esp_get_free_heap_size() / 1024));

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    lv_label_set_text_fmt(s_run, "uptime %02lu:%02lu:%02lu",
                          (unsigned long)(up / 3600),
                          (unsigned long)((up / 60) % 60),
                          (unsigned long)(up % 60));
}
