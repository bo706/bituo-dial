/**
 * @file    cmd_dispatch.h
 * @brief   GATT / HTTP / MQTT cmd 共用分发（任务书第 7 章统一 JSON）
 * @version ESP-IDF v5.2.3
 * @date    2026-09-03
 */
#ifndef MAIN_CMD_DISPATCH_H_
#define MAIN_CMD_DISPATCH_H_

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 16 表 get_meters JSON 可达 ~2KB，缓冲必须足够大（原 GATT 坑点） */
#define BITUO_RESP_CAP  8192

/**
 * @brief 执行一条统一 JSON 命令，写入 {ok,msg,event,d} 到 out
 * @return 写入字节数（不含 '\\0'）；out 始终以 '\\0' 结尾
 */
int cmd_dispatch_exec(const char *json, char *out, int cap);

/** @brief Dial SN：DIAL- + eFuse MAC 后 3 字节 */
void cmd_get_dial_sn(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_CMD_DISPATCH_H_ */
