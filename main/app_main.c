/**
 * @file    app_main.c
 * @brief   Bituo Dial 固件入口：GPIO46 电源保持 -> NVS -> 各模块初始化序列
 *          （严格按任务书 5.1 节顺序）
 * @version ESP-IDF v5.2.3
 * @date    2026-09-01
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi_default.h"   /* esp_netif_create_default_wifi_ap/sta */

#include "meter_store.h"
#include "gc9a01.h"
#include "ui_task.h"        /* lvgl_init() */
#include "encoder.h"
#include "buzzer.h"
#include "crypto.h"
#include "ble_scanner.h"
#include "ble_gatt_server.h"  /* Phase2：明文配置通道 */
#include "http_server.h"      /* Phase2：/api/info */
#include "config.h"
#include "tasks.h"
#include "sys_task.h"

static const char *TAG = "app_main";

/* ── Phase0 硬件自检开关：进入 Phase1 后改为 0 ─────────────────────
 * 自检内容：蜂鸣器短鸣 + 屏幕红/绿/蓝/白四色循环 + 5 秒旋钮/按键轮询。
 * 目的：在没有任何业务逻辑前，先用人的耳朵/眼睛/手确认载板器件完好。
 */
/* Phase0 硬件自检开关：Phase3 正式 LVGL UI 已落地，开机不再跑四色自检
 * （自检用裸驱刷屏，会与 LVGL 抢屏且阻塞 6.5s）；需要复测器件时临时改回 1。 */
#define PHASE0_SELFTEST_ENABLE   0

/* RGB565 基础色 */
#define COLOR_RGB565_RED         0xF800
#define COLOR_RGB565_GREEN       0x07E0
#define COLOR_RGB565_BLUE        0x001F
#define COLOR_RGB565_WHITE       0xFFFF

#if PHASE0_SELFTEST_ENABLE
/**
 * @brief Phase0 硬件自检（阻塞约 6.5 秒，仅自检期使用）
 * @note  【需要你手动参与】听到蜂鸣器"滴"一声、看到屏幕依次显示
 *        红→绿→蓝→白（各约 400ms），随后 5 秒内旋转旋钮/按下侧键，
 *        串口应实时打印 [selftest] 行；无输出说明对应器件接线/引脚不符。
 */
