/**
 * @file    http_server.c
 * @brief   REST：任务书 5.8 全部端点；JSON 与 GATT 共用 cmd_dispatch
 *          首页为 EMBED_FILES index.html（三标签，约 10KB，不走 SPIFFS）
 *          需求 5：电表同名路径别名 /save-config /data /model 等
 * @date    2026-09-09
 */
#include "http_server.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "config.h"
#include "meter_store.h"
#include "cmd_dispatch.h"
#include "ota_update.h"
#include "sys_task.h"
#include "bituo_mqtt.h"

static const char *TAG = "http";
static httpd_handle_t s_server;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static bool token_ok(httpd_req_t *req)
{
    if (g_api_token[0] == '\0') {
        return true;
    }
    char tok[CONFIG_TOKEN_LEN];
    if (httpd_req_get_hdr_value_str(req, "X-Api-Token", tok, sizeof(tok)) == ESP_OK) {
        if (strcmp(tok, g_api_token) == 0) {
            return true;
        }
    }
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "token", tok, sizeof(tok)) == ESP_OK) {
            return strcmp(tok, g_api_token) == 0;
        }
    }
    return false;
}

static esp_err_t send_json(httpd_req_t *req, const char *s)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, s != NULL ? s : "{}");
}

static esp_err_t send_forbidden(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    return send_json(req, "{\"ok\":false,\"msg\":\"unauthorized\",\"event\":\"\",\"d\":{}}");
}

static int recv_body(httpd_req_t *req, char *buf, int cap)
{
    int total = req->content_len;
    if (total < 0 || total >= cap) {
        return -1;
    }
    if (total == 0) {
        buf[0] = '\0';
        return 0;
    }
    int off = 0;
    while (off < total) {
        int n = httpd_req_recv(req, buf + off, total - off);
        if (n <= 0) {
            return -1;
        }
        off += n;
    }
    buf[off] = '\0';
    return off;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode_inplace(char *s)
{
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && hex_nibble(r[1]) >= 0 && hex_nibble(r[2]) >= 0) {
            *w++ = (char)((hex_nibble(r[1]) << 4) | hex_nibble(r[2]));
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* JSON 正文，或电表 HTTP 80 表单字段 plain=<JSON> */
static int extract_cmd_json(const char *body, char *out, int cap)
{
    if (out == NULL || cap < 3) {
        return -1;
    }
    if (body == NULL) {
        out[0] = '{';
        out[1] = '}';
        out[2] = '\0';
        return 0;
    }
    const char *p = body;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (*p == '{') {
        snprintf(out, (size_t)cap, "%s", p);
        return 0;
    }
    const char *key = strstr(body, "plain=");
    if (key == NULL) {
        snprintf(out, (size_t)cap, "%s", body[0] ? body : "{}");
        return 0;
    }
    key += 6;
    const char *end = strchr(key, '&');
    size_t n = (end != NULL) ? (size_t)(end - key) : strlen(key);
    if (n >= (size_t)cap) {
        return -1;
    }
    memcpy(out, key, n);
    out[n] = '\0';
    url_decode_inplace(out);
    return 0;
}

static void http_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
}

/** 分块写出 d 对象：{"dial_sn":"...","meters":[...]}，16 表不建整棵 cJSON。 */
static esp_err_t send_meters_data_object(httpd_req_t *req)
{
    char dial_sn[32];
    cmd_get_dial_sn(dial_sn, sizeof(dial_sn));
    char head[96];
    snprintf(head, sizeof(head), "{\"dial_sn\":\"%s\",\"meters\":[", dial_sn);
    if (httpd_resp_sendstr_chunk(req, head) != ESP_OK) {
        return ESP_FAIL;
    }
    char one[640];
    for (int i = 0; i < g_meter_count; i++) {
        meter_data_t data;
        memset(&data, 0, sizeof(data));
        if (g_meter_queues[i] != NULL) {
            xQueuePeek(g_meter_queues[i], &data, 0);
        }
        if (i > 0 && httpd_resp_sendstr_chunk(req, ",") != ESP_OK) {
            return ESP_FAIL;
        }
        if (meter_store_native_json_print(i, &data, dial_sn, one, sizeof(one)) < 0) {
            if (httpd_resp_sendstr_chunk(req, "{}") != ESP_OK) {
                return ESP_FAIL;
            }
            continue;
        }
        if (httpd_resp_sendstr_chunk(req, one) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return httpd_resp_sendstr_chunk(req, "]}");
}

static cJSON *make_status_d(void)
{
    char ip[16] = "";
    sys_wifi_ip_str(ip, sizeof(ip));
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "wifi_connected", sys_wifi_sta_got_ip());
    cJSON_AddStringToObject(d, "wifi_ssid", g_wifi_cred.ssid);
    cJSON_AddBoolToObject(d, "mqtt_connected", mqtt_client_is_connected());
    cJSON_AddStringToObject(d, "mqtt_host", g_mqtt_cfg.host);
    cJSON_AddNumberToObject(d, "mqtt_port", g_mqtt_cfg.port);
    cJSON_AddBoolToObject(d, "http_enabled", http_server_is_running());
    cJSON_AddStringToObject(d, "ip", ip);
    cJSON_AddNumberToObject(d, "meter_count", g_meter_count);
    cJSON_AddNumberToObject(d, "online_count", meter_store_online_count());
    return d;
}

static esp_err_t send_envelope(httpd_req_t *req, bool ok, const char *msg,
                               const char *event, cJSON *d)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "msg", msg != NULL ? msg : "");
    cJSON_AddStringToObject(root, "event", event != NULL ? event : "");
    if (d != NULL) {
        cJSON_AddItemToObject(root, "d", d);
    } else {
        cJSON_AddObjectToObject(root, "d");
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t rc = send_json(req, s);
    cJSON_free(s);
    return rc;
}

static esp_err_t send_cmd(httpd_req_t *req, const char *json)
{
    char *buf = malloc(BITUO_RESP_CAP);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cmd_dispatch_exec(json, buf, BITUO_RESP_CAP);
    esp_err_t rc = send_json(req, buf);
    free(buf);
    return rc;
}

static const char *uri_last(const char *uri)
{
    const char *p = strrchr(uri, '/');
    return (p != NULL && p[1] != '\0') ? (p + 1) : "";
}

static esp_err_t send_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    int n = (int)(index_html_end - index_html_start);
    if (n < 0) {
        n = 0;
    }
    return httpd_resp_send(req, (const char *)index_html_start, n);
}

