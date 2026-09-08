/**
 * @file    ui_task.c
 * @brief   LVGL 主循环（任务书 5.4/5.5）：
 *            - lvgl_init()：lv_init + 1ms tick + 双帧缓冲 + 注册 GC9A01 flush
 *            - ui_task：Core1/prio5/栈8192，编码器采集、三页调度、按键短按/
 *              长按状态机、15~90s 黄显/90s 离线检测、上/下线蜂鸣、lv_timer_handler
 * @note    !!! 坑点提醒（任务书坑3）：全工程只有本任务允许调用 lv_* API。
 *          本机无 PSRAM（任务书描述有误），帧缓冲改用内部 DMA RAM。
 * @version LVGL v8.3 / ESP-IDF v5.2.3
 * @date    2026-09-01  @modified 2026-09-03 Phase3 收尾：方向自检 + FPS
 */
#include "ui_task.h"

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "gc9a01.h"
#include "encoder.h"
#include "buzzer.h"
#include "meter_store.h"
#include "ui_overview.h"
#include "ui_detail.h"
#include "ui_sysinfo.h"

static const char *TAG = "ui_task";

/* ── 时序参数 ─────────────────────────────────────────────────────── */
#define UI_LOOP_PERIOD_MS     5      /* 主循环/编码器轮询周期 5ms（1~5ms） */
#define UI_REFRESH_PERIOD_MS  200    /* 数据刷新周期 200ms（≤1s 要求，留余量） */
#define UI_BTN_DEBOUNCE_CNT   3      /* 连续 3×5ms=15ms 一致才确认消抖 */
#define UI_BTN_LONG_MS        800    /* 长按阈值 */

/* 分块刷新行数：240×20×2B ≈ 9.6KB/块（无 PSRAM，用内部 DMA RAM） */
#define UI_DRAW_LINES         20

static TaskHandle_t s_task_handle = NULL;
static ui_page_t    s_page = UI_PAGE_OVERVIEW;

static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t      s_disp_drv;

/* LVGL 8.3 monitor_cb 累计：每个真正发生刷新的 refr 周期 +1（不是 SPI 分条次数） */
static uint32_t s_fps_frames;
static uint32_t s_fps_px;
static uint32_t s_fps_ms;

static void disp_monitor_cb(lv_disp_drv_t *drv, uint32_t time, uint32_t px)
{
    (void)drv;
    s_fps_frames++;
    s_fps_px += px;
    s_fps_ms += time;
}

static void ui_fps_log(const char *mode)
{
    uint32_t n  = s_fps_frames;
    uint32_t px = s_fps_px;
    uint32_t ms = s_fps_ms;
    s_fps_frames = 0;
    s_fps_px = 0;
    s_fps_ms = 0;
    ESP_LOGI(TAG, "FPS=%lu mode=%s frames=%lu px=%lu render_ms=%lu",
             (unsigned long)n, mode,
             (unsigned long)n, (unsigned long)px, (unsigned long)ms);
}

#if BITUO_ORIENT_SELFTEST
static void ui_delay_pump(uint32_t ms)
{
    /* 自检/探针等待期间继续跑 lv_timer_handler，且 delay≥1 tick，避免饿死 IDLE1 */
    TickType_t t0 = xTaskGetTickCount();
    while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(ms)) {
        lv_timer_handler();
        TickType_t wait = pdMS_TO_TICKS(UI_LOOP_PERIOD_MS);
        vTaskDelay(wait > 0 ? wait : 1);
    }
}

static void ui_force_full_refresh(lv_obj_t *scr)
{
    if (scr != NULL) {
        lv_obj_invalidate(scr);
    }
    lv_refr_now(NULL);
}
#endif

#if BITUO_ORIENT_SELFTEST
#if LV_FONT_MONTSERRAT_28
#define UI_ORIENT_BIG_FONT  (&lv_font_montserrat_28)
#else
#define UI_ORIENT_BIG_FONT  (&lv_font_montserrat_20)
#endif

/* 四个候选均含 BGR=0x08：0x08 / MX|BGR / MY|BGR / MY|MX|BGR */
static const uint8_t s_orient_madctl[4] = {
    (uint8_t)(GC9A01_MADCTL_BGR),
    (uint8_t)(GC9A01_MADCTL_MX | GC9A01_MADCTL_BGR),
    (uint8_t)(GC9A01_MADCTL_MY | GC9A01_MADCTL_BGR),
    (uint8_t)(GC9A01_MADCTL_MY | GC9A01_MADCTL_MX | GC9A01_MADCTL_BGR),
};

