/**
 * @file    config.c
 * @brief   NVS 配置持久化：wifi_ssid/pass、mqtt_host/port/user/pass/tls、
 *          dial_label、api_token（任务书 5.10）；电表列表由 meter_store 管理。
 * @note    !!! 坑点提醒（任务书坑4）!!!
 *          NVS 只允许在用户主动改配置时写一次；实时电表数据只能进 RAM 队列，
 *          绝不写 NVS（Flash 擦写寿命约 10 万次，~5Hz 写入几天就坏）。
 *          本文件所有 config_save_*() 仅由 GATT/HTTP 配置命令路径调用。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#include "config.h"

#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "meter_store.h"

static const char *TAG = "config";

/* 全局运行期配置镜像定义（头文件 extern 声明） */
wifi_cred_t g_wifi_cred;
mqtt_cfg_t  g_mqtt_cfg;
char        g_dial_label[CONFIG_LABEL_LEN];
char        g_api_token[CONFIG_TOKEN_LEN];

/* ======================================================================
 * 内部辅助：NVS 字符串读取（先查长度，再分配/截断读入）
 * ====================================================================== */
static esp_err_t nvs_read_str(nvs_handle_t h, const char *key,
                               char *out, size_t out_cap)
{
    if (out_cap == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    size_t len = 0;
    esp_err_t rc = nvs_get_str(h, key, NULL, &len);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;   /* 键不存在视为空配置，不报错 */
    }
    if (rc != ESP_OK) return rc;
    if (len == 0) return ESP_OK;
    if (len > out_cap) {
        /* 存储值比当前缓冲区大（版本变更），截断读入并告警，不崩溃 */
        ESP_LOGW(TAG, "nvs key '%s' stored len=%u > cap=%u, truncated",
                 key, (unsigned)len, (unsigned)out_cap);
        len = out_cap;
    }
    rc = nvs_get_str(h, key, out, &len);
    return rc;
}

/* ======================================================================
 * 载入全部配置到内存镜像
 * ====================================================================== */
esp_err_t config_load(void)
{
    ESP_LOGI(TAG, "config_load: reading NVS namespace '%s'", CONFIG_NVS_NAMESPACE);

    memset(&g_wifi_cred, 0, sizeof(g_wifi_cred));
    memset(&g_mqtt_cfg, 0, sizeof(g_mqtt_cfg));
    memset(g_dial_label, 0, sizeof(g_dial_label));
    memset(g_api_token, 0, sizeof(g_api_token));
    g_mqtt_cfg.port = 1883;   /* MQTT 默认端口 */

    nvs_handle_t h;
    esp_err_t rc = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READONLY, &h);
    if (rc != ESP_OK) {
        /* 首次启动时命名空间尚未创建（nvs_set 第一次写入时才创建），
         * NVS_READONLY 打开返回 ESP_ERR_NVS_NOT_FOUND，属正常状态，
         * 视为空配置用默认值，不报错、不阻断启动。 */
        ESP_LOGW(TAG, "nvs_open '%s': %s, using defaults (first boot or namespace empty)",
                 CONFIG_NVS_NAMESPACE, esp_err_to_name(rc));
        meter_store_load_nvs();
        return ESP_OK;
    }

    (void)nvs_read_str(h, CONFIG_KEY_WIFI_SSID, g_wifi_cred.ssid, CONFIG_WIFI_SSID_LEN);
    (void)nvs_read_str(h, CONFIG_KEY_WIFI_PASS, g_wifi_cred.pass, CONFIG_WIFI_PASS_LEN);
    (void)nvs_read_str(h, CONFIG_KEY_MQTT_HOST, g_mqtt_cfg.host, CONFIG_MQTT_HOST_LEN);
    (void)nvs_read_str(h, CONFIG_KEY_MQTT_USER, g_mqtt_cfg.user, CONFIG_MQTT_USER_LEN);
    (void)nvs_read_str(h, CONFIG_KEY_MQTT_PASS, g_mqtt_cfg.pass, CONFIG_MQTT_PASS_LEN);
    (void)nvs_read_str(h, CONFIG_KEY_LABEL,      g_dial_label,    CONFIG_LABEL_LEN);
    (void)nvs_read_str(h, CONFIG_KEY_TOKEN,      g_api_token,     CONFIG_TOKEN_LEN);

    uint16_t port = 0;
    if (nvs_get_u16(h, CONFIG_KEY_MQTT_PORT, &port) == ESP_OK && port != 0) {
        g_mqtt_cfg.port = port;
    }
    uint8_t tls = 0;
    if (nvs_get_u8(h, CONFIG_KEY_MQTT_TLS, &tls) == ESP_OK) {
        g_mqtt_cfg.tls = tls;
    }

    nvs_close(h);

    ESP_LOGI(TAG, "config_load ok: wifi='%s' mqtt='%s:%u' tls=%u label='%s'",
             g_wifi_cred.ssid[0] ? g_wifi_cred.ssid : "(none)",
             g_mqtt_cfg.host[0] ? g_mqtt_cfg.host : "(none)",
             g_mqtt_cfg.port, g_mqtt_cfg.tls,
             g_dial_label[0] ? g_dial_label : "(default)");

    /* 电表列表 NVS 载入（结构在 meter_store 模块） */
    meter_store_load_nvs();
    return ESP_OK;
}

