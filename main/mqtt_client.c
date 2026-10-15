/**
 * @file    mqtt_client.c
 * @brief   MQTT 北向：单表 data（功率 W，QoS0+retain）+ summary + mdata(QoS1 retain)
 *          订阅 cmd、响应 cdata。Topic 用 DialSN/MeterSN，禁止 MAC。
 * @note    广播无 PF，MQTT payload 不带 PowerFactor* 字段。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-03
 */
#include "bituo_mqtt.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "mqtt_client.h"   /* IDF esp-mqtt；本模块对外头是 bituo_mqtt.h */
#include "esp_crt_bundle.h"
#include "esp_coexist.h"

#include "config.h"
#include "meter_store.h"
#include "cmd_dispatch.h"
#include "sys_task.h"
#include "ble_scanner.h"
#include "tasks.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static int64_t s_disc_us;
static char s_uri[160];
static char s_client_id[32];
static char s_cmd_topic[80];
static char s_cdata_topic[80];
static char s_dial_sn[32];
static esp_timer_handle_t s_coex_tmr;

#define MQTT_UI_GRACE_US           (8LL * 1000000LL)
#define MQTT_SCAN_SETTLE_US        (6LL * 1000000LL)
#define MQTT_SCAN_HOLD_MAX_US      (12LL * 1000000LL)
#define MQTT_WATCHDOG_US           (30LL * 1000000LL)

static int64_t s_watchdog_kick_us;
static int64_t s_scan_resume_at_us;
static int64_t s_hold_origin_us;

static void mqtt_radio_steady(void)
{
    /* 16 表时 BALANCE 仍会让扫描抢走 TCP。稳定后 Wi-Fi 优先，扫描用较短窗口。 */
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    ble_scanner_set_wifi_boost(false);
    sys_wifi_refresh_ps();
}

static void mqtt_scan_resume_cb(void *arg);

static void mqtt_arm_scan_resume(int64_t resume_at_us)
{
    int64_t now = esp_timer_get_time();
    s_scan_resume_at_us = resume_at_us;
    if (s_coex_tmr == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = &mqtt_scan_resume_cb,
            .name = "mqtt_coex",
        };
        if (esp_timer_create(&targs, &s_coex_tmr) != ESP_OK) {
            ESP_LOGW(TAG, "coex timer create failed");
            s_scan_resume_at_us = 0;
            return;
        }
    }
    esp_timer_stop(s_coex_tmr);
    int64_t delay = resume_at_us - now;
    if (delay < 200000LL) {
        delay = 200000LL;
    }
    esp_timer_start_once(s_coex_tmr, delay);
}

static void mqtt_scan_resume_cb(void *arg)
{
    (void)arg;
    s_scan_resume_at_us = 0;
    s_hold_origin_us = 0;
    mqtt_radio_steady();
    ble_scanner_set_mqtt_hold(false);
    ESP_LOGI(TAG, "scan resume mqtt=%d", s_connected ? 1 : 0);
}

/* prefer_later：刚连上时可以把开扫时间推到「现在 + 6s」，但整段暂停不超过 12s。
 * 断线传 false，重复断线不能把开扫时间再往后推，避免扫描一直停。 */
static void mqtt_hold_scan(int64_t resume_at, bool prefer_later)
{
    int64_t now = esp_timer_get_time();
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    ble_scanner_set_wifi_boost(false);
    sys_wifi_refresh_ps();
    ble_scanner_set_mqtt_hold(true);
    if (s_hold_origin_us == 0) {
        s_hold_origin_us = now;
    }
    int64_t cap = s_hold_origin_us + MQTT_SCAN_HOLD_MAX_US;
    if (now >= cap) {
        if (s_scan_resume_at_us == 0) {
            mqtt_arm_scan_resume(now + 200000LL);
        }
        return;
    }
    if (resume_at > cap) {
        resume_at = cap;
    }
    if (resume_at < now + 200000LL) {
        resume_at = now + 200000LL;
    }
    if (resume_at > cap) {
        resume_at = cap;
    }
    if (s_scan_resume_at_us != 0) {
        if (!prefer_later && resume_at >= s_scan_resume_at_us) {
            return;
        }
        if (prefer_later && resume_at <= s_scan_resume_at_us) {
            return;
        }
    }
    mqtt_arm_scan_resume(resume_at);
}

