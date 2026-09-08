/**
 * @file    sys_task.c
 * @brief   Wi-Fi STA 连接、60s 未连上回落 SoftAP、指数退避重连、mDNS
 *          （任务书 5.6 / 5.9）
 * @note    !!! 坑点提醒：Wi-Fi 与 NimBLE 共存，event 回调里禁止长时间阻塞。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-03
 */
#include "sys_task.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_coexist.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "config.h"
#include "tasks.h"
#include "bituo_mqtt.h"

static const char *TAG = "sys";

static bool s_wifi_inited;
static bool s_wifi_started;
static bool s_ap_started;
static bool s_keep_ap;          /* SoftAP 配网期间保持 APSTA，避免保存后踢掉手机 */
static bool s_sta_got_ip;
static bool s_mdns_started;
static bool s_need_ap;
static bool s_stop_ap;
static uint32_t s_backoff_ms = 1000;
static int64_t s_sta_try_us;
static esp_timer_handle_t s_reconnect_timer;

static void build_ap_ssid(char *ssid, size_t cap)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(ssid, cap, "%s%02X%02X%02X",
             SYS_SOFTAP_SSID_PREFIX, mac[3], mac[4], mac[5]);
}

bool sys_wifi_sta_got_ip(void)
{
    return s_sta_got_ip;
}

bool sys_wifi_ap_started(void)
{
    return s_ap_started;
}

void sys_wifi_ip_str(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return;
    }
    buf[0] = '\0';
    esp_netif_ip_info_t ipi;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta != NULL && esp_netif_get_ip_info(sta, &ipi) == ESP_OK && ipi.ip.addr != 0) {
        snprintf(buf, cap, IPSTR, IP2STR(&ipi.ip));
        return;
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap != NULL && esp_netif_get_ip_info(ap, &ipi) == ESP_OK && ipi.ip.addr != 0) {
        snprintf(buf, cap, IPSTR, IP2STR(&ipi.ip));
    }
}

esp_err_t sys_mdns_start(void)
{
    if (s_mdns_started) {
        return ESP_OK;
    }
    esp_err_t rc = mdns_init();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(rc));
        return rc;
    }
    mdns_hostname_set("bituo-dial");
    mdns_instance_name_set("Bituo Dial Energy Gateway");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns_started = true;
    ESP_LOGI(TAG, "mDNS started: http://bituo-dial.local");
    return ESP_OK;
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (config_has_wifi() && !s_sta_got_ip) {
        ESP_LOGI(TAG, "STA reconnect attempt (backoff=%lums)", (unsigned long)s_backoff_ms);
        esp_wifi_connect();
    }
}

static void wifi_sta_keepalive(void)
{
    /* 连上后驱动会把 PS 改回 min-modem（日志 pm type:1），BLE 扫描一占空比
     * TCP/beacon 就被饿死。每次关联后都要再关省电。 */
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) {
        ESP_LOGW(TAG, "wifi_set_ps NONE: %s", esp_err_to_name(ps));
    }
    /* 默认 6s 收不到 AP beacon 就踢 STA。80% BLE 扫描下很容易误踢。 */
    esp_err_t ina = esp_wifi_set_inactive_time(WIFI_IF_STA, 20);
    if (ina != ESP_OK) {
        ESP_LOGW(TAG, "wifi inactive_time: %s", esp_err_to_name(ina));
    }
}

static void wifi_stop_softap_for_sta(void)
{
    s_keep_ap = false;
    s_need_ap = false;
    s_stop_ap = true;
}

static const char *wifi_disc_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:        return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:         return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:       return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY:      return "ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED:         return "NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED:        return "NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE:        return "ASSOC_LEAVE";
    case WIFI_REASON_BEACON_TIMEOUT:     return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:          return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:         return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:  return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:    return "CONNECTION_FAIL";
    default:                             return "OTHER";
    }
}