bool config_has_wifi(void)
{
    return g_wifi_cred.ssid[0] != '\0';
}

/* ======================================================================
 * 写入接口（仅配置命令路径调用）
 * ====================================================================== */
esp_err_t config_save_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (pass == NULL) pass = "";

    nvs_handle_t h;
    esp_err_t rc = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "save_wifi nvs_open: %s", esp_err_to_name(rc));
        return rc;
    }
    rc = nvs_set_str(h, CONFIG_KEY_WIFI_SSID, ssid);
    if (rc == ESP_OK) rc = nvs_set_str(h, CONFIG_KEY_WIFI_PASS, pass);
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "save_wifi write failed: %s", esp_err_to_name(rc));
        return rc;
    }
    /* 更新内存镜像 */
    snprintf(g_wifi_cred.ssid, sizeof(g_wifi_cred.ssid), "%s", ssid);
    snprintf(g_wifi_cred.pass, sizeof(g_wifi_cred.pass), "%s", pass);
    ESP_LOGI(TAG, "wifi saved: ssid='%s'", ssid);
    return ESP_OK;
}

esp_err_t config_save_mqtt(const mqtt_cfg_t *cfg)
{
    if (cfg == NULL || cfg->host[0] == '\0') return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t rc = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (rc != ESP_OK) return rc;
    rc  = nvs_set_str(h, CONFIG_KEY_MQTT_HOST, cfg->host);
    if (rc == ESP_OK) rc = nvs_set_u16(h, CONFIG_KEY_MQTT_PORT, cfg->port ? cfg->port : 1883);
    if (rc == ESP_OK) rc = nvs_set_str(h, CONFIG_KEY_MQTT_USER, cfg->user);
    if (rc == ESP_OK) rc = nvs_set_str(h, CONFIG_KEY_MQTT_PASS, cfg->pass);
    if (rc == ESP_OK) rc = nvs_set_u8 (h, CONFIG_KEY_MQTT_TLS,  cfg->tls ? 1u : 0u);
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);

    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "save_mqtt write failed: %s", esp_err_to_name(rc));
        return rc;
    }
    memcpy(&g_mqtt_cfg, cfg, sizeof(g_mqtt_cfg));
    ESP_LOGI(TAG, "mqtt saved: host='%s' port=%u tls=%u",
             g_mqtt_cfg.host, g_mqtt_cfg.port, g_mqtt_cfg.tls);
    return ESP_OK;
}

esp_err_t config_save_label(const char *label)
{
    if (label == NULL) label = "";
    nvs_handle_t h;
    esp_err_t rc = nvs_open(CONFIG_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (rc != ESP_OK) return rc;
    rc = nvs_set_str(h, CONFIG_KEY_LABEL, label);
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);
    if (rc == ESP_OK) {
        snprintf(g_dial_label, sizeof(g_dial_label), "%s", label);
    }
    return rc;
}