bool mqtt_client_is_connected(void)
{
    return s_connected;
}

bool mqtt_client_ui_connected(void)
{
    if (s_connected) {
        return true;
    }
    if (s_disc_us == 0) {
        return false;
    }
    return (esp_timer_get_time() - s_disc_us) < MQTT_UI_GRACE_US;
}

static void add_fstr(cJSON *o, const char *key, const char *fmt, double v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, v);
    cJSON_AddStringToObject(o, key, buf);
}


static void publish_mdata(void)
{
    if (s_client == NULL) {
        return;
    }
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "dial_sn", s_dial_sn);
    cJSON_AddStringToObject(d, "fw_ver", "1.1.4");
    char ip[16] = "";
    sys_wifi_ip_str(ip, sizeof(ip));
    cJSON_AddStringToObject(d, "ip", ip);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_meter_count; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "sn", g_meter_configs[i].sn);
        cJSON_AddStringToObject(m, "label", g_meter_configs[i].label);
        cJSON_AddItemToArray(arr, m);
    }
    cJSON_AddItemToObject(d, "meters", arr);
    char *s = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    if (s == NULL) {
        return;
    }
    char topic[96];
    snprintf(topic, sizeof(topic), "bituo-dial/%s/mdata", s_dial_sn);
    esp_mqtt_client_publish(s_client, topic, s, 0, 1, 1);
    cJSON_free(s);
}

static void publish_one_meter(int i)
{
    if (!s_connected || s_client == NULL || i < 0 || i >= g_meter_count) {
        return;
    }
    meter_data_t data;
    memset(&data, 0, sizeof(data));
    if (g_meter_queues[i] != NULL) {
        xQueuePeek(g_meter_queues[i], &data, 0);
    }
    char payload[768];
    if (meter_store_native_json_print(i, &data, s_dial_sn, payload, sizeof(payload)) < 0) {
        ESP_LOGW(TAG, "meter %d json too big", i);
        return;
    }
    char topic[128];
    const char *sn = data.sn[0] ? data.sn : g_meter_configs[i].sn;
    snprintf(topic, sizeof(topic), "bituo-dial/%s/meters/%s/data", s_dial_sn, sn);
    /* QoS 0：满 16 表时 QoS1 PUBACK 会被 BLE 扫描饿死。Retain 仍留给新订阅。 */
    int rc = esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
    if (rc < 0) {
        ESP_LOGW(TAG, "publish meter %d failed rc=%d", i, rc);
    }
}

static void publish_summary(void)
{
    if (!s_connected || s_client == NULL) {
        return;
    }
    cJSON *sum = cJSON_CreateObject();
    if (sum == NULL) {
        return;
    }
    cJSON_AddStringToObject(sum, "dial_sn", s_dial_sn);
    cJSON_AddNumberToObject(sum, "time",
                            (uint32_t)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddNumberToObject(sum, "meter_count", g_meter_count);
    cJSON_AddNumberToObject(sum, "online_count", meter_store_online_count());
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < g_meter_count; i++) {
        meter_data_t data;
        memset(&data, 0, sizeof(data));
        if (g_meter_queues[i] != NULL) {
            xQueuePeek(g_meter_queues[i], &data, 0);
        }
        cJSON *row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "sn", g_meter_configs[i].sn);
        cJSON_AddStringToObject(row, "label", g_meter_configs[i].label);
        cJSON_AddBoolToObject(row, "online", data.valid);
        add_fstr(row, "TotalActivePower", "%.1f", data.total_active_power * 1000.0);
        cJSON_AddNumberToObject(row, "_rssi", data.rssi);
        cJSON_AddItemToArray(arr, row);
    }
    cJSON_AddItemToObject(sum, "meters", arr);
    char *ss = cJSON_PrintUnformatted(sum);
    cJSON_Delete(sum);
    if (ss == NULL) {
        return;
    }
    char topic[96];
    snprintf(topic, sizeof(topic), "bituo-dial/%s/summary", s_dial_sn);
    int rc = esp_mqtt_client_publish(s_client, topic, ss, 0, 0, 1);
    cJSON_free(ss);
    if (rc < 0) {
        ESP_LOGW(TAG, "publish summary failed rc=%d", rc);
    }
}

