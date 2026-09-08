/**
 * @file    encoder.h
 * @brief   M5Dial 旋转编码器(A=40,B=41) + 侧按键(BTN=42) 驱动
 *          （任务书 2.2 / 官方 PinMap：16 档/圈、64 脉冲/圈）
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#ifndef MAIN_ENCODER_H_
#define MAIN_ENCODER_H_

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ENCODER_PIN_A       40
#define ENCODER_PIN_B       41
#define ENCODER_PIN_BTN     42   /* 侧按键，兼作 WAKE */

#define ENCODER_DIR_INVERT  0

/**
 * @brief 初始化 A/B/BTN 三个 GPIO（输入 + 内部上拉）
 */
esp_err_t encoder_init(void);

/**
 * @brief 对 A/B 相电平采样一次，更新内部累计脉冲数
 * @note  需由固定周期任务调用（建议 ui_task 内 1~5ms 一次）；
 *        Phase3 也可改为 GPIO 中断方式。
 */
void encoder_poll(void);

/**
 * @brief 读取并清零自上次调用以来的累计脉冲（正=一个方向，负=反方向）
 * @return 带符号脉冲数；旋转方向若与实物相反，对调符号即可（见 .c 注释）
 */
int encoder_get_delta(void);

/**
 * @brief 读取侧按键当前电平状态
 * @return true=按下（默认低电平有效，需以原理图最终确认）
 */
bool encoder_button_is_pressed(void);

/**
 * @brief 诊断：取出并清零自上次调用以来 A/B 相的“原始电平翻转次数”，
 *        同时返回当前 A/B 电平。用于区分“硬件没信号(FPC/编码器)”与
 *        “有信号但正交解码/分发软件有问题”。
 * @param a_toggles  输出 A 相翻转次数（NULL 可空）
 * @param b_toggles  输出 B 相翻转次数（NULL 可空）
 * @param level_a    输出 A 相当前电平 0/1（NULL 可空）
 * @param level_b    输出 B 相当前电平 0/1（NULL 可空）
 * @note  正常旋转时两个 toggles 应同步快速增长；若怎么转都恒为 0 且电平
 *        恒定为 1(上拉默认高)，说明 GPIO40/41 收不到脉冲，属硬件/FPC 问题。
 */
void encoder_diag_consume(uint32_t *a_toggles, uint32_t *b_toggles,
                          int *level_a, int *level_b);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_ENCODER_H_ */
