/**
 * @file    cmd_dispatch.c
 * @brief   8 条统一命令分发（任务书 7.3）：GATT / HTTP / MQTT cmd 共用
 * @date    2026-09-03
 */
#include "cmd_dispatch.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "config.h"
#include "meter_store.h"
#include "ble_scanner.h"
#include "tasks.h"
#include "bituo_mqtt.h"

static const char *TAG = "cmd";

static SemaphoreHandle_t s_mu;

static const char *cj_str(cJSON *obj, const char *key, const char *def)
{
    cJSON *j = cJSON_GetObjectItem(obj, key);
    if (j != NULL && cJSON_IsString(j) && j->valuestring != NULL) {
        return j->valuestring;
    }
    return def;
}

/* cJSON_AddNumber 会把 float 打成 223.19999694824219，三表 JSON 过大，
 * HTTP 在 BLE 共存下发不出去。短数字走 raw。 */
static void add_fraw(cJSON *o, const char *key, const char *fmt, double v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, v);
    cJSON_AddRawToObject(o, key, buf);
}

static void mac_to_display(const uint8_t stored[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             stored[5], stored[4], stored[3], stored[2], stored[1], stored[0]);
}

void cmd_get_dial_sn(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return;
    }
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(buf, cap, "DIAL-%02X%02X%02X",
             mac[3] & 0xFF, mac[4] & 0xFF, mac[5] & 0xFF);
}

static int build_resp(char *out, int cap, bool ok, const char *msg,
                      const char *event, cJSON *d)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "msg", (msg != NULL) ? msg : "");
    cJSON_AddStringToObject(root, "event", (event != NULL) ? event : "");
    if (d != NULL) {
        cJSON_AddItemToObject(root, "d", d);
    } else {
        cJSON_AddObjectToObject(root, "d");
    }
    char *s = cJSON_PrintUnformatted(root);
    int len = 0;
    if (s != NULL) {
        len = snprintf(out, cap, "%s", s);
        cJSON_free(s);
    }
    cJSON_Delete(root);
    if (len < 0) {
        len = 0;
    }
    if (len >= cap) {
        len = cap - 1;
    }
    return len;
}

static cJSON *build_info_d(void)
{
    cJSON *d = cJSON_CreateObject();

    char dial_sn[32];
    cmd_get_dial_sn(dial_sn, sizeof(dial_sn));
    cJSON_AddStringToObject(d, "dial_sn", dial_sn);
    cJSON_AddStringToObject(d, "fw_ver", "1.1.0");
    cJSON_AddStringToObject(d, "hw_ver", "M5Dial-r1");

    char ipstr[16] = "";
    bool sta_up = false;
    int rssi = 0;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipi;
    if (sta != NULL && esp_netif_get_ip_info(sta, &ipi) == ESP_OK && ipi.ip.addr != 0) {
        snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&ipi.ip));
        sta_up = true;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }
    } else {
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap != NULL && esp_netif_get_ip_info(ap, &ipi) == ESP_OK && ipi.ip.addr != 0) {
            snprintf(ipstr, sizeof(ipstr), IPSTR, IP2STR(&ipi.ip));
        }
    }
    cJSON_AddStringToObject(d, "ip", ipstr);

    cJSON *wifi = cJSON_CreateObject();
    cJSON_AddStringToObject(wifi, "ssid", g_wifi_cred.ssid);
    cJSON_AddNumberToObject(wifi, "rssi", rssi);
    cJSON_AddBoolToObject(wifi, "connected", sta_up);
    cJSON_AddItemToObject(d, "wifi", wifi);

    cJSON *mqtt = cJSON_CreateObject();
    cJSON_AddStringToObject(mqtt, "host", g_mqtt_cfg.host);
    cJSON_AddNumberToObject(mqtt, "port", g_mqtt_cfg.port);
    cJSON_AddBoolToObject(mqtt, "connected", mqtt_client_is_connected());
    cJSON_AddItemToObject(d, "mqtt", mqtt);

    cJSON *scan = cJSON_CreateObject();
    cJSON_AddBoolToObject(scan, "running", ble_scanner_is_scanning());
    cJSON_AddNumberToObject(scan, "meter_count", g_meter_count);
    cJSON_AddNumberToObject(scan, "online_count", meter_store_online_count());
    cJSON_AddItemToObject(d, "ble_scan", scan);

    cJSON_AddNumberToObject(d, "uptime_s",
                            (uint32_t)(esp_timer_get_time() / 1000000ULL));
    return d;
}