void mqtt_publish_cycle(void)
{
    if (!s_connected || s_client == NULL) {
        return;
    }
    for (int i = 0; i < g_meter_count; i++) {
        publish_one_meter(i);
    }
    publish_summary();
}

static char s_cmd_req[BITUO_RESP_CAP];
static char s_cmd_resp[BITUO_RESP_CAP];

static void handle_cmd_payload(const char *data, int len)
{
    if (data == NULL || len <= 0 || len >= BITUO_RESP_CAP) {
        return;
    }
    memcpy(s_cmd_req, data, (size_t)len);
    s_cmd_req[len] = '\0';
    cmd_dispatch_exec(s_cmd_req, s_cmd_resp, sizeof(s_cmd_resp));
    if (s_client != NULL) {
        esp_mqtt_client_publish(s_client, s_cdata_topic, s_cmd_resp, 0, 1, 0);
    }
}

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_disc_us = 0;
        /* 先让这条 TCP 安静几秒再开扫，避免扫描在握手当下把会话挤掉。 */
        mqtt_hold_scan(esp_timer_get_time() + MQTT_SCAN_SETTLE_US, true);
        ESP_LOGI(TAG, "MQTT connected, subscribe %s", s_cmd_topic);
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
        publish_mdata();
        publish_summary();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        s_disc_us = esp_timer_get_time();
        mqtt_hold_scan(s_disc_us + MQTT_SCAN_SETTLE_US, false);
        ESP_LOGW(TAG, "MQTT disconnected, pause scan");
        break;
    case MQTT_EVENT_DATA:
        if (ev != NULL && ev->data != NULL) {
            handle_cmd_payload(ev->data, ev->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (ev != NULL && ev->error_handle != NULL) {
            ESP_LOGW(TAG, "MQTT error type=%d sock=%d tls=%s",
                     (int)ev->error_handle->error_type,
                     ev->error_handle->esp_transport_sock_errno,
                     esp_err_to_name(ev->error_handle->esp_tls_last_esp_err));
        } else {
            ESP_LOGW(TAG, "MQTT error");
        }
        break;
    default:
        break;
    }
}

