/**
 * @file    ble_gatt_server.c
 * @brief   NimBLE GATT Server：A003 明文配置服务，C313 写 JSON 命令、
 *          C314 读最后响应、C315 Notify 回发统一响应（任务书 5.7 / 第7章）
 * @note    本阶段全程明文 JSON，不做 EC J-PAKE 握手；MTU 协商到 512（坑8）。
 *          GATT 服务必须在 nimble_port_init 之后、nimble_port_freertos_init
 *          （host 启动）之前注册；广播必须在 host sync 回调里启动。
 * @version ESP-IDF v5.2.3 / NimBLE
 * @date    2026-09-01
 */
#include "ble_gatt_server.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* NimBLE 头文件 */
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "host/ble_att.h"
#include "host/ble_hs_adv.h"
#include "os/os_mbuf.h"

#include "cmd_dispatch.h"

static const char *TAG = "ble_gatt";

/* C315 Notify 特征句柄（服务注册后由 NimBLE 填充） */
/* !!! 坑点提醒（响应长度）!!!：16 块表的 list_meters/get_meters 响应可达
 * ~2KB，响应缓冲必须足够大，否则 snprintf 截断成非法 JSON。缓冲用静态全局，
 * 不能放 NimBLE host 任务栈上。 */
static uint16_t s_notify_handle = 0;
/* 最后一次响应缓存（供 C314 读取） */
static char     s_last_resp[BITUO_RESP_CAP];
static uint16_t s_last_resp_len = 0;
/* GATT 写回调序列化缓冲（静态：GATT 回调单线程执行，无需加锁） */
static char     s_resp_buf[BITUO_RESP_CAP];

/* ======================================================================
 * GATT 服务/特征定义（任务书 5.7.1：SVC=A003, WRITE=C313, READ=C314, NOTIFY=C315）
 * ====================================================================== */
static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_chr_def s_chars[] = {
    {
        .uuid      = BLE_UUID16_DECLARE(0xC313),   /* 客户端 -> Dial：写 JSON 命令 */
        .access_cb = gatt_access_cb,
        .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid      = BLE_UUID16_DECLARE(0xC314),   /* 读最后一次响应 */
        .access_cb = gatt_access_cb,
        .flags     = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid      = BLE_UUID16_DECLARE(0xC315),   /* Dial -> 客户端：Notify 响应 */
        .access_cb = gatt_access_cb,
        .flags     = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_notify_handle,   /* 注册后 NimBLE 填充句柄，供 notify 使用 */
    },
    { 0 }   /* 特征表终止 */
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type              = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid              = BLE_UUID16_DECLARE(0xA003),
        .characteristics   = s_chars,
    },
    { 0 }   /* 服务表终止 */
};

/* 命令分发已抽到 cmd_dispatch.c（GATT / HTTP / MQTT 共用） */

/* ======================================================================
 * GATT 访问回调：C313 写命令 -> 解析 -> 响应经 C315 Notify + 缓存供 C314 读
 * ====================================================================== */
