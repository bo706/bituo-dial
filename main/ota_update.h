/**
 * @file    ota_update.h
 * @brief   双槽 OTA：局域网 HTTP 上传；断电/校验失败留在旧槽；
 *          新固件须运行约 20s 后 mark valid，否则下次启动回滚。
 */
#ifndef MAIN_OTA_UPDATE_H_
#define MAIN_OTA_UPDATE_H_

#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

void ota_update_start_confirm_task(void);
void ota_update_add_info(cJSON *d);
esp_err_t ota_update_http_post(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_OTA_UPDATE_H_ */
