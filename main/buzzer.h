/**
 * @file    buzzer.h
 * @brief   M5Dial 板载蜂鸣器 PWM(LEDC) 驱动，控制脚 GPIO3（任务书 2.2）
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#ifndef MAIN_BUZZER_H_
#define MAIN_BUZZER_H_

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUZZER_PIN_GPIO         3
#define BUZZER_DEFAULT_FREQ_HZ  2700    /* 蜂鸣器谐振频率附近 */

/**
 * @brief 初始化 LEDC PWM 通道（GPIO3）
 */
esp_err_t buzzer_init(void);

/**
 * @brief 鸣响指定时长（阻塞式，任务书 5.5.4：上线 100ms 一声/离线两声）
 * @param duration_ms 鸣响时长(ms)
 * @note  仅允许在 ui_task 上下文调用；后续如需非阻塞可改 esp_timer 实现
 */
void buzzer_beep_ms(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_BUZZER_H_ */
