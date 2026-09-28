/**
 * @file    gc9a01.c
 * @brief   GC9A01 圆形屏 SPI 驱动框架：SPI 总线初始化、命令/数据写入、
 *          上电初始化序列框架、LVGL flush 回调
 * @note    初始化序列只给安全的最小框架；完整寄存器序列需对照
 *          M5Stack 官方 Arduino BSP（M5Dial）实测，尤其 CASET/RASET
 *          offset，否则会出现图像偏移/四角异常像素（任务书 坑7）。
 * @version ESP-IDF v5.2.3 / LVGL v8.3
 * @date    2026-09-01
 */
#include "gc9a01.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "gc9a01";

static spi_device_handle_t s_spi_dev = NULL;

/* ── 底层：一次 SPI 轮询传输 ─────────────────────────────────────── */
static void spi_tx(const uint8_t *buf, size_t len)
{
    if (s_spi_dev == NULL || buf == NULL || len == 0) {
        return;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = buf,
    };
    spi_device_polling_transmit(s_spi_dev, &t);
}

void gc9a01_write_cmd(uint8_t cmd)
{
    gpio_set_level(GC9A01_PIN_DC, 0);   /* DC=0：命令 */
    spi_tx(&cmd, 1);
}

void gc9a01_write_data(const uint8_t *data, uint32_t len)
{
    gpio_set_level(GC9A01_PIN_DC, 1);   /* DC=1：数据/参数 */
    spi_tx(data, len);
}

static void write_data_byte(uint8_t b)
{
    gpio_set_level(GC9A01_PIN_DC, 1);
    spi_tx(&b, 1);
}

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ── 硬件硬复位（RST=GPIO8）──────────────────────────────────────── */
static void gc9a01_hard_reset(void)
{
    gpio_set_level(GC9A01_PIN_RST, 1);
    delay_ms(10);
    gpio_set_level(GC9A01_PIN_RST, 0);
    delay_ms(20);
    gpio_set_level(GC9A01_PIN_RST, 1);
    delay_ms(120);
}

/* ── 上电初始化序列 ──────────────────────────────────────────────── */
/* 单条命令：命令字节 + 若干参数 + 发送后延时(ms)，延时 0 表示不等待 */
typedef struct {
    uint8_t cmd;
    uint8_t args[12];
    uint8_t nargs;
    uint8_t delay_ms;
} gc9a01_init_cmd_t;

