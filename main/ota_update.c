/**
 * @file    ota_update.c
 * @brief   写入非活动 ota 槽；完整校验后才切启动分区。
 *          写到一半断电：otadata 仍指向旧槽。
 *          新槽启动后 20s 内崩溃/看门狗：bootloader 回滚旧槽。
 */
#include "ota_update.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "ota";
#define OTA_CONFIRM_MS     20000
#define OTA_CHUNK          2048
#define OTA_MAGIC          0xE9

static SemaphoreHandle_t s_mu;
static volatile bool s_busy;

static const char *state_str(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:             return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:  return "pending_verify";
    case ESP_OTA_IMG_VALID:           return "valid";
    case ESP_OTA_IMG_INVALID:         return "invalid";
    case ESP_OTA_IMG_ABORTED:         return "aborted";
    case ESP_OTA_IMG_UNDEFINED:       return "undefined";
    default:                          return "unknown";
    }
}

static void confirm_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_CONFIRM_MS));
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run != NULL) {
        (void)esp_ota_get_state_partition(run, &st);
    }
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t rc = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "confirm running image: %s", esp_err_to_name(rc));
    } else {
        ESP_LOGI(TAG, "no pending verify (state=%s), skip mark", state_str(st));
    }
    vTaskDelete(NULL);
}

void ota_update_start_confirm_task(void)
{
    if (xTaskCreate(confirm_task, "ota_ok", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "confirm task create failed");
    }
}

void ota_update_add_info(cJSON *d)
{
    if (d == NULL) {
        return;
    }
    cJSON *ota = cJSON_CreateObject();
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (run != NULL) {
        cJSON_AddStringToObject(ota, "running", run->label);
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(run, &st) == ESP_OK) {
            cJSON_AddStringToObject(ota, "state", state_str(st));
        }
    }
    if (boot != NULL) {
        cJSON_AddStringToObject(ota, "boot", boot->label);
    }
    if (next != NULL) {
        cJSON_AddStringToObject(ota, "next", next->label);
        cJSON_AddNumberToObject(ota, "next_size", next->size);
    }
    cJSON_AddBoolToObject(ota, "busy", s_busy);
    cJSON_AddItemToObject(d, "ota", ota);
}

static void restart_later(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    vTaskDelete(NULL);
}

static esp_err_t send_ota_json(httpd_req_t *req, bool ok, const char *msg)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"ok\":%s,\"msg\":\"%s\",\"event\":\"ota\",\"d\":{}}",
             ok ? "true" : "false", msg != NULL ? msg : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, buf);
}

esp_err_t ota_update_http_post(httpd_req_t *req)
{
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateMutex();
    }
    if (s_mu == NULL || xSemaphoreTake(s_mu, 0) != pdTRUE) {
        return send_ota_json(req, false, "ota busy");
    }
    s_busy = true;

    int total = req->content_len;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_err_t rc = ESP_FAIL;
    const char *msg = "ota failed";
    esp_ota_handle_t handle = 0;
    bool began = false;

    if (total <= 1024 || part == NULL) {
        msg = "invalid image size";
        goto done;
    }
    if (total > (int)part->size) {
        msg = "image larger than ota slot";
        goto done;
    }

    ESP_LOGI(TAG, "OTA begin dest=%s size=%d", part->label, total);
    rc = esp_ota_begin(part, total, &handle);
    if (rc != ESP_OK) {
        msg = "esp_ota_begin failed";
        goto done;
    }
    began = true;

    char buf[OTA_CHUNK];
    int got_total = 0;
    bool magic_ok = false;
    while (got_total < total) {
        int want = total - got_total;
        if (want > (int)sizeof(buf)) {
            want = (int)sizeof(buf);
        }
        int n = httpd_req_recv(req, buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            msg = "upload interrupted";
            rc = ESP_FAIL;
            goto done;
        }
        if (!magic_ok) {
            if ((uint8_t)buf[0] != OTA_MAGIC) {
                msg = "not an esp32 firmware (magic)";
                rc = ESP_FAIL;
                goto done;
            }
            magic_ok = true;
        }
        rc = esp_ota_write(handle, buf, (size_t)n);
        if (rc != ESP_OK) {
            msg = "esp_ota_write failed";
            goto done;
        }
        got_total += n;
        if ((got_total & 0xffff) == 0) {
            vTaskDelay(1);
        }
    }

    rc = esp_ota_end(handle);
    began = false;
    if (rc != ESP_OK) {
        msg = (rc == ESP_ERR_OTA_VALIDATE_FAILED) ? "image validate failed" : "esp_ota_end failed";
        goto done;
    }
    rc = esp_ota_set_boot_partition(part);
    if (rc != ESP_OK) {
        msg = "set boot partition failed";
        goto done;
    }
    msg = "ota ok, rebooting";
    ESP_LOGW(TAG, "OTA success, reboot to %s", part->label);
    send_ota_json(req, true, msg);
    s_busy = false;
    xSemaphoreGive(s_mu);
    xTaskCreate(restart_later, "ota_rst", 2048, NULL, 5, NULL);
    return ESP_OK;

done:
    if (began) {
        esp_ota_abort(handle);
    }
    ESP_LOGE(TAG, "OTA abort: %s (%s)", msg, esp_err_to_name(rc));
    s_busy = false;
    xSemaphoreGive(s_mu);
    return send_ota_json(req, false, msg);
}
