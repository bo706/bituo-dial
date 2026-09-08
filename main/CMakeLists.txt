# =============================================================================
# main/CMakeLists.txt - 主组件构建脚本
# 创建日期: 2026-09-01
# 说明: 列出 main/ 下全部 18 个 .c 源文件；组件依赖覆盖 LVGL / mbedTLS /
#       NimBLE / Wi-Fi / HTTP Server / MQTT / mDNS / NVS / 驱动 等。
# 注意: lvgl 组件需在首次完整编译前放入 components/lvgl（LVGL v8.3，
#       任务书第 8 章），否则 CMake 配置阶段会报找不到 lvgl 组件。
# =============================================================================

idf_component_register(
    SRCS
        "app_main.c"
        "tasks.c"
        "ble_scanner.c"
        "ble_gatt_server.c"
        "crypto.c"
        "meter_store.c"
        "config.c"
        "cmd_dispatch.c"
        "sys_task.c"
        "http_server.c"
        "mqtt_client.c"
        "ui_task.c"
        "ui_overview.c"
        "ui_detail.c"
        "ui_sysinfo.c"
        "gc9a01.c"
        "encoder.c"
        "buzzer.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        # ── 底层驱动：GPIO / SPI / LEDC(PWM)（v5.2.x 统一在 driver 组件，
        #    esp_driver_gpio/esp_driver_spi 是 v5.3+ 才拆分出的组件，勿写）──
        driver
        # ── NVS 持久化 ──
        nvs_flash
        # ── 蓝牙：NimBLE（bt 组件提供 NimBLE 主机栈）──
        bt
        # ── mbedTLS：AES-128-CCM 解密 ──
        mbedtls
        # ── Wi-Fi / 网络事件 / 网络接口 ──
        esp_wifi
        esp_event
        esp_netif
        esp_coex
        # ── HTTP Server（REST API）──
        esp_http_server
        # ── MQTT 北向上报 ──
        mqtt
        # ── mDNS 局域网发现 bituo-dial.local ──
        # 注意：v5.2 起 mdns 不在 IDF 核心树内，由 main/idf_component.yml
        #       声明 espressif/mdns 托管组件，配置阶段自动下载
        mdns
        esp_timer
        # ── SPIFFS（storage 分区，内嵌 Web 配置页）──
        spiffs
        # ── cJSON：统一 JSON 命令/响应解析 ──
        json
        # ── LVGL v8.3（components/lvgl 子模块）──
        lvgl
)
