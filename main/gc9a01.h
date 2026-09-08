/**
 * @file    gc9a01.h
 * @brief   M5Dial 1.28 寸圆形 GC9A01 TFT 的 SPI 驱动（LVGL flush_cb 后端）
 *          引脚：MOSI=5, CLK(SCK)=6, CS=7, DC(RS)=4, RST=8, BL=9
 *          （以乐鑫官方 esp-bsp m5dial BSP 与 M5Unified PinMap 为准）
 * @version ESP-IDF v5.2.3 / LVGL v8.3
 * @date    2026-09-01
 */
#ifndef MAIN_GC9A01_H_
#define MAIN_GC9A01_H_

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"   /* flush 回调使用 lv_disp_drv_t / lv_area_t / lv_color_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── 引脚定义 ─────────────────────────────────────────────────────
 * !!! 坑点提醒（任务书第二处实测错误，第一处为 PSRAM）!!!
 * 任务书 2.2 写的是 MOSI=6/CLK=5，与真机相反；经乐鑫官方 esp-bsp
 * m5dial BSP（BSP_LCD_MOSI=GPIO5 / BSP_LCD_PCLK=GPIO6）、M5Unified、
 * espboards、ESP32_Display_Panel 多方一致确认，真机为 MOSI=5、SCK=6。
 * MOSI/SCK 接反时 SPI 时钟与数据错位，面板收不到任何命令，表现为全黑。
 * CS=7 / DC=4 / RST=8 / BL=9 与任务书一致，无需改动。
 */
#define GC9A01_PIN_MOSI      5
#define GC9A01_PIN_CLK       6
#define GC9A01_PIN_CS        7
#define GC9A01_PIN_DC        4    /* LCD_RS / 数据命令选择 */
#define GC9A01_PIN_RST       8
#define GC9A01_PIN_BL        9    /* 背光：高电平点亮，固件不主动开则一直灭 */

#define GC9A01_SPI_HOST      SPI2_HOST
#define GC9A01_WIDTH         240
#define GC9A01_HEIGHT        240
#define GC9A01_SPI_FREQ_HZ   (40 * 1000 * 1000)

/* MADCTL bit3 = BGR，本屏必须置 1，去掉会整屏偏色（已真机验证）。 */
#define GC9A01_MADCTL_BGR        0x08u
#define GC9A01_MADCTL_MX         0x40u
#define GC9A01_MADCTL_MY         0x80u
/* 2026-09-04：用户握持 USB 口朝下看屏。
 * 2026-09-03 四档自检：idx1 0x48（MX|BGR）文字正、箭头朝三角（USB 对面）。
 * idx3 0xC8 在 USB 朝下时仍颠倒，已证伪。BGR 必须保留。 */
#define GC9A01_MADCTL_DEFAULT    (GC9A01_MADCTL_MX | GC9A01_MADCTL_BGR)

/**
 * @brief 初始化 SPI 总线、SPI 设备、控制引脚与 GC9A01 上电序列
 * @note  对应任务书中的 display_init()，由 app_main 调用
 */
esp_err_t display_init(void);

/* ── 底层写命令/写数据（供初始化序列与 flush 使用）── */
void gc9a01_write_cmd(uint8_t cmd);
void gc9a01_write_data(const uint8_t *data, uint32_t len);

/**
 * @brief 运行中改写 MADCTL（命令 0x36）。
 * @note  bit3(BGR) 若未置位会强制补上。调用后必须整屏 invalidate + lv_refr_now，
 *        否则面板显存按新方向映射、画面会错位。
 */
void gc9a01_set_madctl(uint8_t madctl);

/**
 * @brief LVGL 显示刷新回调（任务书 5.5.2：disp_drv.flush_cb = gc9a01_flush）
 */
void gc9a01_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);

/**
 * @brief Phase0 测试辅助：整屏填充一种 RGB565 颜色（验证屏幕方向/offset）
 */
esp_err_t gc9a01_fill_screen(uint16_t rgb565);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_GC9A01_H_ */