static esp_err_t index_get(httpd_req_t *req)
{
    return send_index(req);
}

static esp_err_t dash_get(httpd_req_t *req)
{
    return send_index(req);
}

static esp_err_t info_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    return send_cmd(req, "{\"cmd\":\"get_info\"}");
}

static esp_err_t meters_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    return send_cmd(req, "{\"cmd\":\"get_meters\"}");
}

static esp_err_t meters_one_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    const char *sn = uri_last(req->uri);
    if (meter_store_find_by_sn(sn) < 0) {
        return send_json(req, "{\"ok\":false,\"msg\":\"sn not found\",\"event\":\"get_meters\",\"d\":{}}");
    }
    char *all = malloc(BITUO_RESP_CAP);
    if (all == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cmd_dispatch_exec("{\"cmd\":\"get_meters\"}", all, BITUO_RESP_CAP);
    cJSON *root = cJSON_Parse(all);
    free(all);
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON *d = cJSON_GetObjectItem(root, "d");
    cJSON *arr = (d != NULL) ? cJSON_GetObjectItem(d, "meters") : NULL;
    cJSON *hit = NULL;
    if (arr != NULL) {
        cJSON *it;
        cJSON_ArrayForEach(it, arr) {
            cJSON *jsn = cJSON_GetObjectItem(it, "sn");
            if (jsn != NULL && cJSON_IsString(jsn) && strcmp(jsn->valuestring, sn) == 0) {
                hit = it;
                break;
            }
        }
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", hit != NULL);
    cJSON_AddStringToObject(out, "msg", hit != NULL ? "" : "sn not found");
    cJSON_AddStringToObject(out, "event", "get_meters");
    if (hit != NULL) {
        cJSON *dd = cJSON_CreateObject();
        cJSON_AddItemToObject(dd, "meter", cJSON_Duplicate(hit, 1));
        cJSON_AddItemToObject(out, "d", dd);
    } else {
        cJSON_AddObjectToObject(out, "d");
    }
    char *s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);
    esp_err_t rc = send_json(req, s);
    cJSON_free(s);
    return rc;
}

