/**
 * @file    BaiduTTS.h
 * @brief   百度语音合成 API 封装
 *
 * 提供 access_token 获取和文本→WAV 音频合成功能。
 * API Key / Secret Key 通过下方宏定义配置。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 *  ！！！用户必须填入自己的百度 API 凭证！！！
 *  申请地址: https://console.bce.baidu.com/ai/#/ai/speech/overview
 * ================================================================ */
#define BAIDU_API_KEY       "1j9dAnhNGV984AmMXj2Jf02C"
#define BAIDU_SECRET_KEY    "3RnJN44V11AuWq2PQRe3H8TuKkryrccR"

/**
 * @brief  获取百度 OAuth2 access_token
 * @param  token_buf  输出缓冲区（存放 token 字符串）
 * @param  buf_size   缓冲区大小（建议 >= 256）
 * @return ESP_OK 成功，其他值表示失败
 */
esp_err_t baidu_tts_get_token(char *token_buf, size_t buf_size);

/**
 * @brief  调用百度语音合成，返回 WAV 格式音频数据
 *
 * @param  text      要合成的中文 UTF-8 文本（会被 URL-encode 后 POST）
 * @param  speed     语速 (0-15)，0=最慢，15=最快
 * @param  wav_data  输出：WAV 数据指针（调用者需 free() 释放）
 * @param  wav_len   输出：WAV 数据长度（字节）
 * @param  token     有效的 access_token
 * @return ESP_OK 成功，*wav_data 指向 SPIRAM 中分配的 WAV 数据
 *         其他值表示失败，*wav_data 为 NULL
 */
esp_err_t baidu_tts_synthesize(const char *text, int speed,
                                uint8_t **wav_data, size_t *wav_len,
                                const char *token);

#ifdef __cplusplus
}
#endif