static void ui_orient_selftest(void)
{
    ESP_LOGI(TAG, "==== orient selftest START (4x3s, MADCTL 0x08/48/88/C8) ====");

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 箭头指向 LVGL 坐标系的屏幕上方（y 减小方向）。哪一档物理朝上即正立。
     * 点数组必须静态：lv_line 只保存指针。 */
    static lv_point_t shaft[2];
    static lv_point_t head[3];
    shaft[0].x = 120; shaft[0].y = 72;
    shaft[1].x = 120; shaft[1].y = 28;
    head[0].x = 88;  head[0].y = 50;
    head[1].x = 120; head[1].y = 16;
    head[2].x = 152; head[2].y = 50;

    lv_obj_t *ln_shaft = lv_line_create(scr);
    lv_line_set_points(ln_shaft, shaft, 2);
    lv_obj_set_style_line_width(ln_shaft, 10, 0);
    lv_obj_set_style_line_color(ln_shaft, UI_C_ACCENT, 0);
    lv_obj_set_style_line_rounded(ln_shaft, true, 0);

    lv_obj_t *ln_head = lv_line_create(scr);
    lv_line_set_points(ln_head, head, 3);
    lv_obj_set_style_line_width(ln_head, 10, 0);
    lv_obj_set_style_line_color(ln_head, UI_C_ACCENT, 0);
    lv_obj_set_style_line_rounded(ln_head, true, 0);

    lv_obj_t *up = lv_label_create(scr);
    lv_label_set_text(up, "THIS SIDE UP");
    lv_obj_set_style_text_font(up, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(up, UI_C_TEXT, 0);
    lv_obj_align(up, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t *num = lv_label_create(scr);
    lv_obj_set_style_text_font(num, UI_ORIENT_BIG_FONT, 0);
    lv_obj_set_style_text_color(num, UI_C_TEXT, 0);

    lv_obj_t *hex = lv_label_create(scr);
    lv_obj_set_style_text_font(hex, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(hex, UI_C_ACCENT, 0);

    lv_scr_load(scr);

    for (int i = 0; i < 4; i++) {
        uint8_t m = s_orient_madctl[i];
        gc9a01_set_madctl(m);
        lv_label_set_text_fmt(num, "%d", i);
        lv_label_set_text_fmt(hex, "MADCTL 0x%02X", m);
        lv_obj_align(num, LV_ALIGN_CENTER, 0, 16);
        lv_obj_align(hex, LV_ALIGN_CENTER, 0, 56);
        ui_force_full_refresh(scr);
        ESP_LOGI(TAG, "orient selftest idx=%d MADCTL=0x%02X — look for upright arrow+text",
                 i, (unsigned)m);
        ui_delay_pump(3000);
    }

    gc9a01_set_madctl(GC9A01_MADCTL_DEFAULT);
    lv_scr_load(ui_overview_screen());
    lv_obj_del(scr);
    s_page = UI_PAGE_OVERVIEW;
    ui_overview_refresh();
    ui_force_full_refresh(ui_overview_screen());
    ESP_LOGI(TAG, "==== orient selftest END, enter overview (MADCTL back to 0x%02X) ====",
             (unsigned)GC9A01_MADCTL_DEFAULT);
}
#endif /* BITUO_ORIENT_SELFTEST */

#if BITUO_FPS_PROBE
static void ui_fps_probe(void)
{
    /* 任务书 11.3 测的是渲染能力，不是 200ms 数据轮询。满屏 invalidate
     * 让 LVGL 按 LV_DISP_DEF_REFR_PERIOD(30ms) 持续刷新，上限约 33FPS。 */
    ESP_LOGI(TAG, "==== FPS probe START (5s full-invalidate, pass if FPS>=20) ====");
    s_fps_frames = 0;
    s_fps_px = 0;
    s_fps_ms = 0;

    lv_obj_t *scr = lv_scr_act();
    TickType_t t0 = xTaskGetTickCount();
    TickType_t last_log = t0;
    while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(5000)) {
        if (scr != NULL) {
            lv_obj_invalidate(scr);
        }
        lv_timer_handler();
        TickType_t now = xTaskGetTickCount();
        if ((now - last_log) >= pdMS_TO_TICKS(1000)) {
            last_log = now;
            ui_fps_log("probe");
        }
        TickType_t wait = pdMS_TO_TICKS(UI_LOOP_PERIOD_MS);
        vTaskDelay(wait > 0 ? wait : 1);
    }
    if (s_fps_frames > 0) {
        ui_fps_log("probe");
    }
    ESP_LOGI(TAG, "==== FPS probe END ====");
}
#endif /* BITUO_FPS_PROBE */

/* ── 1ms LVGL tick（esp_timer 回调，仅做轻量自增，禁止阻塞）────────── */
static void lv_tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

/* 分配一块 DMA 可用帧缓冲（SPI DMA 要求内部 RAM；失败再退普通内部 RAM） */
static lv_color_t *alloc_draw_buf(size_t pixels)
{
    size_t bytes = pixels * sizeof(lv_color_t);
    lv_color_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
    if (buf == NULL) {
        ESP_LOGW(TAG, "DMA-cap buf alloc failed, fallback to internal RAM");
        buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return buf;
}

esp_err_t lvgl_init(void)
{
    ESP_LOGI(TAG, "lvgl_init: lv_init + tick + draw buffers + display driver");

    lv_init();

    /* 1ms tick 定时器 */
    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_timer_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer;
    esp_err_t err = esp_timer_create(&tick_args, &tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create: %s", esp_err_to_name(err));
        return err;
    }
    esp_timer_start_periodic(tick_timer, 1000);   /* 1000us = 1ms */

    /* 双帧缓冲：!!! 任务书写 MALLOC_CAP_SPIRAM，但本机无 PSRAM，改 DMA RAM !!! */
    size_t pixels = UI_SCREEN_SIZE * UI_DRAW_LINES;
    lv_color_t *buf1 = alloc_draw_buf(pixels);
    lv_color_t *buf2 = alloc_draw_buf(pixels);
    if (buf1 == NULL) {
        ESP_LOGE(TAG, "draw buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    lv_disp_draw_buf_init(&s_disp_buf, buf1, buf2, pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = UI_SCREEN_SIZE;
    s_disp_drv.ver_res  = UI_SCREEN_SIZE;
    s_disp_drv.flush_cb = gc9a01_flush;       /* 同步 SPI 刷新，内部已 flush_ready */
    s_disp_drv.monitor_cb = disp_monitor_cb;  /* LVGL 8.3：每个 refr 周期回调一次 */
    s_disp_drv.draw_buf = &s_disp_buf;
    lv_disp_drv_register(&s_disp_drv);

    /* 创建三页 screen（仅创建控件，不渲染业务数据） */
    ui_overview_create();
    ui_detail_create();
    ui_sysinfo_create();

    /* 默认进入列表概览 */
    lv_scr_load(ui_overview_screen());
    s_page = UI_PAGE_OVERVIEW;

    ESP_LOGI(TAG, "lvgl_init done (RGB565 swap by Kconfig, %u px/buf x2)",
             (unsigned)pixels);
    return ESP_OK;
}

/* ── 页面切换：仅在 ui_task 调用 lv_scr_load（任务书坑3）────────────── */
static void ui_switch_page(ui_page_t page)
{
    lv_obj_t *scr = NULL;
    switch (page) {
    case UI_PAGE_OVERVIEW: scr = ui_overview_screen(); break;
    case UI_PAGE_DETAIL:   scr = ui_detail_screen();   break;
    case UI_PAGE_SYSINFO:  scr = ui_sysinfo_screen();  break;
    default: return;
    }
    s_page = page;
    lv_scr_load(scr);
    /* 切页立即刷新一次，避免残留上一帧 */
    switch (page) {
    case UI_PAGE_OVERVIEW: ui_overview_refresh(); break;
    case UI_PAGE_DETAIL:   ui_detail_refresh();   break;
    case UI_PAGE_SYSINFO:  ui_sysinfo_refresh();  break;
    default: break;
    }
    ESP_LOGI(TAG, "switch to page %d", (int)page);
}

/* ── 按键短按/长按事件（按当前页上下文分发）────────────────────────── */
static void ui_on_short_press(void)
{
    buzzer_beep_ms(40);   /* 短按轻反馈 */
    switch (s_page) {
    /* 概览：短按进入选中表详情；详情→系统→概览 构成三页循环 */
    case UI_PAGE_OVERVIEW:
        ui_detail_show(ui_overview_selected());
        ui_switch_page(UI_PAGE_DETAIL);
        break;
    case UI_PAGE_DETAIL:
        ui_switch_page(UI_PAGE_SYSINFO);
        break;
    case UI_PAGE_SYSINFO:
        ui_switch_page(UI_PAGE_OVERVIEW);
        break;
    default: break;
    }
}

static void ui_on_long_press(void)
{
    buzzer_beep_ms(80);   /* 长按确认反馈 */
    switch (s_page) {
    case UI_PAGE_DETAIL:
        ui_switch_page(UI_PAGE_OVERVIEW);    /* 详情长按返回列表 */
        break;
    case UI_PAGE_SYSINFO:
        ui_sysinfo_long_press();             /* 系统页长按进 SoftAP */
        break;
    case UI_PAGE_OVERVIEW:
    default: break;
    }
}

static void ui_cycle_page(int dir)
{
    int p = (int)s_page + dir;
    if (p < 0) {
        p = UI_PAGE_NUM - 1;
    } else if (p >= UI_PAGE_NUM) {
        p = 0;
    }
    ui_switch_page((ui_page_t)p);
}

/* 旋钮：系统页始终翻三页；仅 1 块表时概览/详情也翻页（否则选中不变，像没反应）。
 * 多表时概览移动窗口选中行，详情切表。 */
static void ui_on_rotate(int dir)
{
    if (dir == 0) {
        return;
    }
    if (s_page == UI_PAGE_SYSINFO || g_meter_count <= 1) {
        ui_cycle_page(dir > 0 ? 1 : -1);
        return;
    }
    if (s_page == UI_PAGE_OVERVIEW) {
        ui_overview_select_delta(dir > 0 ? 1 : -1);
        ui_overview_refresh();
    } else if (s_page == UI_PAGE_DETAIL) {
        ui_detail_step(dir > 0 ? 1 : -1);
        ui_detail_refresh();
    }
}

/* ── 离线检测 + 上/下线蜂鸣（任务书 5.5.3/5.5.4，阈值见 ui_task.h）────
 * 在线: valid && age<=WARN；WARN~OFFLINE 黄色(页面处理,不响)；>OFFLINE
 * 离线(置 valid=0 写回队列并两声提示)；离线→在线一声提示。首轮只建基线。 */
static bool s_baseline[MAX_METERS];
static bool s_was_online[MAX_METERS];

static void ui_offline_monitor(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);

    for (int i = 0; i < g_meter_count && i < MAX_METERS; i++) {
        meter_data_t d;
        if (xQueuePeek(g_meter_queues[i], &d, 0) != pdTRUE) {
            continue;   /* 从未收到数据，不参与 */
        }
        uint32_t age = now - d.timestamp;
        bool online  = d.valid && age <= UI_AGE_WARN_S;
        bool offline = (!d.valid) || age > UI_AGE_OFFLINE_S;

        /* 超时：把 valid 置 false 写回深度1队列（保留其余字段） */
        if (age > UI_AGE_OFFLINE_S && d.valid) {
            d.valid = false;
            xQueueOverwrite(g_meter_queues[i], &d);
        }

        if (!s_baseline[i]) {
            s_baseline[i] = true;
            s_was_online[i] = online;
            continue;
        }

        if (s_was_online[i] && offline) {
            /* 在线 → 离线：响100-停100-响100（阻塞约300ms，偶发可接受） */
            ESP_LOGW(TAG, "meter[%d] OFFLINE (age=%lus), beep x2", i,
                     (unsigned long)age);
            buzzer_beep_ms(100);
            vTaskDelay(pdMS_TO_TICKS(100));
            buzzer_beep_ms(100);
            s_was_online[i] = false;
        } else if (!s_was_online[i] && online) {
            ESP_LOGI(TAG, "meter[%d] ONLINE, beep x1", i);
            buzzer_beep_ms(100);
            s_was_online[i] = true;
        }
    }
}

/* ── ui_task 主循环 ───────────────────────────────────────────────── */
static void ui_task_entry(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ui_task running on core %d, prio %d",
             (int)xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

#if BITUO_ORIENT_SELFTEST
    ui_orient_selftest();
#endif
#if BITUO_FPS_PROBE
    ui_fps_probe();
#endif

    /* 按键消抖/短长按状态机 */
    bool btn_confirmed = false;      /* 消抖后的当前按下状态 */
    bool btn_last = false;           /* 上一次确认状态，用于边沿 */
    int  debounce = 0;
    bool long_fired = false;
    TickType_t press_start = 0;

    TickType_t last_refresh = xTaskGetTickCount();
    TickType_t last_diag    = xTaskGetTickCount();   /* 编码器原始电平诊断节拍 */

    for (;;) {
        /* 1) 编码器采集与旋转分发 */
        encoder_poll();
        int delta = encoder_get_delta();
        if (delta != 0) {
            ESP_LOGI(TAG, "rotate delta=%d page=%d meters=%d",
                     delta, (int)s_page, (int)g_meter_count);
            ui_on_rotate(delta);
        }

        /* 2) 按键消抖（连续 N 次一致才翻转） */
        bool raw = encoder_button_is_pressed();
        if (raw == btn_confirmed) {
            debounce = 0;
        } else {
            debounce++;
            if (debounce >= UI_BTN_DEBOUNCE_CNT) {
                btn_confirmed = raw;
                debounce = 0;
            }
        }

        /* 按下沿：记录起始时刻 */
        if (btn_confirmed && !btn_last) {
            press_start = xTaskGetTickCount();
            long_fired = false;
        }
        /* 按住中：到达长按阈值触发一次 */
        if (btn_confirmed && !long_fired &&
            (xTaskGetTickCount() - press_start) >= pdMS_TO_TICKS(UI_BTN_LONG_MS)) {
            long_fired = true;
            ui_on_long_press();
        }
        /* 释放沿：未触发过长按则为短按 */
        if (!btn_confirmed && btn_last) {
            if (!long_fired) {
                ui_on_short_press();
            }
        }
        btn_last = btn_confirmed;

        /* 3) 周期刷新当前页 + 离线监测/蜂鸣 */
        TickType_t now_tick = xTaskGetTickCount();
        if ((now_tick - last_refresh) >= pdMS_TO_TICKS(UI_REFRESH_PERIOD_MS)) {
            last_refresh = now_tick;
            switch (s_page) {
            case UI_PAGE_OVERVIEW: ui_overview_refresh(); break;
            case UI_PAGE_DETAIL:   ui_detail_refresh();   break;
            case UI_PAGE_SYSINFO:  ui_sysinfo_refresh();  break;
            default: break;
            }
            ui_offline_monitor();
        }

        /* 3.5) 每 1s 打印一次编码器原始电平诊断（排查旋转无响应：toggles 不涨=硬件无信号） */
        TickType_t t_diag = xTaskGetTickCount();
        if ((t_diag - last_diag) >= pdMS_TO_TICKS(1000)) {
            last_diag = t_diag;
            uint32_t ta = 0, tb = 0;
            int la = -1, lb = -1;
            encoder_diag_consume(&ta, &tb, &la, &lb);
            ESP_LOGI(TAG, "enc diag: idle A=%d B=%d | toggles in last 1s A=%lu B=%lu",
                     la, lb, (unsigned long)ta, (unsigned long)tb);
            ui_fps_log("live");
        }

        /* 4) LVGL 渲染/刷新（内部按脏区域驱动 gc9a01_flush） */
        lv_timer_handler();

        /* !!! 坑点提醒：延时必须保证至少 1 个 tick。低 FreeRTOS HZ(如100Hz,
         * 1tick=10ms) 下 pdMS_TO_TICKS(5)=0，vTaskDelay(0) 退化为 yield，
         * 会让 ui_task 占满 Core1、饿死 IDLE1 触发任务看门狗。 !!! */
        TickType_t wait = pdMS_TO_TICKS(UI_LOOP_PERIOD_MS);
        vTaskDelay(wait > 0 ? wait : 1);
    }
}

esp_err_t ui_task_start(void)
{
    if (s_task_handle != NULL) {
        ESP_LOGW(TAG, "ui_task already started");
        return ESP_OK;
    }
    /* !!! 坑点提醒：最后一个参数必须为 1（钉 Core1），严禁 0/tskNO_AFFINITY !!! */
    BaseType_t rc = xTaskCreatePinnedToCore(ui_task_entry, "ui",
                                            8192, NULL, 5,
                                            &s_task_handle, 1);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore ui failed: %d", (int)rc);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ui_task created (Core1, prio5, stack8192)");
    return ESP_OK;
}
