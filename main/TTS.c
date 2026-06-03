/**
 * @file    TTS.c
 * @brief   云端 TTS 语音合成模块（百度 TTS API）
 * 
 * 内部实现从本地 esp-tts 切换为 HTTP 调用百度语音合成。
 * 返回的 WAV 音频（16kHz/16bit/mono）跳过 44 字节头后直接写入 I2S。
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "driver/i2s.h"
#include "WiFi.h"
#include "BaiduTTS.h"
#include "TTS.h"
#include "I2S.h"

#define TAG "TTS"

/* 播放后的冷却时间（毫秒），期间 feed_Task 持续清空 RX DMA 但不喂入 AFE */
#define TTS_COOLDOWN_MS  500

/* Token 提前刷新时间（秒）—— 百度 Token 有效期 30 天，提前 1 小时刷新 */
#define TOKEN_REFRESH_MARGIN_SEC  3600

/* 跨任务可见的播放状态标志 */
volatile bool g_tts_playing = false;

/* ---- 静态状态 ---- */
static char    s_access_token[256] = {0};  /* 百度 OAuth2 access_token */
static int64_t s_token_expiry_us = 0;      /* Token 过期时间戳（esp_timer） */
static char    s_cuid[24] = {0};           /* 设备唯一标识 */
static int     s_tts_speed = 5;            /* 百度 spd 参数 (0-15, 默认中速) */

/* ================================================================
 *  初始化
 * ================================================================ */
void TTS_Init(void)
{
    /* 1. 初始化 WiFi 并等待获取 IP */
    wifi_sta_init();

    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "WiFi 未连接，TTS 不可用");
        return;
    }

    /* 2. 基于 STA MAC 生成设备唯一标识 (CUID) */
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_cuid, sizeof(s_cuid),
                 "ESP32_%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(s_cuid, sizeof(s_cuid), "ESP32_TTS");
    }

    /* 3. 获取百度 OAuth2 access_token */
    if (baidu_tts_get_token(s_access_token, sizeof(s_access_token)) != ESP_OK) {
        ESP_LOGE(TAG, "百度 access_token 获取失败，TTS 不可用");
        s_access_token[0] = '\0';
        return;
    }

    /* Token 通常有效期 30 天 (2592000 秒)，提前 1 小时刷新 */
    s_token_expiry_us = esp_timer_get_time()
                      + (2592000LL - TOKEN_REFRESH_MARGIN_SEC) * 1000000LL;

    ESP_LOGI(TAG, "云端 TTS 初始化完成 (CUID: %s, 语速: %d)", s_cuid, s_tts_speed);
}

/* ================================================================
 *  合成并播放
 * ================================================================ */