static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len <= 0 || len >= (int)sizeof(s_last_resp)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        char buf[512];
        int rc = os_mbuf_copydata(ctxt->om, 0, len, buf);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
        buf[len] = '\0';
        ESP_LOGI(TAG, "GATT write (conn=%u len=%d): %s", conn_handle, len, buf);

        int rlen = cmd_dispatch_exec(buf, s_resp_buf, sizeof(s_resp_buf));
        if (rlen <= 0) return 0;

        /* 缓存最后响应，供 C314 读取 */
        memcpy(s_last_resp, s_resp_buf, rlen);
        s_last_resp_len = (uint16_t)rlen;

        /* 通过 C315 Notify 回发（客户端需先订阅 CCC，bleak start_notify 会自动订阅）。
         * !!! 坑点提醒（任务书坑8延伸）!!!：单条 notification 有效载荷上限 =
         * 协商后 ATT_MTU - 3；即使 MTU=512 也只有 509 字节，而 16 块表的
         * list_meters 响应可达 ~2KB。因此按 MTU-3 切片连续 notify，客户端累积
         * 到能 json.loads 出完整 JSON 再处理（见 dial_config.py / phase2_selftest.py）。
         * 注意：本回调运行在 NimBLE host 任务，片间【不能】vTaskDelay 阻塞——分包
         * 续发依赖 host 任务处理 controller 事件，自我阻塞会导致整包卡死；连续提交
         * 即可，发不出去的分片由 NimBLE L2CAP tx_q 自动排队、在后续连接事件续发。 */
        if (s_notify_handle != 0) {
            uint16_t att_mtu = ble_att_mtu(conn_handle);          /* 当前连接实际 MTU */
            uint16_t chunk = (att_mtu >= 3) ? (uint16_t)(att_mtu - 3) : 20;
            uint16_t off = 0;
            int chunks = 0;
            while (off < (uint16_t)rlen) {
                uint16_t n = ((uint16_t)rlen - off > chunk) ? chunk : (uint16_t)((uint16_t)rlen - off);
                struct os_mbuf *om = ble_hs_mbuf_from_flat(s_resp_buf + off, n);
                if (om == NULL) {
                    ESP_LOGE(TAG, "mbuf alloc failed at off=%u", off);
                    break;
                }
                int nrc = ble_gatts_notify_custom(conn_handle, s_notify_handle, om);
                if (nrc != 0) {
                    ESP_LOGW(TAG, "notify chunk off=%u/%u rc=%d", off, rlen, nrc);
                    break;
                }
                chunks++;
                off += n;
            }
            ESP_LOGI(TAG, "notify resp len=%d mtu=%u chunk=%u -> %d chunks",
                     rlen, att_mtu, chunk, chunks);
        }
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (s_last_resp_len > 0) {
            int rc = os_mbuf_append(ctxt->om, s_last_resp, s_last_resp_len);
            return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ======================================================================
 * 对外接口
 * ====================================================================== */
esp_err_t ble_gatt_server_start(void)
{
    ESP_LOGI(TAG, "ble_gatt_server_start: register A003 service, preferred MTU=%u",
             BITUO_GATT_PREFERRED_MTU);

    /* !!! 坑点提醒（任务书坑8）!!!
     * 默认 BLE MTU=23 字节，长 JSON 响应会被拆成多包，客户端 json.loads 失败。
     * 必须在 host 启动前设置 preferred MTU=512，连接时双方协商到最大可用值。
     * 若协商后仍有分包，需在 _on_notify 按 '}' 拼包（本项目响应均 <500 字节）。 */
    ble_att_set_preferred_mtu(BITUO_GATT_PREFERRED_MTU);

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
        return ESP_FAIL;
    }

    /* 服务注册后，s_notify_handle 已被 NimBLE 通过 val_handle 指针填充 */
    ESP_LOGI(TAG, "GATT svc A003 registered: write=C313 read=C314 notify=C315(handle=%u)",
             s_notify_handle);
    return ESP_OK;
}

/**
 * @brief 广播 GAP 事件回调：连接时 NimBLE 自动停广播，
 *        断开后必须在此重新启动可连接广播，否则设备只可被发现/连接一次。
 */
static uint8_t s_adv_addr_type = 0;

static int adv_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "[ADV] central connected, handle=%d", event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "[ADV] connect failed status=%d, restart adv", event->connect.status);
            ble_gatt_server_adv_start();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "[ADV] disconnected reason=%d, restart advertising",
                 event->disconnect.reason);
        /* !!! 关键：断开后重新广播，否则 PC 下次扫描找不到设备 !!! */
        ble_gatt_server_adv_start();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGW(TAG, "[ADV] ADV_COMPLETE reason=%d, restart", event->adv_complete.reason);
        ble_gatt_server_adv_start();
        break;
    default:
        break;
    }
    return 0;
}

/* 广播包：flags(0x06=LE General Discoverable + BR/EDR not supported)
 * + Complete Local Name("BitUo-Dial")，共 15 字节（≤31 合法）。 */
static const uint8_t s_adv_data[] = {
    0x02, 0x01, 0x06,
    0x0B, 0x09, 'B','i','t','U','o','-','D','i','a','l'
};
static bool s_adv_data_set = false;

/**
 * @brief 启动（或恢复）可连接非定向广播。可重复调用：已在广播则直接返回。
 * @note  广播与被动扫描共存于 NimBLE；PC 端 dial_config.py 按名 "BitUo-Dial" 发现。
 */
esp_err_t ble_gatt_server_adv_start(void)
{
    if (ble_gap_adv_active()) {
        return ESP_OK;   /* 已在广播，幂等返回 */
    }

    if (!s_adv_data_set) {
        int sr = ble_gap_adv_set_data(s_adv_data, sizeof(s_adv_data));
        if (sr != 0) {
            ESP_LOGE(TAG, "ble_gap_adv_set_data rc=%d", sr);
            return ESP_FAIL;
        }
        s_adv_data_set = true;
    }

    ble_hs_id_infer_auto(0, &s_adv_addr_type);

    struct ble_gap_adv_params advp;
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode  = BLE_GAP_CONN_MODE_UND;   /* 可连接非定向（NimBLE 宏名 _UND） */
    advp.disc_mode  = BLE_GAP_DISC_MODE_GEN;   /* 一般可发现 */
    /* 自身广播过密会挤掉扫表；200~300ms 仍可被 dial_config.py 发现。 */
    advp.itvl_min   = 320;   /* 200ms（单位 0.625ms） */
    advp.itvl_max   = 480;   /* 300ms */
    advp.channel_map = 7;    /* 37/38/39 三广播信道全发 */

    int rc = ble_gap_adv_start(s_adv_addr_type, NULL, BLE_HS_FOREVER,
                               &advp, adv_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "advertising started: 'BitUo-Dial' connectable+discoverable");
    return ESP_OK;
}
