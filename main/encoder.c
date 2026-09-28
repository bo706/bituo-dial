/**
 * @file    encoder.c
 * @brief   旋转编码器正交解码 + 侧按键电平读取（轮询式框架）
 * @note    A=GPIO40, B=GPIO41, BTN=GPIO42（任务书 2.2）。
 *          正交状态机为通用实现；若真机旋转方向与增减相反，
 *          调换 s_trans_table 符号或对调 A/B 定义即可。
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#include "encoder.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "encoder";

static volatile int s_accum_delta = 0;
static uint8_t s_prev_ab = 0;

/* ── 诊断用：A/B 相“原始电平”翻转计数（独立于正交状态机，只看电平有没有跳变）── */
static volatile uint32_t s_raw_a_tog = 0;
static volatile uint32_t s_raw_b_tog = 0;
static int s_last_raw_a = 1;   /* 上拉默认静止为高 */
static int s_last_raw_b = 1;

/* 正交解码表：行=旧状态(AB)，列=新状态(AB)，值=脉冲增量 */
static const int8_t s_trans_table[4][4] = {
    /* new: 00  01  10  11 */
    /*00*/ {  0, -1, +1,  0 },
    /*01*/ { +1,  0,  0, -1 },
    /*10*/ { -1,  0,  0, +1 },
    /*11*/ {  0, +1, -1,  0 },
};

esp_err_t encoder_init(void)
{
    ESP_LOGI(TAG, "encoder_init: A=%d B=%d BTN=%d",
             ENCODER_PIN_A, ENCODER_PIN_B, ENCODER_PIN_BTN);

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << ENCODER_PIN_A) |
                        (1ULL << ENCODER_PIN_B) |
                        (1ULL << ENCODER_PIN_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,      /* 编码器/按键默认上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 记录初始相位，避免首帧误判 */
    int a = gpio_get_level(ENCODER_PIN_A);
    int b = gpio_get_level(ENCODER_PIN_B);
    s_prev_ab = (uint8_t)((a << 1) | b);
    s_accum_delta = 0;

    /* 诊断基线：静止电平 + 翻转计数清零 */
    s_last_raw_a = a;
    s_last_raw_b = b;
    s_raw_a_tog = 0;
    s_raw_b_tog = 0;
    ESP_LOGI(TAG, "idle levels at init: A=%d B=%d BTN=%d",
             a, b, gpio_get_level(ENCODER_PIN_BTN));

    return ESP_OK;
}

void encoder_poll(void)
{
    int a = gpio_get_level(ENCODER_PIN_A);
    int b = gpio_get_level(ENCODER_PIN_B);
    uint8_t now = (uint8_t)((a << 1) | b);

    /* 诊断：独立统计每相原始电平翻转（不依赖正交表），用于判断 GPIO 有无收到脉冲 */
    if (a != s_last_raw_a) { s_raw_a_tog++; s_last_raw_a = a; }
    if (b != s_last_raw_b) { s_raw_b_tog++; s_last_raw_b = b; }

    if (now != s_prev_ab) {
        int step = s_trans_table[s_prev_ab][now];
        if (step != 0) {
            s_accum_delta += step;
        }
        s_prev_ab = now;
    }
}

int encoder_get_delta(void)
{
    /* encoder_poll() 与本函数都在 ui_task 中顺序调用，同一任务上下文无需加锁；
     * 若后续改为 GPIO 中断中更新 s_accum_delta，需改用 portMUX_TYPE 自旋锁保护。
     */
    int v = s_accum_delta;
    s_accum_delta = 0;
#if ENCODER_DIR_INVERT
    return -v;
#else
    return v;
#endif
}

bool encoder_button_is_pressed(void)
{
    /* TODO(Phase3): 以 M5Dial 原理图确认按键有效电平；
     * 当前按"内部上拉、按下接地(低电平有效)"处理，短按/长按消抖在 ui_task 做。
     */
    return gpio_get_level(ENCODER_PIN_BTN) == 0;
}

void encoder_diag_consume(uint32_t *a_toggles, uint32_t *b_toggles,
                          int *level_a, int *level_b)
{
    if (a_toggles) { *a_toggles = s_raw_a_tog; s_raw_a_tog = 0; }
    if (b_toggles) { *b_toggles = s_raw_b_tog; s_raw_b_tog = 0; }
    if (level_a)  { *level_a = gpio_get_level(ENCODER_PIN_A); }
    if (level_b)  { *level_b = gpio_get_level(ENCODER_PIN_B); }
}
