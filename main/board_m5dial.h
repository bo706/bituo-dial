/**
 * @file    board_m5dial.h
 * @brief   当前样机板级参数：M5Dial（ESP32-S3 + GC9A01 240 圆屏）
 *
 * 换另一块 S3 屏时只改本文件（引脚、分辨率、MADCTL、电源保持脚）。
 * 不要把新引脚写进 ui_*.c、ble_scanner、mqtt_client、meter_store。
 * 面板控制器不是 GC9A01 时，再替换 gc9a01.c，保持 display_init / gc9a01_flush 这两个入口名。
 */
#ifndef MAIN_BOARD_M5DIAL_H_
#define MAIN_BOARD_M5DIAL_H_

#define BOARD_NAME              "m5dial-gc9a01"

/* 圆屏 SPI。任务书 MOSI/SCK 写反了，真机以本表为准。 */
#define BOARD_LCD_MOSI          5
#define BOARD_LCD_CLK           6
#define BOARD_LCD_CS            7
#define BOARD_LCD_DC            4
#define BOARD_LCD_RST           8
#define BOARD_LCD_BL            9

#define BOARD_LCD_W             240
#define BOARD_LCD_H             240
/* USB 朝下、橙色圈三角朝上。bit3(BGR)=1 必须留着，否则偏色。 */
#define BOARD_LCD_MADCTL        0x48u

/* 旋钮 A/B、侧键。按下为低。 */
#define BOARD_ENC_A             40
#define BOARD_ENC_B             41
#define BOARD_ENC_BTN           42

#define BOARD_BUZZER_GPIO       3

/* 上电第一件事拉高，否则约 100ms 断电。无 USB 供电时拉低即关机。 */
#define BOARD_PWR_HOLD_GPIO     46

#endif /* MAIN_BOARD_M5DIAL_H_ */