static void phase0_selftest(void)
{
    ESP_LOGI(TAG, "==== Phase0 selftest START (look/listen/rotate) ====");

    /* 1) 蜂鸣器：短鸣 80ms */
    buzzer_init();
    buzzer_beep_ms(80);

    /* 2) 屏幕四色循环（颜色顺序与时长固定，便于核对通道与偏移） */
    static const struct { const char *name; uint16_t color; } colors[] = {
        { "RED",   COLOR_RGB565_RED   },
        { "GREEN", COLOR_RGB565_GREEN },
        { "BLUE",  COLOR_RGB565_BLUE  },
        { "WHITE", COLOR_RGB565_WHITE },
    };
    for (int i = 0; i < 4; i++) {
        gc9a01_fill_screen(colors[i].color);
        ESP_LOGI(TAG, "[selftest] screen -> %s", colors[i].name);
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    /* 3) 旋钮/按键：窗口时长（屏幕/蜂鸣器已验收通过；旋钮硬件待重插排线，
     *    暂用 3 秒短窗口避免每次开机长时间阻塞，排线修复并验收后可整体关闭自检）。 */

    /* 3.0) 上拉/下拉对比诊断（区分"固件读取坏"还是"编码器硬件未接通"）：
     * 静止时 EC11 两相/按键均开路。内部上拉应读到 1；切内部下拉后，
     * 若读到 0 -> GPIO 读取通路正常、外部无驱动，即编码器触点未接到引脚
     * （FPC 排线松/硬件断）；若仍恒 1 -> 引脚被外部强上拉，同样属硬件异常。 */
    ESP_LOGI(TAG, "[selftest] pull diagnostic: PULL-UP   A=%d B=%d BTN=%d (expect 1/1/1)",
             gpio_get_level(40), gpio_get_level(41), gpio_get_level(42));
    gpio_set_pull_mode(GPIO_NUM_40, GPIO_PULLDOWN_ONLY);
    gpio_set_pull_mode(GPIO_NUM_41, GPIO_PULLDOWN_ONLY);
    gpio_set_pull_mode(GPIO_NUM_42, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "[selftest] pull diagnostic: PULL-DOWN A=%d B=%d BTN=%d (0/0/0=read path OK, encoder open)",
             gpio_get_level(40), gpio_get_level(41), gpio_get_level(42));
    gpio_set_pull_mode(GPIO_NUM_40, GPIO_PULLUP_ONLY);   /* 恢复内部上拉，进入正常检测 */
    gpio_set_pull_mode(GPIO_NUM_41, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_NUM_42, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "[selftest] rotate encoder / press button in next 3s");
    bool last_btn = encoder_button_is_pressed();
    int32_t total = 0;
    for (int i = 0; i < 300; i++) {               /* 300 × 10ms = 3s 短窗口 */
        encoder_poll();
        int d = encoder_get_delta();
        if (d != 0) {
            total += d;
            ESP_LOGI(TAG, "[selftest] encoder delta=%d, accumulated=%ld", d, (long)total);
        }
        bool btn = encoder_button_is_pressed();
        if (btn != last_btn) {
            ESP_LOGI(TAG, "[selftest] button %s (GPIO42 level=%d)",
                     btn ? "PRESSED" : "RELEASED", btn ? 0 : 1);
            last_btn = btn;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "==== Phase0 selftest END (accumulated pulses=%ld) ====", (long)total);
}
#endif /* PHASE0_SELFTEST_ENABLE */

void app_main(void)
{
    /* ============================================================
     * !!! 坑点提醒（任务书 坑1 / M5Dial 官方文档 HOLD 引脚）!!!
     * GPIO46 是 M5Dial 板载电源保持(HOLD)引脚：设备被 WAKE 键/RTC
     * 唤醒后，必须由固件第一时间把 GPIO46 拉高才能维持供电，否则 PMU
     * 会在约 100ms 后切断电源，现象为"串口刚出日志就反复断电重启"。
     * 因此下面两行必须是 app_main() 的第一行可执行代码，其前严禁
     * 插入任何 ESP_LOGx / vTaskDelay / 其他初始化（无 USB 外部供电
     * 时，将 GPIO46 拉低即关机）。
     * ============================================================ */
    gpio_config_t hold_pin_cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_46,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&hold_pin_cfg);
    gpio_set_level(GPIO_NUM_46, 1);   // 锁存电源，全程保持高电平

    ESP_LOGI(TAG, "Bituo Dial boot: GPIO46 HOLD asserted HIGH, power latched");

    /* ── 1. NVS Flash 初始化（版本不匹配/无空闲页时擦除重建）── */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init returned %s, erasing NVS then re-init",
                 esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ── 2. 按任务书 5.1 固定顺序初始化各子系统 ──
     * meter_store(PSRAM) -> config_load(NVS 载入电表/网络配置)
     * -> [NVS 空时注入 Phase1 测试表] -> display(GC9A01 SPI) -> lvgl
     * -> encoder -> 自检 -> tasks -> crypto -> BLE(GATT 服务注册 -> host 启动)
     */
    meter_store_init();
    ESP_ERROR_CHECK(config_load());   /* 先从 NVS 载入真实配置 */
    /* Phase1：NVS 为空时注入真机测试槽（MAC+broadcast_key），让扫描回调直接解密；
     * NVS 已有配置时该函数自动跳过。Phase2 联调 add_meter 成功后可整体删除。 */
    meter_store_phase1_inject();
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(lvgl_init());        /* Phase3 前为占位空实现 */
    ESP_ERROR_CHECK(encoder_init());
    /* 蜂鸣器正式初始化（Phase0 自检关闭后，ui_task 的按键/上下线提示依赖它） */
    ESP_ERROR_CHECK(buzzer_init());

#if PHASE0_SELFTEST_ENABLE
    /* Phase0 硬件自检：蜂鸣器/屏幕四色/旋钮按键（Phase1 起由宏关闭） */
    phase0_selftest();
#endif

    ESP_ERROR_CHECK(tasks_start());

    /* ── Phase1：CCM 固定向量自测 → 启动 NimBLE 被动扫描 ──
     * selftest 不通过说明 mbedTLS CCM 参数有误，直接断言暴露问题；
     * 扫描器在未配置任何电表时进入 observe 模式，打印原始 5B/26B 厂商包。
     *
     * Phase2：GATT 服务必须在 nimble_port_init 之后、host 启动之前注册，
     * 因此顺序为 ble_scanner_init(port_init) -> ble_gatt_server_start(注册服务)
     * -> ble_scanner_start(host 启动，sync 后同时开始扫描+可连接广播)。 */
    ESP_ERROR_CHECK(crypto_selftest());
    ESP_ERROR_CHECK(ble_scanner_init());
    ESP_ERROR_CHECK(ble_gatt_server_start());
    ESP_ERROR_CHECK(ble_scanner_start());

    /* ── 3.0 网络基础初始化（lwIP tcpip 栈 + 默认事件循环 + 默认 netif）──
     * !!! 坑点提醒：httpd_start()/Wi-Fi 都依赖 lwIP tcpip 线程，
     * 必须先 esp_netif_init()，否则 httpd 内部 tcpip_send_msg 会
     * assert "Invalid mbox" 并重启。AP/STA 两个默认 netif 都先建好，
     * 后续 setwifi(STA) 与 SoftAP 配网可直接使用。 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    /* ── 3. Wi-Fi 策略：NVS 有凭据走 STA；无凭据直接开 SoftAP 配网 ── */
    if (config_has_wifi()) {
        ESP_LOGI(TAG, "Wi-Fi credentials found, start STA mode");
        sys_wifi_connect();
    } else {
        ESP_LOGI(TAG, "no Wi-Fi credentials, start SoftAP provisioning");
        sys_softap_start();
    }

    /* ── 4. HTTP Server：/api/info 可访问（STA/SoftAP 均监听 0.0.0.0:80）── */
    ESP_ERROR_CHECK(http_server_start());
}
