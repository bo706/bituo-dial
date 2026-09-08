/**
 * @file    buzzer.c
 * @brief   蜂鸣器 LEDC PWM 驱动框架（GPIO3）
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#include "buzzer.h"

#include <stdbool.h>
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "buzzer";

#define BUZZER_LEDC_TIMER       LEDC_TIMER_0
#define BUZZER_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BUZZER_LEDC_DUTY_RES    LEDC_TIMER_10_BIT   /* 0..1023 */
#define BUZZER_LEDC_DUTY_50PCT  (512)

static bool s_inited = false;

esp_err_t buzzer_init(void)
{
    ESP_LOGI(TAG, "buzzer_init: GPIO=%d", BUZZER_PIN_GPIO);

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BUZZER_LEDC_DUTY_RES,
        .timer_num = BUZZER_LEDC_TIMER,
        .freq_hz = BUZZER_DEFAULT_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num = BUZZER_PIN_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,                  /* 初始静音 */
        .hpoint = 0,
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config: %s", esp_err_to_name(ret));
        return ret;
    }

    s_inited = true;
    return ESP_OK;
}

void buzzer_beep_ms(uint32_t duration_ms)
{
    if (!s_inited || duration_ms == 0) {
        return;
    }

    /* 50% 占空比方波发声 */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, BUZZER_LEDC_DUTY_50PCT);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    /* 占空比归零静音 */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);
}