static void gc9a01_init_panel(void)
{
    /* !!! 坑点提醒（任务书 坑7）!!!
     * 下列厂商私有寄存器序列来自乐鑫官方 esp-bsp 组件
     * components/lcd/esp_lcd_gc9a01/esp_lcd_gc9a01.c 的
     * vendor_specific_init_default[]（GC9A01 点屏必需的电源/泵/伽马序列）。
     * 早期"最小框架"只发 SLPOUT/COLMOD/INVON/DISPON，因未配置内部电源
     * 与伽马，面板保持全黑——黑屏根因即缺这段序列。
     * 圆形面板原生 240x240，CASET/RASET offset=0，无需坐标偏移。
     */
    static const gc9a01_init_cmd_t seq[] = {
        { 0xFE, { 0x00 },                         1, 0 },   /* Enable Inter Register */
        { 0xEF, { 0x00 },                         1, 0 },
        { 0xEB, { 0x14 },                         1, 0 },
        { 0x84, { 0x60 },                         1, 0 },
        { 0x85, { 0xFF },                         1, 0 },
        { 0x86, { 0xFF },                         1, 0 },
        { 0x87, { 0xFF },                         1, 0 },
        { 0x8E, { 0xFF },                         1, 0 },
        { 0x8F, { 0xFF },                         1, 0 },
        { 0x88, { 0x0A },                         1, 0 },
        { 0x89, { 0x23 },                         1, 0 },
        { 0x8A, { 0x00 },                         1, 0 },
        { 0x8B, { 0x80 },                         1, 0 },
        { 0x8C, { 0x01 },                         1, 0 },
        { 0x8D, { 0x03 },                         1, 0 },
        { 0x90, { 0x08, 0x08, 0x08, 0x08 },       4, 0 },
        { 0xFF, { 0x60, 0x01, 0x04 },             3, 0 },
        { 0xC3, { 0x13 },                         1, 0 },   /* Power Control */
        { 0xC4, { 0x13 },                         1, 0 },
        { 0xC9, { 0x30 },                         1, 0 },
        { 0xBE, { 0x11 },                         1, 0 },
        { 0xE1, { 0x10, 0x0E },                   2, 0 },
        { 0xDF, { 0x21, 0x0C, 0x02 },             3, 0 },
        { 0xF0, { 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A }, 6, 0 }, /* Gamma */
        { 0xF1, { 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F }, 6, 0 },
        { 0xF2, { 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A }, 6, 0 },
        { 0xF3, { 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F }, 6, 0 },
        { 0xED, { 0x1B, 0x0B },                   2, 0 },
        { 0xAE, { 0x77 },                         1, 0 },
        { 0xCD, { 0x63 },                         1, 0 },
        { 0x70, { 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03 }, 9, 0 },
        { 0xE8, { 0x34 },                         1, 0 },   /* 4-dot inversion */
        { 0x60, { 0x38, 0x0B, 0x6D, 0x6D, 0x39, 0xF0, 0x6D, 0x6D }, 8, 0 },
        { 0x61, { 0x38, 0xF4, 0x6D, 0x6D, 0x38, 0xF7, 0x6D, 0x6D }, 8, 0 },
        { 0x62, { 0x38, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x38, 0x0F,
                  0x71, 0xEF, 0x70, 0x70 },      12, 0 },
        { 0x63, { 0x38, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x38, 0x13,
                  0x71, 0xF3, 0x70, 0x70 },      12, 0 },
        { 0x64, { 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07 }, 7, 0 },
        { 0x66, { 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00 }, 10, 0 },
        { 0x67, { 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98 }, 10, 0 },
        { 0x74, { 0x10, 0x45, 0x80, 0x00, 0x00, 0x4E, 0x00 }, 7, 0 },
        { 0x98, { 0x3E, 0x07 },                   2, 0 },
        { 0x99, { 0x3E, 0x07 },                   2, 0 },
    };

    gc9a01_hard_reset();

    gc9a01_write_cmd(0x11);                 /* SLPOUT 退出睡眠（官方要求后等 100ms） */
    delay_ms(100);

    /* MADCTL：USB 朝下为正立握持，0x48 = MX|BGR（自检 idx1，箭头朝三角）。
     * bit3=BGR 必须保留，去掉会整屏偏色。 */
    gc9a01_set_madctl(GC9A01_MADCTL_DEFAULT);

    gc9a01_write_cmd(0x3A);                 /* COLMOD 16-bit RGB565 */
    write_data_byte(0x05);

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        gc9a01_write_cmd(seq[i].cmd);
        if (seq[i].nargs > 0) {
            gc9a01_write_data(seq[i].args, seq[i].nargs);
        }
        if (seq[i].delay_ms > 0) {
            delay_ms(seq[i].delay_ms);
        }
    }

    gc9a01_write_cmd(0x21);                 /* INVON 显示反转（GC9A01 必需） */
    delay_ms(10);

    gc9a01_write_cmd(0x29);                 /* DISPON 开显示 */
    delay_ms(20);
}

void gc9a01_set_madctl(uint8_t madctl)
{
    if ((madctl & GC9A01_MADCTL_BGR) == 0u) {
        ESP_LOGW(TAG, "MADCTL 0x%02X missing BGR bit, forcing |0x08", madctl);
        madctl = (uint8_t)(madctl | GC9A01_MADCTL_BGR);
    }
    gc9a01_write_cmd(0x36);
    write_data_byte(madctl);
    ESP_LOGI(TAG, "MADCTL set to 0x%02X", madctl);
}

/* ── 设置像素写入窗口（flush 使用）───────────────────────────────── */
static void gc9a01_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    /* !!! 坑7：若真机图像偏移，需要在这里对 x/y 加 offset（以 BSP 实测为准） */
    uint8_t col[4] = { (uint8_t)(x1 >> 8), (uint8_t)x1,
                       (uint8_t)(x2 >> 8), (uint8_t)x2 };
    uint8_t row[4] = { (uint8_t)(y1 >> 8), (uint8_t)y1,
                       (uint8_t)(y2 >> 8), (uint8_t)y2 };

    gc9a01_write_cmd(0x2A);             /* CASET 列地址 */
    gc9a01_write_data(col, sizeof(col));
    gc9a01_write_cmd(0x2B);             /* RASET 行地址 */
    gc9a01_write_data(row, sizeof(row));
    gc9a01_write_cmd(0x2C);             /* RAMWR 写显存 */
}

