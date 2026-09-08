# -*- coding: utf-8 -*-
"""一次性补丁：ble_scanner.c 由按 MAC 匹配改为按 broadcast_key 盲解匹配。"""
import io, sys

PATH = r"C:\Users\kangx\Doubao\chats\2026-09-01\new-chat-2\bituo-dial\main\ble_scanner.c"

OLD = '''    /* 过滤：MAC 必须是已配置电表；一台未配置时进入观测模式打印原始帧，
     * 便于 Phase1 在没有 key 的阶段先确认空口能收到目标电表。 */
    int idx = meter_store_find_by_mac(addr);
    if (idx < 0) {
        if (g_meter_count == 0) {
            if (observe_allow_log(addr)) {
                ESP_LOGI(TAG, "(observe) LONG%u %s rssi=%d ctrl=0x%02x counter=%lu",
                         (unsigned)mfg_len, addr_s, event->disc.rssi,
                         mfg[off_ctrl], (unsigned long)counter);
                log_hex_line("(observe) long hex: ", mfg, mfg_len);
            }
        }
        return 0;
    }

    /* 构造 BE32 Nonce（禁止 memcpy，见函数内警告） */
    uint8_t nonce[BITUO_CCM_NONCE_LEN];
    ble_scanner_build_nonce(counter, nonce);

    /* AES-128-CCM 认证解密：密文 15B，Tag 4B */
    uint8_t plaintext[BITUO_PLAINTEXT_LEN];
    int dec_ret = aes_ccm_decrypt(mfg + off_ct, BITUO_PLAINTEXT_LEN,
                                  mfg + off_tag, BITUO_CCM_TAG_LEN,
                                  g_meter_configs[idx].broadcast_key,
                                  nonce, plaintext);
    if (dec_ret != 0) {
        /* MIC 失败帧直接丢弃；排查顺序：Nonce 大小端 -> key -> 长度 */
        ESP_LOGD(TAG, "idx=%d decrypt fail -0x%04X", idx, (unsigned)-dec_ret);
        return 0;
    }
'''

NEW = '''    /* 构造 BE32 Nonce（禁止 memcpy，见函数内警告）。Nonce 只依赖广播
     * counter，与源 BLE 地址无关，因此对所有候选表只构造一次。 */
    uint8_t nonce[BITUO_CCM_NONCE_LEN];
    ble_scanner_build_nonce(counter, nonce);

    /* !!! 坑点提醒（RPA 随机私有地址，关键）!!!
     * 该型电表 BLE 地址是随机私有地址，每次重新配网或重启后都会变
     * （实测同一表先后为 7E:1E:.. 与 47:0E:..），因此【绝不能】按 MAC 匹配
     * ——配置时记下的地址下一帧就失效，find_by_mac 永远落空。
     * 正确做法（安全通道文档 9.3：持有 broadcast_key 即可无连接解密，与地址
     * 无关）：对每一块已配置电表，用其 broadcast_key 逐一尝试 AES-128-CCM
     * 解密（密文 15B、Tag 4B），谁的 Tag 校验通过，这一帧就属于谁。
     * 最多 16 次解密，ESP32-S3 上总耗时几十微秒，相对 200ms 周期可忽略。
     * 注：扫描回调与 add/del 配置回调同属 NimBLE host 单任务，遍历期间配置
     * 不会被并发修改，无需额外加锁。 */
    uint8_t plaintext[BITUO_PLAINTEXT_LEN];
    int idx = -1;
    for (int i = 0; i < g_meter_count; i++) {
        if (!g_meter_configs[i].enabled) {
            continue;
        }
        int try_rc = aes_ccm_decrypt(mfg + off_ct, BITUO_PLAINTEXT_LEN,
                                     mfg + off_tag, BITUO_CCM_TAG_LEN,
                                     g_meter_configs[i].broadcast_key,
                                     nonce, plaintext);
        if (try_rc == 0) {
            idx = i;   /* Tag 校验通过，命中该表 */
            break;
        }
    }

    if (idx < 0) {
        /* 所有已配置 key 均无法通过 Tag 校验：非本网关配置的表，或 key 不符。
         * 一台表都未配置时进入观测模式打印原始长包，便于空口排查。 */
        if (g_meter_count == 0) {
            if (observe_allow_log(addr)) {
                ESP_LOGI(TAG, "(observe) LONG%u %s rssi=%d ctrl=0x%02x counter=%lu",
                         (unsigned)mfg_len, addr_s, event->disc.rssi,
                         mfg[off_ctrl], (unsigned long)counter);
                log_hex_line("(observe) long hex: ", mfg, mfg_len);
            }
        }
        return 0;
    }
'''

with io.open(PATH, "r", encoding="utf-8") as f:
    src = f.read()

n = src.count(OLD)
if n != 1:
    print("MATCH_COUNT=%d 中止（要求恰好 1 处）" % n)
    sys.exit(1)

src = src.replace(OLD, NEW, 1)
with io.open(PATH, "w", encoding="utf-8", newline="\n") as f:
    f.write(src)
print("PATCHED OK")
