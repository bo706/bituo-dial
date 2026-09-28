/**
 * @file    config.h
 * @brief   NVS 持久化读写封装（任务书 5.10 节 Key 表）
 * @note    铁律：只在配置变更时写 NVS，严禁在 BLE 数据回调（~5Hz）路径上
 *          写 Flash（任务书坑4）。实时数据只进 PSRAM/内部 RAM 队列。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#ifndef MAIN_CONFIG_H_
#define MAIN_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 各字段最大长度（含末尾 '\0'） */
#define CONFIG_WIFI_SSID_LEN   64
#define CONFIG_WIFI_PASS_LEN   64
#define CONFIG_MQTT_HOST_LEN   64
#define CONFIG_MQTT_USER_LEN   64
#define CONFIG_MQTT_PASS_LEN   128
#define CONFIG_LABEL_LEN       32
#define CONFIG_TOKEN_LEN       64

/* NVS 命名空间与 Key 名（任务书 5.10） */
#define CONFIG_NVS_NAMESPACE  "bituo"
#define CONFIG_KEY_WIFI_SSID  "wifi_ssid"
#define CONFIG_KEY_WIFI_PASS  "wifi_pass"
#define CONFIG_KEY_MQTT_HOST  "mqtt_host"
#define CONFIG_KEY_MQTT_PORT  "mqtt_port"
#define CONFIG_KEY_MQTT_USER  "mqtt_user"
#define CONFIG_KEY_MQTT_PASS  "mqtt_pass"
#define CONFIG_KEY_MQTT_TLS       "mqtt_tls"
#define CONFIG_KEY_MQTT_INSECURE  "mqtt_insec"
#define CONFIG_KEY_HTTP_LAN       "http_lan"
#define CONFIG_KEY_LABEL          "dial_label"
#define CONFIG_KEY_TOKEN          "api_token"
/* meter_count / meter_0..meter_15 由 meter_store.c 管理（结构在该模块） */

/* Wi-Fi 凭据（运行期内存镜像） */
typedef struct {
    char ssid[CONFIG_WIFI_SSID_LEN];
    char pass[CONFIG_WIFI_PASS_LEN];
} wifi_cred_t;

/* MQTT 配置（运行期内存镜像） */
typedef struct {
    char     host[CONFIG_MQTT_HOST_LEN];
    uint16_t port;
    char     user[CONFIG_MQTT_USER_LEN];
    char     pass[CONFIG_MQTT_PASS_LEN];
    uint8_t  tls;        /* 0=mqtt://  1=mqtts:// 单向 TLS，无客户端证书 */
    uint8_t  insecure;   /* 1=不校验 Broker 证书（实验室自签默认） */
} mqtt_cfg_t;

/* 全局运行期配置镜像（config_load 后有效） */
extern wifi_cred_t g_wifi_cred;
extern mqtt_cfg_t  g_mqtt_cfg;
extern uint8_t     g_http_lan;   /* 0=默认关闭局域网 HTTP */
extern char        g_dial_label[CONFIG_LABEL_LEN];
extern char        g_api_token[CONFIG_TOKEN_LEN];

/**
 * @brief 启动时从 NVS 载入全部配置到内存镜像（app_main 初始化序列调用）
 * @note  电表列表由 meter_store_load_nvs() 载入，本函数内部串联调用。
 */
esp_err_t config_load(void);

/** @brief NVS 中是否已存在非空 Wi-Fi SSID（决定走 STA 还是 SoftAP） */
bool config_has_wifi(void);

/** @brief 写入 Wi-Fi 凭据到 NVS 并更新内存镜像（仅 setwifi 命令路径调用） */
esp_err_t config_save_wifi(const char *ssid, const char *pass);

/** @brief 写入 MQTT 配置到 NVS 并更新内存镜像（仅 set_mqtt 命令路径调用） */
esp_err_t config_save_mqtt(const mqtt_cfg_t *cfg);

/** @brief 局域网 HTTP 开关（默认关）；仅配置命令路径调用 */
esp_err_t config_save_http(uint8_t enable);

/** @brief 当前是否允许局域网 HTTP */
bool config_http_enabled(void);

/** @brief 写入设备显示标签（可选） */
esp_err_t config_save_label(const char *label);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_CONFIG_H_ */