static void apply_sta_config(void)
{
    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    strncpy((char *)sta.sta.ssid, g_wifi_cred.ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, g_wifi_cred.pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &sta));
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            wifi_sta_keepalive();
            if (config_has_wifi()) {
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            wifi_sta_keepalive();
            ESP_LOGI(TAG, "STA associated, PS_NONE + inactive=20s");
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
            uint8_t reason = ev != NULL ? ev->reason : 0;
            s_sta_got_ip = false;
            if (g_sys_events != NULL) {
                xEventGroupClearBits(g_sys_events, SYS_EVENT_WIFI_CONNECTED);
            }
            ESP_LOGW(TAG, "STA disconnected reason=%u (%s) rssi=%d",
                     (unsigned)reason, wifi_disc_reason_str(reason),
                     ev != NULL ? (int)ev->rssi : 0);

            /* 60s SoftAP 回落从「本次掉线」起算，不是从开机第一次 connect 起算。
             * 以前 GOT_IP 不清 s_sta_try_us，上电 1 分钟后第一次 beacon 丢失
             * 就会开 SoftAP(ch1)，APSTA 把 STA 从路由器信道拽走，只能复位才回来。 */
            if (s_sta_try_us == 0) {
                s_sta_try_us = esp_timer_get_time();
            }
            if (!s_ap_started &&
                (esp_timer_get_time() - s_sta_try_us) >= 60LL * 1000000LL) {
                s_need_ap = true;
            }

            if (config_has_wifi()) {
                uint32_t delay;
                if (reason == WIFI_REASON_BEACON_TIMEOUT ||
                    reason == WIFI_REASON_NO_AP_FOUND) {
                    delay = 1000;
                    s_backoff_ms = 1000;
                } else {
                    delay = s_backoff_ms < 1000 ? 1000 : s_backoff_ms;
                    uint32_t next = s_backoff_ms * 2;
                    if (next > 60000) {
                        next = 60000;
                    }
                    s_backoff_ms = next;
                }
                if (s_reconnect_timer != NULL) {
                    esp_timer_stop(s_reconnect_timer);
                    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000ULL);
                }
            }
        } else if (id == WIFI_EVENT_AP_START) {
            s_ap_started = true;
            ESP_LOGI(TAG, "SoftAP started, join SSID prefix %s pass %s IP %s",
                     SYS_SOFTAP_SSID_PREFIX, SYS_SOFTAP_PASSWORD, SYS_SOFTAP_IP);
        } else if (id == WIFI_EVENT_AP_STOP) {
            s_ap_started = false;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_sta_got_ip = true;
        s_backoff_ms = 1000;
        s_sta_try_us = 0;
        if (s_reconnect_timer != NULL) {
            esp_timer_stop(s_reconnect_timer);
        }
        wifi_sta_keepalive();
        wifi_stop_softap_for_sta();
        ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        if (g_sys_events != NULL) {
            xEventGroupSetBits(g_sys_events, SYS_EVENT_WIFI_CONNECTED);
        }
        sys_mdns_start();
        mqtt_client_start();
    }
}

static esp_err_t wifi_ensure_init(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }

    /* TCP 靠 WIFI_PS_NONE 保活；扫表是主业，射频让给蓝牙，减少变灰。 */
    esp_coex_preference_set(ESP_COEX_PREFER_BT);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t rc = esp_wifi_init(&cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(rc));
        return rc;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    const esp_timer_create_args_t targs = {
        .callback = &reconnect_timer_cb,
        .name = "sta_reconn",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    s_wifi_inited = true;
    return ESP_OK;
}

static esp_err_t wifi_ensure_started(wifi_mode_t mode)
{
    esp_err_t rc = wifi_ensure_init();
    if (rc != ESP_OK) {
        return rc;
    }
    rc = esp_wifi_set_mode(mode);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "set_mode %d: %s", (int)mode, esp_err_to_name(rc));
        return rc;
    }
    if (!s_wifi_started) {
        rc = esp_wifi_start();
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(rc));
            return rc;
        }
        s_wifi_started = true;
        wifi_sta_keepalive();
    }
    return ESP_OK;
}

