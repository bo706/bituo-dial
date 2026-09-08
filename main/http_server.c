/**
 * @file    http_server.c
 * @brief   REST：任务书 5.8 全部端点；JSON 与 GATT 共用 cmd_dispatch
 * @date    2026-09-03
 */
#include "http_server.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

#include "config.h"
#include "meter_store.h"
#include "cmd_dispatch.h"

static const char *TAG = "http";
static httpd_handle_t s_server;

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Bituo Dial</title>"
    "<style>body{font-family:sans-serif;max-width:28em;margin:1em auto;padding:0 1em}"
    "input,button{width:100%;margin:.3em 0;padding:.4em}label{font-size:.9em}"
    "pre{white-space:pre-wrap;background:#f4f4f4;padding:.6em;min-height:3em}</style>"
    "</head><body><h2>Bituo Dial</h2>"
    "<p><a href='/dash'>仪表盘</a> &middot; "
    "<a href='/api/info'>/api/info</a> &middot; "
    "<a href='/api/meters'>/api/meters</a> &middot; "
    "<a href='/config'>/config</a></p>"
    "<h3>Wi-Fi (2.4G only)</h3>"
    "<label>SSID</label><input id='ssid'>"
    "<label>Password</label><input id='wpass' type='password'>"
    "<button type='button' onclick='saveWifi()'>Save Wi-Fi</button>"
    "<h3>MQTT (optional)</h3>"
    "<label>Host</label><input id='host'>"
    "<label>Port</label><input id='port' value='1883'>"
    "<label>User</label><input id='user'>"
    "<label>Pass</label><input id='mpass' type='password'>"
    "<button type='button' onclick='saveMqtt()'>Save MQTT</button>"
    "<p><button type='button' onclick='doRestart()'>Restart</button></p>"
    "<pre id='out'>ready</pre>"
    "<script>"
    "function $(id){return document.getElementById(id);}"
    "async function post(u,b){"
    "  var el=$('out'); el.textContent='saving...';"
    "  try{"
    "    var r=await fetch(u,{method:'POST',"
    "      headers:{'Content-Type':'application/json'},"
    "      body:JSON.stringify(b)});"
    "    el.textContent=await r.text();"
    "  }catch(e){ el.textContent='request failed: '+e"
    "    +'\\nHotspot may drop after save. Check Dial System page.'; }"
    "}"
    "function saveWifi(){post('/config/wifi',{ssid:$('ssid').value,pass:$('wpass').value});}"
    "function saveMqtt(){post('/config/mqtt',{host:$('host').value,"
    "  port:parseInt($('port').value)||1883,user:$('user').value,"
    "  pass:$('mpass').value,tls:0});}"
    "function doRestart(){post('/sys/restart',{});}"
    "</script>"
    "</body></html>";

