/**
 * @file    mqtt_client.c
 * @brief   MQTT 北向：单表 data（功率换算为 W）+ summary + mdata(retain)
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

#define MQTT_UI_GRACE_US  (8LL * 1000000LL)

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
    int rc = esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
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
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        ESP_LOGI(TAG, "MQTT connected, subscribe %s", s_cmd_topic);
        esp_mqtt_client_subscribe(s_client, s_cmd_topic, 1);
        publish_mdata();
        publish_summary();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        s_disc_us = esp_timer_get_time();
        esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
        ESP_LOGW(TAG, "MQTT disconnected, prefer Wi-Fi until back");
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

    /* GOT_IP 会反复进来。已有客户端交给 esp-mqtt 自动重连，禁止拆建（会闪黄）。 */
    if (s_client != NULL) {
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
    cfg.session.keepalive = 30;
    cfg.network.reconnect_timeout_ms = 3000;
    cfg.network.timeout_ms = 15000;
    cfg.buffer.size = 2048;
    cfg.outbox.limit = 24576;

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
