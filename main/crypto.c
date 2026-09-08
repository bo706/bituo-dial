/**
 * @file    crypto.c
 * @brief   mbedtls_ccm_auth_decrypt 的薄封装：每次调用独立初始化/释放上下文，
 *          无 AAD，适配 Bituo 电表 26B 加密广播的解密参数（任务书 5.2.2）
 * @version ESP-IDF v5.2.3 / mbedTLS
 * @date    2026-09-01
 */
#include "crypto.h"

#include <string.h>
#include "mbedtls/ccm.h"
#include "esp_log.h"

static const char *TAG = "crypto";

int aes_ccm_decrypt(const uint8_t *ciphertext, size_t cipher_len,
                    const uint8_t *tag, size_t tag_len,
                    const uint8_t key[BITUO_AES_KEY_LEN],
                    const uint8_t nonce[BITUO_CCM_NONCE_LEN],
                    uint8_t *plaintext_out)
{
    if (ciphertext == NULL || tag == NULL || key == NULL ||
        nonce == NULL || plaintext_out == NULL) {
        return -1;
    }

    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);

    /* AES-128：密钥位宽固定 128 bit */
    int ret = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES,
                                 key, BITUO_AES_KEY_LEN * 8);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_ccm_setkey failed: -0x%04X", (unsigned)-ret);
        goto exit;
    }

    /* iv=nonce(12B)；add=NULL/add_len=0 表示无 AAD；
     * 内部同时完成解密与 MIC 认证，MIC 不匹配返回 -0x000D。
     */
    ret = mbedtls_ccm_auth_decrypt(&ctx,
                                   cipher_len,
                                   nonce, BITUO_CCM_NONCE_LEN,
                                   NULL, 0,
                                   ciphertext, plaintext_out,
                                   tag, tag_len);
    if (ret != 0) {
        /* !!! 坑点提醒（任务书坑2）：密钥确认无误却返回 -0x000D 时，
         * 99% 是 Nonce 大小端错误（counter 小端、Nonce 需 BE32），
         * 而不是密钥问题，请回到 ble_scanner_build_nonce() 排查。
         */
        ESP_LOGD(TAG, "ccm_auth_decrypt ret=-0x%04X", (unsigned)-ret);
    }

exit:
    mbedtls_ccm_free(&ctx);
    return ret;
}

/* ======================================================================
 * 固定向量自测（与 tools/test_decrypt.py 同一组向量，PC 端 cryptography
 * 库 AESCCM(tag=4) 预先生成；任何一端改动 CCM 参数都会导致对拍失败）
 *
 *   key     = 000102030405060708090a0b0c0d0e0f
 *   counter = 0x11223344
 *   nonce12 = 000000000000000011223344        （8B 零 + BE32 counter）
 *   plain15 = 000102030405060708090a0b0c0d0e
 *   ct15    = dba850e60b9abcb3eb60293fdd56e7
 *   tag4    = 58c643e3
 * ====================================================================== */
esp_err_t crypto_selftest(void)
{
    static const uint8_t tv_key[BITUO_AES_KEY_LEN] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    static const uint8_t tv_nonce[BITUO_CCM_NONCE_LEN] = {
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x11,0x22,0x33,0x44
    };
    static const uint8_t tv_ct[BITUO_PLAINTEXT_LEN] = {
        0xdb,0xa8,0x50,0xe6,0x0b,0x9a,0xbc,0xb3,
        0xeb,0x60,0x29,0x3f,0xdd,0x56,0xe7
    };
    static const uint8_t tv_tag[BITUO_CCM_TAG_LEN] = { 0x58,0xc6,0x43,0xe3 };
    static const uint8_t tv_plain[BITUO_PLAINTEXT_LEN] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e
    };

    /* ── 正样本：必须解密成功且明文逐字节一致 ── */
    uint8_t plain[BITUO_PLAINTEXT_LEN];
    int ret = aes_ccm_decrypt(tv_ct, sizeof(tv_ct), tv_tag, sizeof(tv_tag),
                              tv_key, tv_nonce, plain);
    if (ret != 0) {
        ESP_LOGE(TAG, "selftest POSITIVE failed: decrypt ret=-0x%04X", (unsigned)-ret);
        return ESP_FAIL;
    }
    if (memcmp(plain, tv_plain, sizeof(tv_plain)) != 0) {
        ESP_LOGE(TAG, "selftest POSITIVE failed: plaintext mismatch");
        return ESP_FAIL;
    }

    /* ── 负样本：篡改 1 字节 tag，MIC 必须拒绝（返回非 0）── */
    uint8_t bad_tag[BITUO_CCM_TAG_LEN];
    memcpy(bad_tag, tv_tag, sizeof(bad_tag));
    bad_tag[3] ^= 0xFF;
    uint8_t dummy[BITUO_PLAINTEXT_LEN];
    int bad_ret = aes_ccm_decrypt(tv_ct, sizeof(tv_ct), bad_tag, sizeof(bad_tag),
                                  tv_key, tv_nonce, dummy);
    if (bad_ret == 0) {
        ESP_LOGE(TAG, "selftest NEGATIVE failed: tampered tag was accepted");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "selftest OK: CCM positive vector matched, tampered tag rejected");
    return ESP_OK;
}