static const char DASH_HTML[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Bituo Dial</title>"
    "<style>"
    "body{margin:0;background:#0b0f14;color:#e8eaed;font-family:sans-serif}"
    "header{padding:14px 18px;background:#111827;display:flex;"
    "justify-content:space-between;align-items:center}"
    "h1{font-size:18px;margin:0;color:#4fc3f7}"
    "a{color:#9aa0a6}"
    ".meta{padding:8px 18px;color:#9aa0a6;font-size:13px}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));"
    "gap:12px;padding:12px 18px 24px}"
    ".card{background:#151b23;border-radius:12px;padding:14px}"
    ".card h2{margin:0 0 6px;font-size:16px}"
    ".on{color:#35d47a}.off{color:#6b7075}"
    "table{width:100%;border-collapse:collapse;font-size:14px}"
    "td{padding:3px 0}.k{color:#9aa0a6}.v{text-align:right;font-variant-numeric:tabular-nums}"
    ".err{color:#f2c14e;padding:0 18px}"
    "</style></head><body>"
    "<header><h1>Bituo Dial</h1><a href='/'>配置</a></header>"
    "<div class=meta id=meta>loading...</div>"
    "<div class=err id=err></div>"
    "<div class=grid id=grid></div>"
    "<script>"
    "function n(x,d){if(x===undefined||x===null)return '--';"
    "var v=Number(x);return isFinite(v)?v.toFixed(d):'--';}"
    "async function tick(){"
    " try{"
    "  var i=await (await fetch('/api/info')).json();"
    "  var m=await (await fetch('/api/meters')).json();"
    "  var d=i.d||{}; var w=d.wifi||{}; var b=d.ble_scan||{};"
    "  document.getElementById('meta').textContent="
    "   (d.dial_sn||'')+'  Wi-Fi '+(w.connected?'on':'off')+"
    "   '  meters '+(b.online_count||0)+'/'+(b.meter_count||0);"
    "  document.getElementById('err').textContent='';"
    "  var arr=(m.d&&m.d.meters)||[];"
    "  document.getElementById('grid').innerHTML=arr.map(function(t){"
    "   var on=!!t.valid;"
    "   var fe=(t.forward_energy_x||0)+(t.forward_energy_y||0)+(t.forward_energy_z||0);"
    "   var re=(t.reverse_energy_x||0)+(t.reverse_energy_y||0)+(t.reverse_energy_z||0);"
    "   return '<div class=card><h2>'+(t.label||t.sn)+"
    "    ' <span class='+(on?'on':'off')+'>'+(on?'online':'offline')+'</span></h2>'+"
    "    '<div class=k>'+t.sn+'</div><table>'+"
    "    '<tr><td class=k>X</td><td class=v>'+n(t.voltage_x,1)+' V  '+n(t.current_x,2)+' A</td></tr>'+"
    "    '<tr><td class=k>Y</td><td class=v>'+n(t.voltage_y,1)+' V  '+n(t.current_y,2)+' A</td></tr>'+"
    "    '<tr><td class=k>Z</td><td class=v>'+n(t.voltage_z,1)+' V  '+n(t.current_z,2)+' A</td></tr>'+"
    "    '<tr><td class=k>P</td><td class=v>'+n(t.total_active_power,3)+' kW</td></tr>'+"
    "    '<tr><td class=k>E+</td><td class=v>'+n(fe,2)+' kWh</td></tr>'+"
    "    '<tr><td class=k>E-</td><td class=v>'+n(re,2)+' kWh</td></tr>'+"
    "    '<tr><td class=k>RSSI</td><td class=v>'+n(t.rssi,0)+' dBm</td></tr>'+"
    "    '</table></div>';"
    "  }).join('');"
    " }catch(e){document.getElementById('err').textContent='refresh failed';}"
    "}"
    "tick();setInterval(tick,2000);"
    "</script></body></html>";

static bool token_ok(httpd_req_t *req)
{
    if (g_api_token[0] == '\0') {
        return true;
    }
    char tok[CONFIG_TOKEN_LEN];
    if (httpd_req_get_hdr_value_str(req, "X-Api-Token", tok, sizeof(tok)) != ESP_OK) {
        return false;
    }
    return strcmp(tok, g_api_token) == 0;
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

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, INDEX_HTML);
}

static esp_err_t dash_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, DASH_HTML);
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
    return send_cmd(req, body);
}

static esp_err_t config_wifi_post(httpd_req_t *req)
{
    return post_with_cmd(req, "setwifi");
}

static esp_err_t config_mqtt_post(httpd_req_t *req)
{
    return post_with_cmd(req, "set_mqtt");
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
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

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
    register_uri(s_server, "/config", HTTP_GET, config_get);
    register_uri(s_server, "/config", HTTP_POST, config_post);
    register_uri(s_server, "/config/wifi", HTTP_POST, config_wifi_post);
    register_uri(s_server, "/config/mqtt", HTTP_POST, config_mqtt_post);
    register_uri(s_server, "/config/meters", HTTP_GET, config_meters_get);
    register_uri(s_server, "/config/meters", HTTP_POST, config_meters_post);
    register_uri(s_server, "/config/meters/*", HTTP_DELETE, config_meters_del);
    register_uri(s_server, "/sys/restart", HTTP_POST, sys_restart_post);

    ESP_LOGI(TAG, "http server started on port 80");
    return ESP_OK;
}