static void restart_delay_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "restart command received, reboot in 500ms");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static int dispatch_obj(cJSON *root, char *out, int cap)
{
    const char *cmd = cj_str(root, "cmd", "");
    cJSON *d = NULL;
    bool ok = false;
    const char *msg = "";

    if (strcmp(cmd, "get_info") == 0) {
        d = build_info_d();
        ok = true;
    } else if (strcmp(cmd, "setwifi") == 0) {
        const char *ssid = cj_str(root, "ssid", "");
        const char *pass = cj_str(root, "pass", "");
        if (ssid[0] == '\0') {
            ok = false;
            msg = "invalid ssid";
        } else {
            ok = (config_save_wifi(ssid, pass) == ESP_OK);
            msg = ok ? "wifi connecting" : "nvs write failed";
            if (ok && g_sys_events != NULL) {
                xEventGroupSetBits(g_sys_events, SYS_EVENT_WIFI_CONFIG_CHANGED);
            }
        }
    } else if (strcmp(cmd, "set_mqtt") == 0) {
        mqtt_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        snprintf(cfg.host, sizeof(cfg.host), "%s", cj_str(root, "host", ""));
        cJSON *pj = cJSON_GetObjectItem(root, "port");
        cfg.port = (pj != NULL && cJSON_IsNumber(pj)) ? (uint16_t)pj->valuedouble : 1883;
        snprintf(cfg.user, sizeof(cfg.user), "%s", cj_str(root, "user", ""));
        snprintf(cfg.pass, sizeof(cfg.pass), "%s", cj_str(root, "pass", ""));
        cJSON *tj = cJSON_GetObjectItem(root, "tls");
        if (tj != NULL && ((cJSON_IsNumber(tj) && tj->valuedouble != 0) ||
                           (cJSON_IsBool(tj) && cJSON_IsTrue(tj)))) {
            cfg.tls = 1u;
        }
        if (cfg.host[0] == '\0') {
            ok = false;
            msg = "invalid host";
        } else {
            ok = (config_save_mqtt(&cfg) == ESP_OK);
            msg = ok ? "mqtt saved" : "nvs write failed";
            if (ok && g_sys_events != NULL) {
                xEventGroupSetBits(g_sys_events, SYS_EVENT_MQTT_CONFIG_CHANGED);
            }
        }
    } else if (strcmp(cmd, "add_meter") == 0) {
        const char *sn    = cj_str(root, "sn", "");
        const char *mac   = cj_str(root, "mac", "");
        const char *key   = cj_str(root, "bcast_key", "");
        const char *label = cj_str(root, "label", "");
        int idx = meter_store_add(sn, mac, key, label);
        if (idx >= 0) {
            ok = true;
            msg = "meter added";
            d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "sn", sn);
            cJSON_AddNumberToObject(d, "index", idx);
            if (g_sys_events != NULL) {
                xEventGroupSetBits(g_sys_events, SYS_EVENT_METER_LIST_CHANGED);
            }
        } else if (idx == -1) {
            ok = false;
            msg = "meter list full (max 16)";
        } else if (idx == -4) {
            ok = false;
            msg = "invalid sn (expect 12 hex)";
        } else if (idx == -5) {
            ok = false;
            msg = "invalid key (expect 32 hex)";
        } else if (idx == -6) {
            ok = false;
            msg = "invalid mac (expect AA:BB:CC:DD:EE:FF)";
        } else {
            ok = false;
            msg = "nvs write failed";
        }
    } else if (strcmp(cmd, "del_meter") == 0) {
        const char *sn = cj_str(root, "sn", "");
        int r = meter_store_del_by_sn(sn);
        if (r == 0) {
            ok = true;
            msg = "meter deleted";
            if (g_sys_events != NULL) {
                xEventGroupSetBits(g_sys_events, SYS_EVENT_METER_LIST_CHANGED);
            }
        } else if (r == -1) {
            ok = false;
            msg = "sn not found";
        } else {
            ok = false;
            msg = "nvs write failed";
        }
    } else if (strcmp(cmd, "list_meters") == 0) {
        d = cJSON_CreateObject();
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < g_meter_count; i++) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "sn",    g_meter_configs[i].sn);
            cJSON_AddStringToObject(m, "label", g_meter_configs[i].label);
            char macs[18];
            mac_to_display(g_meter_configs[i].mac, macs);
            cJSON_AddStringToObject(m, "mac", macs);
            bool online = meter_store_is_online(i);
            cJSON_AddBoolToObject(m, "online", online);
            cJSON_AddItemToArray(arr, m);
        }
        cJSON_AddItemToObject(d, "meters", arr);
        ok = true;
    } else if (strcmp(cmd, "get_meters") == 0) {
        d = cJSON_CreateObject();
        char dial_sn[32];
        cmd_get_dial_sn(dial_sn, sizeof(dial_sn));
        cJSON_AddStringToObject(d, "dial_sn", dial_sn);
        cJSON_AddNumberToObject(d, "time",
                                (uint32_t)(esp_timer_get_time() / 1000000ULL));
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < g_meter_count; i++) {
            meter_data_t data;
            memset(&data, 0, sizeof(data));
            if (g_meter_queues[i] != NULL) {
                xQueuePeek(g_meter_queues[i], &data, 0);
            }
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "sn", data.sn[0] ? data.sn : g_meter_configs[i].sn);
            cJSON_AddStringToObject(m, "label", g_meter_configs[i].label);
            cJSON_AddNumberToObject(m, "rssi",      data.rssi);
            cJSON_AddNumberToObject(m, "counter",   data.counter);
            cJSON_AddNumberToObject(m, "timestamp", data.timestamp);
            add_fraw(m, "voltage_x", "%.1f", data.voltage[0]);
            add_fraw(m, "voltage_y", "%.1f", data.voltage[1]);
            add_fraw(m, "voltage_z", "%.1f", data.voltage[2]);
            add_fraw(m, "current_x", "%.3f", data.current[0]);
            add_fraw(m, "current_y", "%.3f", data.current[1]);
            add_fraw(m, "current_z", "%.3f", data.current[2]);
            /* 内部单位 kW；广播无 PF，overall_power_factor 恒 0，勿当实测 */
            add_fraw(m, "active_power_x", "%.3f", data.active_power[0]);
            add_fraw(m, "active_power_y", "%.3f", data.active_power[1]);
            add_fraw(m, "active_power_z", "%.3f", data.active_power[2]);
            add_fraw(m, "forward_energy_x", "%.2f", data.forward_energy[0]);
            add_fraw(m, "forward_energy_y", "%.2f", data.forward_energy[1]);
            add_fraw(m, "forward_energy_z", "%.2f", data.forward_energy[2]);
            add_fraw(m, "reverse_energy_x", "%.2f", data.reverse_energy[0]);
            add_fraw(m, "reverse_energy_y", "%.2f", data.reverse_energy[1]);
            add_fraw(m, "reverse_energy_z", "%.2f", data.reverse_energy[2]);
            add_fraw(m, "total_active_power", "%.3f", data.total_active_power);
            add_fraw(m, "overall_power_factor", "%.0f", data.overall_power_factor);
            cJSON_AddBoolToObject(m, "valid", data.valid);
            cJSON_AddItemToArray(arr, m);
        }
        cJSON_AddItemToObject(d, "meters", arr);
        ok = true;
    } else if (strcmp(cmd, "restart") == 0) {
        ok = true;
        msg = "restarting";
        xTaskCreate(restart_delay_task, "rst_delay", 2048, NULL, 5, NULL);
    } else {
        ok = false;
        msg = "unknown cmd";
    }

    return build_resp(out, cap, ok, msg, cmd, d);
}

int cmd_dispatch_exec(const char *json, char *out, int cap)
{
    if (out == NULL || cap < 8) {
        return 0;
    }
    out[0] = '\0';
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateMutex();
    }
    if (s_mu != NULL) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }

    cJSON *root = cJSON_Parse(json != NULL ? json : "");
    int len;
    if (root == NULL) {
        len = build_resp(out, cap, false, "invalid json", "unknown", NULL);
    } else {
        len = dispatch_obj(root, out, cap);
        cJSON_Delete(root);
    }

    if (s_mu != NULL) {
        xSemaphoreGive(s_mu);
    }
    return len;
}