/* ── SPI 总线与设备初始化 ────────────────────────────────────────── */
static esp_err_t spi_bus_init(void)
{
    /* DC/RST/BL 作为普通 GPIO 输出；CS 交给 SPI 控制器硬件管理 */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << GC9A01_PIN_DC) |
                        (1ULL << GC9A01_PIN_RST) |
                        (1ULL << GC9A01_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    spi_bus_config_t buscfg = {
        .mosi_io_num = GC9A01_PIN_MOSI,
        .miso_io_num = -1,              /* 屏只写不读 */
        .sclk_io_num = GC9A01_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = GC9A01_WIDTH * GC9A01_HEIGHT * 2,
    };
    esp_err_t ret = spi_bus_initialize(GC9A01_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = GC9A01_SPI_FREQ_HZ,
        .mode = 0,                       /* SPI mode 0：CPOL=0 CPHA=0 */
        .spics_io_num = GC9A01_PIN_CS,   /* CS=GPIO7 硬件片选 */
        .queue_size = 6,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    ret = spi_bus_add_device(GC9A01_SPI_HOST, &devcfg, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "display_init: GC9A01 240x240 round panel");

    ESP_ERROR_CHECK(spi_bus_init());
    gc9a01_init_panel();

    /* 打开背光（GPIO9） */
    gpio_set_level(GC9A01_PIN_BL, 1);

    ESP_LOGI(TAG, "display_init done");
    return ESP_OK;
}

/* ── LVGL flush 回调框架（Phase3 对接 LVGL，此处已可同步推送）────── */
void gc9a01_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    size_t w = (size_t)(area->x2 - area->x1 + 1);
    size_t h = (size_t)(area->y2 - area->y1 + 1);

    gc9a01_set_window((uint16_t)area->x1, (uint16_t)area->y1,
                      (uint16_t)area->x2, (uint16_t)area->y2);

    gpio_set_level(GC9A01_PIN_DC, 1);
    spi_transaction_t t = {
        .length = w * h * 16,
        .tx_buffer = color_map,
    };
    spi_device_polling_transmit(s_spi_dev, &t);

    /* 必须通知 LVGL 本块刷新完成（同步传输，传完即 ready） */
    lv_disp_flush_ready(drv);
}

/* ── Phase0 整屏纯色测试（验证接线与 offset；正式版本可删除）─────── */
esp_err_t gc9a01_fill_screen(uint16_t rgb565)
{
    static uint16_t *s_line = NULL;
    if (s_line == NULL) {
        /* DMA 缓冲放内部 RAM（MALLOC_CAP_DMA），一行 240 像素 */
        s_line = heap_caps_malloc(GC9A01_WIDTH * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (s_line == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* !!! 坑点提醒（RGB565 字节序）!!!
     * ESP32 为小端，uint16 在内存中是"低字节在前"；SPI 默认 MSB first，
     * GC9A01 要求 16bit 像素"高字节在前"，不交换会出现红蓝通道错位、
     * 颜色发暗。故每个像素先做高低字节交换再发送。
     * 注意：Phase3 接入 LVGL 后，渲染缓冲由 LVGL 输出，届时必须打开
     * CONFIG_LV_COLOR_16_SWAP=y，由 LVGL 统一交换，本函数仅裸驱使用。
     */
    uint16_t swapped = (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
    for (int x = 0; x < GC9A01_WIDTH; x++) {
        s_line[x] = swapped;   /* 每次调用都刷新，保证可连续切换红/绿/蓝 */
    }

    gc9a01_set_window(0, 0, GC9A01_WIDTH - 1, GC9A01_HEIGHT - 1);
    gpio_set_level(GC9A01_PIN_DC, 1);
    for (int y = 0; y < GC9A01_HEIGHT; y++) {
        spi_transaction_t t = {
            .length = GC9A01_WIDTH * 16,
            .tx_buffer = s_line,
        };
        spi_device_polling_transmit(s_spi_dev, &t);
    }
    return ESP_OK;
}
