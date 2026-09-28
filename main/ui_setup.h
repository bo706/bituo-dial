/**
 * @file    ui_setup.h
 * @brief   系统页长按进入的设置：扫 Wi-Fi、扫电表、HTTP 开关、热点
 * @note    仅 ui_task 调用 LVGL
 */
#ifndef MAIN_UI_SETUP_H_
#define MAIN_UI_SETUP_H_

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_setup_create(void);
bool      ui_setup_is_open(void);
void      ui_setup_open(void);
void      ui_setup_close(void);
void      ui_setup_rotate(int dir);
void      ui_setup_short_press(void);
/** @return true 已消费长按（返回上一级或关闭） */
bool      ui_setup_long_press(void);
void      ui_setup_refresh(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_UI_SETUP_H_ */
