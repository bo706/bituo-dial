/**
 * @file    http_server.h
 * @brief   ESP-IDF HTTP Server，REST API + Web 配置页（任务书 5.8 节）
 * @note    占位框架，Phase 2 先实现 /api/info，Phase 4 补齐全部端点。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#ifndef MAIN_HTTP_SERVER_H_
#define MAIN_HTTP_SERVER_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 HTTP Server（端口 80）
 */
esp_err_t http_server_start(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_HTTP_SERVER_H_ */
