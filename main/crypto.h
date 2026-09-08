/**
 * @file    crypto.h
 * @brief   AES-128-CCM 解密封装（mbedTLS），用于电表 BLE 加密广播解密
 *          （任务书 5.2.2：15B 明文、4B Tag、AAD 空、12B Nonce）
 * @version ESP-IDF v5.2.3 / mbedTLS
 * @date    2026-09-01
 */
#ifndef MAIN_CRYPTO_H_
#define MAIN_CRYPTO_H_

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BITUO_AES_KEY_LEN      16    /* broadcast_key：128 bit */
#define BITUO_CCM_NONCE_LEN    12    /* 8B 全 0 + BE32(counter) */
#define BITUO_CCM_TAG_LEN      4     /* 广播包尾部 4B MIC */
#define BITUO_PLAINTEXT_LEN    15    /* 解密后明文长度 */

/**
 * @brief AES-128-CCM 认证解密
 *
 * @param ciphertext    密文（广播包偏移 7..21，共 15B）
 * @param cipher_len    密文长度（15）
 * @param tag           MIC（广播包偏移 22..25，共 4B）
 * @param tag_len       Tag 长度（4）
 * @param key           broadcast_key，16B
 * @param nonce         12B Nonce（ble_scanner_build_nonce 构造）
 * @param plaintext_out 输出明文缓冲，至少 15B
 * @return int          0=成功；-0x000D=MBEDTLS_ERR_CCM_AUTH_FAILED
 *                      （密钥错误或 Nonce 大小端错误，见任务书坑2）
 */
int aes_ccm_decrypt(const uint8_t *ciphertext, size_t cipher_len,
                    const uint8_t *tag, size_t tag_len,
                    const uint8_t key[BITUO_AES_KEY_LEN],
                    const uint8_t nonce[BITUO_CCM_NONCE_LEN],
                    uint8_t *plaintext_out);

/**
 * @brief CCM 解密链路固定向量自测（开机时调用一次，不依赖空口/电表）
 * @note  向量与 tools/test_decrypt.py 完全一致，由 PC 端 cryptography 库
 *        预先生成，用于证明固件 mbedTLS 参数（tag=4/AAD 空/nonce12）正确。
 * @return ESP_OK 正样本解密成功且明文匹配、负样本(篡改 tag)被正确拒绝；
 *         其余为失败。
 */
esp_err_t crypto_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_CRYPTO_H_ */