static esp_err_t config_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "msg", "");
    cJSON_AddStringToObject(root, "event", "get_config");
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "wifi_ssid", g_wifi_cred.ssid);
    cJSON_AddStringToObject(d, "mqtt_host", g_mqtt_cfg.host);
    cJSON_AddNumberToObject(d, "mqtt_port", g_mqtt_cfg.port);
    cJSON_AddNumberToObject(d, "mqtt_tls", g_mqtt_cfg.tls);
    cJSON_AddNumberToObject(d, "mqtt_insecure", g_mqtt_cfg.insecure);
    cJSON_AddNumberToObject(d, "http_lan", g_http_lan);
    cJSON_AddStringToObject(d, "label", g_dial_label);
    cJSON_AddNumberToObject(d, "meter_count", g_meter_count);
    cJSON_AddItemToObject(root, "d", d);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t rc = send_json(req, s);
    cJSON_free(s);
    return rc;
}

static char *inject_cmd(const char *body, const char *cmd)
{
    cJSON *root = cJSON_Parse((body != NULL && body[0]) ? body : "{}");
    if (root == NULL) {
        root = cJSON_CreateObject();
    }
    if (cJSON_GetObjectItem(root, "cmd") == NULL) {
        cJSON_AddStringToObject(root, "cmd", cmd);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static esp_err_t post_with_cmd(httpd_req_t *req, const char *cmd)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    char body[1024];
    if (recv_body(req, body, sizeof(body)) < 0) {
        return send_json(req, "{\"ok\":false,\"msg\":\"body too large\",\"event\":\"\",\"d\":{}}");
    }
    char *json = inject_cmd(body, cmd);
    if (json == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t rc = send_cmd(req, json);
    cJSON_free(json);
    return rc;
}

static esp_err_t config_post(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    char body[1024];
    if (recv_body(req, body, sizeof(body)) < 0) {
        return send_json(req, "{\"ok\":false,\"msg\":\"body too large\",\"event\":\"\",\"d\":{}}");
    }
    char json[1024];
    if (extract_cmd_json(body, json, sizeof(json)) < 0) {
        return send_json(req, "{\"ok\":false,\"msg\":\"body too large\",\"event\":\"\",\"d\":{}}");
    }
    return send_cmd(req, json);
}

static esp_err_t save_config_post(httpd_req_t *req)
{
    return config_post(req);
}

static esp_err_t config_wifi_post(httpd_req_t *req)
{
    return post_with_cmd(req, "setwifi");
}

static esp_err_t config_mqtt_post(httpd_req_t *req)
{
    return post_with_cmd(req, "set_mqtt");
}

static esp_err_t config_http_post(httpd_req_t *req)
{
    return post_with_cmd(req, "set_http");
}

static esp_err_t scan_wifi_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    return send_cmd(req, "{\"cmd\":\"scan_wifi\"}");
}

static esp_err_t scan_ble_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    return send_cmd(req, "{\"cmd\":\"scan_ble\"}");
}

static esp_err_t config_meters_post(httpd_req_t *req)
{
    return post_with_cmd(req, "add_meter");
}

static esp_err_t config_meters_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    return send_cmd(req, "{\"cmd\":\"list_meters\"}");
}

static esp_err_t config_meters_del(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    const char *sn = uri_last(req->uri);
    char json[96];
    snprintf(json, sizeof(json), "{\"cmd\":\"del_meter\",\"sn\":\"%s\"}", sn);
    return send_cmd(req, json);
}

static esp_err_t sys_restart_post(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    return send_cmd(req, "{\"cmd\":\"restart\"}");
}

static esp_err_t restart_get(httpd_req_t *req)
{
    return sys_restart_post(req);
}

