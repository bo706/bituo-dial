/**
 * @file    http_server.h
 * @brief   局域网 HTTP：默认关闭；开启后 REST + 内嵌状态/配置页
 */
#ifndef MAIN_HTTP_SERVER_H_
#define MAIN_HTTP_SERVER_H_

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t http_server_start(void);
esp_err_t http_server_stop(void);
bool      http_server_is_running(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_HTTP_SERVER_H_ */