void TTS_Speak(const char *chinese_text)
{
    /* ---- 参数校验 ---- */
    if (s_access_token[0] == '\0') {
        ESP_LOGE(TAG, "TTS 未初始化，无法播放");
        return;
    }

    if (NULL == chinese_text || strlen(chinese_text) == 0) {
        ESP_LOGW(TAG, "播放文本为空");
        return;
    }

    /* ---- WiFi 检查 ---- */
    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "WiFi 断开，无法云端合成");
        return;
    }

    /* ---- Token 到期前刷新 ---- */
    if (esp_timer_get_time() >= s_token_expiry_us) {
        ESP_LOGI(TAG, "Token 即将过期，刷新中...");
        if (baidu_tts_get_token(s_access_token, sizeof(s_access_token)) != ESP_OK) {
            ESP_LOGE(TAG, "Token 刷新失败");
            return;
        }
        s_token_expiry_us = esp_timer_get_time()
                          + (2592000LL - TOKEN_REFRESH_MARGIN_SEC) * 1000000LL;
    }

    /* ---- 调用百度云端合成 ---- */
    uint8_t *wav_data = NULL;
    size_t   wav_len  = 0;

    esp_err_t err = baidu_tts_synthesize(chinese_text, s_tts_speed,
                                          &wav_data, &wav_len, s_access_token);
    if (err != ESP_OK || NULL == wav_data || wav_len <= 44) {
        /* Token 失效（百度返回 err_no=502）时尝试刷新 token 并重试一次 */
        if (wav_data) {
            heap_caps_free(wav_data);
            wav_data = NULL;
        }

        ESP_LOGI(TAG, "尝试刷新 Token 并重试...");
        if (baidu_tts_get_token(s_access_token, sizeof(s_access_token)) == ESP_OK) {
            s_token_expiry_us = esp_timer_get_time()
                              + (2592000LL - TOKEN_REFRESH_MARGIN_SEC) * 1000000LL;

            err = baidu_tts_synthesize(chinese_text, s_tts_speed,
                                        &wav_data, &wav_len, s_access_token);
            if (err != ESP_OK || NULL == wav_data || wav_len <= 44) {
                ESP_LOGE(TAG, "Token 刷新后仍然失败");
                if (wav_data) heap_caps_free(wav_data);
                return;
            }
        } else {
            ESP_LOGE(TAG, "Token 刷新失败");
            return;
        }
    }

    /* ---- 验证 WAV 头 ---- */
    if (memcmp(wav_data, "RIFF", 4) != 0 ||
        memcmp(wav_data + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "无效的 WAV 头 (len=%u, %02X%02X%02X%02X)",
                 (unsigned)wav_len,
                 wav_data[0], wav_data[1], wav_data[2], wav_data[3]);
        heap_caps_free(wav_data);
        return;
    }

    /* ---- 播放 PCM（跳过 44 字节 WAV 头） ---- */
    uint8_t *pcm_data = wav_data + 44;
    size_t   pcm_len  = wav_len - 44;

    /* WAV 头解析，确认音频格式 */
    uint16_t audio_format   = (wav_data[20]) | (wav_data[21] << 8);
    uint16_t num_channels   = (wav_data[22]) | (wav_data[23] << 8);
    uint32_t sample_rate    = (wav_data[24]) | (wav_data[25] << 8) | (wav_data[26] << 16) | (wav_data[27] << 24);
    uint16_t bits_per_sample = (wav_data[34]) | (wav_data[35] << 8);

    ESP_LOGI(TAG, "播放: \"%s\" (%u 字节 PCM)", chinese_text, (unsigned)pcm_len);
    ESP_LOGI(TAG, "WAV 头: fmt=%u ch=%u rate=%lu bps=%u",
             audio_format, num_channels, (unsigned long)sample_rate, bits_per_sample);

    /* 打印 PCM 前 16 个样点用于调试 */
    {
        int16_t *samples = (int16_t *)pcm_data;
        printf("PCM前16样点:");
        for (int i = 0; i < 16 && i < (int)(pcm_len / 2); i++) {
            printf(" %d", samples[i]);
        }
        printf("\n");
    }

    g_tts_playing = true;

    size_t offset = 0;
    size_t written = 0;

    /* 先清空 TX DMA 中可能残留的旧数据 */
    i2s_zero_dma_buffer(I2S_NUM_0);

    while (offset < pcm_len) {
        /* 每次写入 I2S_BUFFER_SIZE*2 字节（= 512 样本 × 16bit），对齐 DMA 缓冲区 */
        size_t chunk = (pcm_len - offset > (size_t)(I2S_BUFFER_SIZE * 2))
                       ? (size_t)(I2S_BUFFER_SIZE * 2)
                       : (pcm_len - offset);

        err = i2s_write(I2S_NUM_0, pcm_data + offset, chunk, &written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S 写入失败: %d", err);
            break;
        }
        offset += written;
    }

    /* 计算播放时长并等待，确保 DMA 数据全部发送完毕 */
    uint32_t play_duration_ms = (uint32_t)(offset * 1000ULL / (sample_rate * num_channels * (bits_per_sample / 8)));
    ESP_LOGI(TAG, "等待播放完成 (%lu ms)...", (unsigned long)play_duration_ms);
    vTaskDelay(pdMS_TO_TICKS(play_duration_ms));

    /* 冷却期：防止喇叭声被麦克风采集后触发误识别 */
    ESP_LOGI(TAG, "冷却 %d ms，防止音频回环...", TTS_COOLDOWN_MS);
    vTaskDelay(pdMS_TO_TICKS(TTS_COOLDOWN_MS));

    g_tts_playing = false;
    ESP_LOGI(TAG, "播放完成 (%u / %u 字节)", (unsigned)offset, (unsigned)pcm_len);

    heap_caps_free(wav_data);
}

/* ================================================================
 *  语速设置（映射到百度 spd 参数 0-15）
 * ================================================================ */
void TTS_SetSpeed(int speed)
{
    if (speed < 0)  speed = 0;
    if (speed > 5)  speed = 5;

    /* 线性映射: 0-5 → 0-15 */
    s_tts_speed = speed * 3;
    ESP_LOGI(TAG, "语速: %d (百度 spd=%d)", speed, s_tts_speed);
}

bool TTS_IsPlaying(void)
{
    return g_tts_playing;
}

void TTS_Deinit(void)
{
    g_tts_playing = false;

    /* 清除 Token */
    memset(s_access_token, 0, sizeof(s_access_token));
    s_token_expiry_us = 0;

    /* 释放 WiFi 资源 */
    wifi_sta_deinit();
    ESP_LOGI(TAG, "云端 TTS 资源已释放");
}