static esp_err_t data_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    http_json_headers(req);
    if (httpd_resp_sendstr_chunk(req,
            "{\"ok\":true,\"msg\":\"\",\"event\":\"data\",\"d\":") != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    if (send_meters_data_object(req) != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(req, "}") != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t model_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    char *buf = malloc(BITUO_RESP_CAP);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cmd_dispatch_exec("{\"cmd\":\"get_info\"}", buf, BITUO_RESP_CAP);
    cJSON *info = cJSON_Parse(buf);
    free(buf);
    if (info == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON *d = cJSON_DetachItemFromObject(info, "d");
    cJSON_Delete(info);
    return send_envelope(req, true, "", "model", d);
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    return send_envelope(req, true, "", "status", make_status_d());
}

static esp_err_t snapshot_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    http_json_headers(req);
    if (httpd_resp_sendstr_chunk(req,
            "{\"ok\":true,\"msg\":\"\",\"event\":\"snapshot\",\"d\":{\"data\":") != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    if (send_meters_data_object(req) != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    cJSON *st = make_status_d();
    char *ss = (st != NULL) ? cJSON_PrintUnformatted(st) : NULL;
    if (st != NULL) {
        cJSON_Delete(st);
    }
    if (httpd_resp_sendstr_chunk(req, ",\"model\":{},\"status\":") != ESP_OK) {
        cJSON_free(ss);
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    if (httpd_resp_sendstr_chunk(req, ss != NULL ? ss : "{}") != ESP_OK) {
        cJSON_free(ss);
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    cJSON_free(ss);
    if (httpd_resp_sendstr_chunk(req, "}}") != ESP_OK) {
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_FAIL;
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t sn_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    char sn[32];
    cmd_get_dial_sn(sn, sizeof(sn));
    char line[48];
    snprintf(line, sizeof(line), "bituo-dial %s\n", sn);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, line);
}

static esp_err_t signin_get(httpd_req_t *req)
{
    /* 探测接口：未设 token 为开放模式；已设则须头或 ?token= 匹配 */
    if (g_api_token[0] == '\0') {
        return send_envelope(req, true, "open", "signin", cJSON_CreateObject());
    }
    if (token_ok(req)) {
        return send_envelope(req, true, "ok", "signin", cJSON_CreateObject());
    }
    return send_envelope(req, false, "need login", "signin", cJSON_CreateObject());
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    return ota_update_http_post(req);
}

static esp_err_t ota_get(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return send_forbidden(req);
    }
    return send_cmd(req, "{\"cmd\":\"get_info\"}");
}

static esp_err_t register_uri(httpd_handle_t srv, const char *uri,
                              httpd_method_t method, esp_err_t (*h)(httpd_req_t *))
{
    httpd_uri_t u = {
        .uri = uri,
        .method = method,
        .handler = h,
        .user_ctx = NULL,
    };
    esp_err_t rc = httpd_register_uri_handler(srv, &u);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "register %s failed: %s", uri, esp_err_to_name(rc));
    }
    return rc;
}

esp_err_t http_server_start(void)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "http server already started");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 32;
    config.stack_size = 10240;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 60;
    config.send_wait_timeout = 30;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        return ESP_FAIL;
    }

    register_uri(s_server, "/", HTTP_GET, index_get);
    register_uri(s_server, "/dash", HTTP_GET, dash_get);
    register_uri(s_server, "/api/info", HTTP_GET, info_get);
    register_uri(s_server, "/api/meters", HTTP_GET, meters_get);
    register_uri(s_server, "/api/meters/*", HTTP_GET, meters_one_get);
    register_uri(s_server, "/api/scan/wifi", HTTP_GET, scan_wifi_get);
    register_uri(s_server, "/api/scan/ble", HTTP_GET, scan_ble_get);
    register_uri(s_server, "/config", HTTP_GET, config_get);
    register_uri(s_server, "/config", HTTP_POST, config_post);
    register_uri(s_server, "/save-config", HTTP_POST, save_config_post);
    register_uri(s_server, "/data", HTTP_GET, data_get);
    register_uri(s_server, "/model", HTTP_GET, model_get);
    register_uri(s_server, "/status", HTTP_GET, status_get);
    register_uri(s_server, "/snapshot", HTTP_GET, snapshot_get);
    register_uri(s_server, "/sn", HTTP_GET, sn_get);
    register_uri(s_server, "/restart", HTTP_GET, restart_get);
    register_uri(s_server, "/signin", HTTP_GET, signin_get);
    register_uri(s_server, "/config/wifi", HTTP_POST, config_wifi_post);
    register_uri(s_server, "/config/mqtt", HTTP_POST, config_mqtt_post);
    register_uri(s_server, "/config/http", HTTP_POST, config_http_post);
    register_uri(s_server, "/config/meters", HTTP_GET, config_meters_get);
    register_uri(s_server, "/config/meters", HTTP_POST, config_meters_post);
    register_uri(s_server, "/config/meters/*", HTTP_DELETE, config_meters_del);
    register_uri(s_server, "/sys/restart", HTTP_POST, sys_restart_post);
    register_uri(s_server, "/ota", HTTP_POST, ota_post);
    register_uri(s_server, "/api/ota", HTTP_GET, ota_get);

    ESP_LOGI(TAG, "http server started on port 80");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t rc = httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "http server stopped (%s)", esp_err_to_name(rc));
    return rc;
}

bool http_server_is_running(void)
{
    return s_server != NULL;
}