esp_err_t mqtt_client_start(void)
{
    if (g_mqtt_cfg.host[0] == '\0') {
        ESP_LOGI(TAG, "mqtt_client_start: host empty, skip");
        return ESP_OK;
    }
    if (!sys_wifi_sta_got_ip()) {
        ESP_LOGI(TAG, "mqtt_client_start: wait STA IP");
        return ESP_OK;
    }

    cmd_get_dial_sn(s_dial_sn, sizeof(s_dial_sn));
    snprintf(s_client_id, sizeof(s_client_id), "%s", s_dial_sn);
    snprintf(s_cmd_topic, sizeof(s_cmd_topic), "bituo-dial/%s/cmd", s_dial_sn);
    snprintf(s_cdata_topic, sizeof(s_cdata_topic), "bituo-dial/%s/cdata", s_dial_sn);

    if (g_mqtt_cfg.tls) {
        snprintf(s_uri, sizeof(s_uri), "mqtts://%s:%u",
                 g_mqtt_cfg.host, g_mqtt_cfg.port ? g_mqtt_cfg.port : 8883);
    } else {
        snprintf(s_uri, sizeof(s_uri), "mqtt://%s:%u",
                 g_mqtt_cfg.host, g_mqtt_cfg.port ? g_mqtt_cfg.port : 1883);
    }

    /* GOT_IP 会反复进来。已有客户端禁止拆建（会闪黄）；未连上则踢一脚重连。 */
    if (s_client != NULL) {
        if (!s_connected) {
            mqtt_hold_scan(esp_timer_get_time() + MQTT_SCAN_SETTLE_US, false);
            esp_err_t rc = esp_mqtt_client_reconnect(s_client);
            ESP_LOGI(TAG, "mqtt kick reconnect rc=%s", esp_err_to_name(rc));
            return rc;
        }
        ESP_LOGI(TAG, "mqtt already started");
        return ESP_OK;
    }

    esp_mqtt_client_config_t cfg = { 0 };
    cfg.broker.address.uri = s_uri;
    cfg.credentials.client_id = s_client_id;
    if (g_mqtt_cfg.user[0] != '\0') {
        cfg.credentials.username = g_mqtt_cfg.user;
    }
    if (g_mqtt_cfg.pass[0] != '\0') {
        cfg.credentials.authentication.password = g_mqtt_cfg.pass;
    }
    if (g_mqtt_cfg.tls) {
        if (g_mqtt_cfg.insecure) {
            /* 实验室自签：不挂 CA，配合 CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY */
            cfg.broker.verification.crt_bundle_attach = NULL;
            cfg.broker.verification.skip_cert_common_name_check = true;
            ESP_LOGW(TAG, "MQTTS insecure=1 (broker cert not verified)");
        } else {
            cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
            cfg.broker.verification.skip_cert_common_name_check = false;
            ESP_LOGI(TAG, "MQTTS using public CA bundle");
        }
    }
    cfg.session.keepalive = 60;
    cfg.network.reconnect_timeout_ms = 3000;
    cfg.network.timeout_ms = 15000;
    cfg.buffer.size = 2048;
    cfg.outbox.limit = 24576;

    mqtt_hold_scan(esp_timer_get_time() + MQTT_SCAN_SETTLE_US, false);
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_err_t rc = esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "mqtt start uri=%s rc=%s", s_uri, esp_err_to_name(rc));
    return rc;
}

esp_err_t mqtt_client_restart(void)
{
    if (s_coex_tmr != NULL) {
        esp_timer_stop(s_coex_tmr);
    }
    s_scan_resume_at_us = 0;
    s_hold_origin_us = 0;
    mqtt_hold_scan(esp_timer_get_time() + MQTT_SCAN_SETTLE_US, false);
    if (s_client != NULL) {
        s_connected = false;
        s_disc_us = esp_timer_get_time();
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    return mqtt_client_start();
}

static void north_task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "north_task running on core %d", (int)xPortGetCoreID());
    int idx = 0;
    for (;;) {
        int n = g_meter_count;
        if (s_connected && s_client != NULL && n > 0) {
            publish_one_meter(idx);
            idx++;
            if (idx >= n) {
                idx = 0;
                publish_summary();
            }
            int slot_ms = MQTT_SUMMARY_PERIOD_MS / n;
            if (slot_ms < 800) {
                slot_ms = 800;
            }
            vTaskDelay(pdMS_TO_TICKS(slot_ms));
        } else {
            idx = 0;
            int64_t now = esp_timer_get_time();
            /* 客户端自己会在 3 秒后重连。10 秒一次的强制重连会把刚连上的会话掐断。 */
            if (s_client != NULL && sys_wifi_sta_got_ip() &&
                s_disc_us != 0 && (now - s_disc_us) >= MQTT_WATCHDOG_US &&
                (s_watchdog_kick_us == 0 ||
                 (now - s_watchdog_kick_us) >= MQTT_WATCHDOG_US)) {
                s_watchdog_kick_us = now;
                if (s_scan_resume_at_us == 0) {
                    s_hold_origin_us = 0;
                    mqtt_hold_scan(now + MQTT_SCAN_SETTLE_US, true);
                }
                esp_err_t rc = esp_mqtt_client_reconnect(s_client);
                ESP_LOGW(TAG, "mqtt watchdog reconnect rc=%s", esp_err_to_name(rc));
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

esp_err_t north_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
                        north_task_entry, "north", 8192, NULL, 3, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create north_task failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