esp_err_t sys_softap_start(void)
{
    char ssid[32];
    build_ap_ssid(ssid, sizeof(ssid));

    wifi_mode_t mode = config_has_wifi() ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    esp_err_t rc = wifi_ensure_started(mode);
    if (rc != ESP_OK) {
        return rc;
    }

    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", SYS_SOFTAP_PASSWORD);
    ap.ap.ssid_len = (uint8_t)strlen(ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    rc = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "AP set_config: %s", esp_err_to_name(rc));
        return rc;
    }

    /* 若刚才只是 STA 模式，切到 APSTA 后需要再 start 已做过；set_mode 即可 */
    if (s_wifi_started) {
        esp_wifi_set_mode(mode);
        esp_wifi_set_config(WIFI_IF_AP, &ap);
    }

    s_need_ap = false;
    s_keep_ap = true;
    sys_mdns_start();
    ESP_LOGI(TAG, "SoftAP SSID='%s' pass='%s'", ssid, SYS_SOFTAP_PASSWORD);
    return ESP_OK;
}

esp_err_t sys_wifi_connect(void)
{
    if (!config_has_wifi()) {
        ESP_LOGI(TAG, "no Wi-Fi credentials, SoftAP");
        return sys_softap_start();
    }

    /* 配网中保持 APSTA：先回 HTTP 再连 STA，手机不会立刻掉热点。
     * GOT_IP 后会切回 STA-only，避免 SoftAP 锁信道 1 把 STA 带离路由器。 */
    wifi_mode_t mode = (s_keep_ap || s_ap_started) ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t rc = wifi_ensure_started(mode);
    if (rc != ESP_OK) {
        return rc;
    }
    apply_sta_config();
    s_backoff_ms = 1000;
    s_sta_try_us = esp_timer_get_time();
    s_sta_got_ip = false;
    ESP_LOGI(TAG, "STA connecting ssid='%s'", g_wifi_cred.ssid);
    rc = esp_wifi_connect();
    if (rc != ESP_OK && rc != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(rc));
    }
    return ESP_OK;
}

static void sys_task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "sys_task running on core %d", (int)xPortGetCoreID());

    for (;;) {
        EventBits_t bits = 0;
        if (g_sys_events != NULL) {
            bits = xEventGroupWaitBits(
                       g_sys_events,
                       SYS_EVENT_WIFI_CONFIG_CHANGED | SYS_EVENT_MQTT_CONFIG_CHANGED,
                       pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (bits & SYS_EVENT_WIFI_CONFIG_CHANGED) {
            ESP_LOGI(TAG, "Wi-Fi config changed, delay 1.5s then STA (keep AP)");
            s_keep_ap = true;
            vTaskDelay(pdMS_TO_TICKS(1500));
            sys_wifi_connect();
        }
        if (bits & SYS_EVENT_MQTT_CONFIG_CHANGED) {
            ESP_LOGI(TAG, "MQTT config changed, restart client");
            mqtt_client_start();
        }
        if (s_need_ap && !s_ap_started) {
            ESP_LOGW(TAG, "STA not up in 60s, fallback SoftAP");
            sys_softap_start();
        }
        if (s_stop_ap && s_sta_got_ip) {
            s_stop_ap = false;
            if (s_ap_started) {
                ESP_LOGI(TAG, "STA online, stop SoftAP (leave APSTA)");
                esp_err_t rc = esp_wifi_set_mode(WIFI_MODE_STA);
                if (rc != ESP_OK) {
                    ESP_LOGW(TAG, "set_mode STA: %s", esp_err_to_name(rc));
                }
            }
        }
    }
}

esp_err_t sys_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
                        sys_task_entry, "sys", 4096, NULL, 2, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create sys_task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
