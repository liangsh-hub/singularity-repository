/**
 * @file    BaiduTTS.c
 * @brief   百度语音合成 API 封装
 *
 * - baidu_tts_get_token():  OAuth2 → access_token（HTTPS）
 * - baidu_tts_synthesize(): POST 文本 → WAV 二进制（HTTPS）
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "BaiduTTS.h"

#define TAG "BaiduTTS"

/* OAuth 端点 */
#define BAIDU_AUTH_URL  "https://aip.baidubce.com/oauth/2.0/token"

/* TTS 端点 */
#define BAIDU_TTS_URL   "https://tsn.baidu.com/text2audio"

/* HTTP 响应缓冲区最大字节数（约 30 秒 16kHz/16bit 音频） */
#define MAX_WAV_SIZE    (512 * 1024)

/* HTTP 请求超时（毫秒） */
#define HTTP_TIMEOUT_MS 60000

/* ================================================================
 *  URL 编码（UTF-8 安全）
 *
 *  unreserved 字符 (A-Z a-z 0-9 - _ . ~) 原样输出，
 *  其余字节（包括中文 UTF-8 多字节）编码为 %XX。
 *  调用者负责释放返回的字符串。
 * ================================================================ */
static char *url_encode(const char *src)
{
    if (NULL == src) return NULL;

    /* 最坏情况：每个字节都变成 %XX（3 倍长度 + 1 终止符） */
    size_t src_len = strlen(src);
    size_t max_len = src_len * 3 + 1;
    char *dst = (char *)calloc(1, max_len);
    if (NULL == dst) return NULL;

    size_t di = 0;
    for (size_t si = 0; si < src_len; si++) {
        unsigned char c = (unsigned char)src[si];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else {
            /* 确保不越界 */
            if (di + 3 >= max_len) break;
            snprintf(dst + di, 4, "%%%02X", c);
            di += 3;
        }
    }
    dst[di] = '\0';
    return dst;
}

/* ================================================================
 *  Token 获取
 * ================================================================ */
esp_err_t baidu_tts_get_token(char *token_buf, size_t buf_size)
{
    if (NULL == token_buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    token_buf[0] = '\0';

    /* 构建 POST body */
    char post_body[512];
    int body_len = snprintf(post_body, sizeof(post_body),
                            "grant_type=client_credentials"
                            "&client_id=%s"
                            "&client_secret=%s",
                            BAIDU_API_KEY, BAIDU_SECRET_KEY);

    ESP_LOGI(TAG, "可用堆: %u 字节", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "正在获取 access_token ...");

    /* HTTPS POST —— 手动 open→write→fetch_headers→read→close 流程 */
    esp_http_client_config_t config = {
        .url                           = BAIDU_AUTH_URL,
        .method                        = HTTP_METHOD_POST,
        .timeout_ms                    = HTTP_TIMEOUT_MS,
        .keep_alive_enable             = false,
        .skip_cert_common_name_check   = true,
        .is_async                      = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (NULL == client) {
        ESP_LOGE(TAG, "HTTP 客户端初始化失败");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type",
                               "application/x-www-form-urlencoded");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Host", "aip.baidubce.com");

    esp_err_t err = esp_http_client_open(client, body_len);
    if (ESP_OK != err) {
        ESP_LOGE(TAG, "HTTP open 失败: %d", err);
        esp_http_client_cleanup(client);
        return err;
    }

    int written = esp_http_client_write(client, post_body, body_len);
    if (written < 0) {
        ESP_LOGE(TAG, "HTTP write 失败: %d", written);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Token POST body已发送 %d/%d 字节", written, body_len);

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Token HTTP状态:%d, Content-Length:%d", status, content_len);

    /* fetch_headers 返回负值（且不是 -1，-1 只是表示无 Content-Length / chunked）才表示错误 */
    if (content_len < -1) {
        ESP_LOGE(TAG, "fetch_headers 失败: %d", content_len);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Token 请求返回 HTTP %d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* 读取 JSON 响应 —— 循环读取以支持 chunked 传输 */
    size_t resp_buf_size = (content_len > 0 && content_len < 4096)
                          ? (size_t)(content_len + 1) : 4096;
    char *resp = (char *)calloc(1, resp_buf_size);
    if (NULL == resp) {
        ESP_LOGE(TAG, "Token 响应内存分配失败");
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total_read = 0;
    int read_len = 0;
    int read_attempts = 0;
    while (total_read < (int)(resp_buf_size - 1)) {
        read_len = esp_http_client_read(client, resp + total_read,
                                         resp_buf_size - total_read - 1);
        if (read_len > 0) {
            total_read += read_len;
            read_attempts++;
            ESP_LOGI(TAG, "Token read #%d: %d 字节 (累计 %d)", read_attempts, read_len, total_read);
        } else if (read_len == 0) {
            break;
        } else {
            ESP_LOGW(TAG, "Token read 错误: %d (已读 %d)", read_len, total_read);
            break;
        }
    }

    esp_http_client_cleanup(client);
    resp[total_read] = '\0';

    ESP_LOGI(TAG, "Token 响应: %d 字节", total_read);

    if (total_read == 0) {
        ESP_LOGE(TAG, "Token 响应为空，HTTP %d, Content-Length: %d", status, content_len);
        free(resp);
        return ESP_FAIL;
    }

    /* 解析 JSON */
    cJSON *json = cJSON_Parse(resp);
    if (NULL == json) {
        ESP_LOGE(TAG, "Token JSON 解析失败: %s", resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *token_item = cJSON_GetObjectItem(json, "access_token");
    cJSON *expires_item = cJSON_GetObjectItem(json, "expires_in");

    if (NULL == token_item || !cJSON_IsString(token_item)) {
        /* 可能有错误信息 */
        cJSON *err_item = cJSON_GetObjectItem(json, "error_description");
        ESP_LOGE(TAG, "Token 响应无 access_token: %s",
                 err_item ? err_item->valuestring : resp);
        cJSON_Delete(json);
        free(resp);
        return ESP_FAIL;
    }

    strncpy(token_buf, token_item->valuestring, buf_size - 1);
    token_buf[buf_size - 1] = '\0';

    int expires_in = expires_item ? expires_item->valueint : 2592000;
    ESP_LOGI(TAG, "access_token 获取成功 (有效期 %d 秒)", expires_in);

    cJSON_Delete(json);
    free(resp);
    return ESP_OK;
}

/* ================================================================
 *  TTS 合成
 * ================================================================ */
esp_err_t baidu_tts_synthesize(const char *text, int speed,
                                uint8_t **wav_data, size_t *wav_len,
                                const char *token)
{
    if (NULL == text || NULL == wav_data || NULL == wav_len || NULL == token) {
        return ESP_ERR_INVALID_ARG;
    }
    *wav_data = NULL;
    *wav_len  = 0;

    /* 限制 speed 范围 */
    if (speed < 0)  speed = 0;
    if (speed > 15) speed = 15;

    /* URL-encode 中文文本 */
    char *encoded_text = url_encode(text);
    if (NULL == encoded_text) {
        ESP_LOGE(TAG, "URL 编码失败");
        return ESP_ERR_NO_MEM;
    }

    /* 构建 POST body */
    char *post_body = (char *)calloc(1, strlen(encoded_text) + 1024);
    if (NULL == post_body) {
        free(encoded_text);
        return ESP_ERR_NO_MEM;
    }

    int body_len = snprintf(post_body,
                            strlen(encoded_text) + 1024,
                            "tex=%s"
                            "&tok=%s"
                            "&cuid=ESP32_TTS"
                            "&ctp=1"
                            "&lan=zh"
                            "&aue=6"
                            "&spd=%d"
                            "&pit=5"
                            "&vol=5"
                            "&per=0",
                            encoded_text, token, speed);
    free(encoded_text);

    ESP_LOGI(TAG, "可用堆: %u 字节", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "云端合成: \"%s\" (语速=%d)", text, speed);

    /* HTTPS POST —— 手动 open→write→fetch_headers→read→close 流程 */
    esp_http_client_config_t config = {
        .url                           = BAIDU_TTS_URL,
        .method                        = HTTP_METHOD_POST,
        .timeout_ms                    = HTTP_TIMEOUT_MS,
        .keep_alive_enable             = false,
        .skip_cert_common_name_check   = true,
        .is_async                      = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (NULL == client) {
        ESP_LOGE(TAG, "HTTP 客户端初始化失败");
        free(post_body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type",
                               "application/x-www-form-urlencoded");
    esp_http_client_set_header(client, "Accept", "audio/wav");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Host", "tsn.baidu.com");

    esp_err_t err = esp_http_client_open(client, body_len);
    if (ESP_OK != err) {
        ESP_LOGE(TAG, "TTS HTTP open 失败: %d", err);
        esp_http_client_cleanup(client);
        free(post_body);
        return err;
    }

    int written = esp_http_client_write(client, post_body, body_len);
    free(post_body);  /* post_body 不再需要 */
    if (written < 0) {
        ESP_LOGE(TAG, "TTS HTTP write 失败: %d", written);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TTS POST body已发送 %d/%d 字节", written, body_len);

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "TTS HTTP状态:%d, Content-Length:%d", status, content_len);

    if (content_len < -1) {
        ESP_LOGE(TAG, "TTS fetch_headers 失败: %d", content_len);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* 检查 Content-Type */
    char *content_type = NULL;
    esp_http_client_get_header(client, "Content-Type", &content_type);
    ESP_LOGI(TAG, "TTS Content-Type: %s", content_type ? content_type : "(null)");

    bool is_wav_by_ct = (content_type && strstr(content_type, "audio/wav"));
    bool is_json_by_ct = (content_type && strstr(content_type, "application/json"));
    bool is_http_err    = (status != 200);

    /* 先偷看响应体前 4 字节，检测 RIFF 魔术字
     * （百度 TTS 有时返回错误的 Content-Type，但 body 确实是 WAV） */
    char peek[4] = {0};
    int peek_len = esp_http_client_read(client, peek, 4);
    bool is_wav_by_magic = (peek_len == 4 && memcmp(peek, "RIFF", 4) == 0);

    if (is_wav_by_ct || is_wav_by_magic) {
        /* ---- 成功：读取 WAV 二进制 ---- */
        size_t alloc_size = (content_len > 0 && content_len <= MAX_WAV_SIZE)
                            ? (size_t)content_len : MAX_WAV_SIZE;

        *wav_data = (uint8_t *)heap_caps_malloc(alloc_size,
                                                 MALLOC_CAP_SPIRAM);
        if (NULL == *wav_data) {
            ESP_LOGE(TAG, "WAV 缓冲区分配失败 (%u 字节)", (unsigned)alloc_size);
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }

        /* 先复制已偷看的 4 字节 */
        if (peek_len > 0) {
            memcpy(*wav_data, peek, peek_len);
            *wav_len = peek_len;
        }

        /* 继续读取剩余数据 */
        int read_len = 0;
        while ((size_t)(*wav_len) < alloc_size) {
            read_len = esp_http_client_read(client,
                                             (char *)(*wav_data + *wav_len),
                                             alloc_size - *wav_len);
            if (read_len > 0) {
                *wav_len += read_len;
            } else if (read_len == 0) {
                break;  /* EOF */
            } else {
                ESP_LOGW(TAG, "WAV read 错误: %d (已读 %u)", read_len, (unsigned)*wav_len);
                break;
            }
        }

        ESP_LOGI(TAG, "WAV 接收完成: %u 字节 (来源: %s)",
                 (unsigned)*wav_len,
                 is_wav_by_magic ? "RIFF魔术字" : "Content-Type");

    } else if (is_json_by_ct || is_http_err) {
        /* ---- 失败：读取 JSON 错误信息 ---- */
        char *err_buf = (char *)calloc(1, 1024);
        if (err_buf) {
            /* 把 peek 数据先复制进去 */
            if (peek_len > 0) memcpy(err_buf, peek, peek_len);
            int total = peek_len;

            int read_len = esp_http_client_read(client, err_buf + total, 1023 - total);
            if (read_len > 0) {
                total += read_len;
                err_buf[total] = '\0';

                cJSON *json = cJSON_Parse(err_buf);
                if (json) {
                    cJSON *err_no  = cJSON_GetObjectItem(json, "err_no");
                    cJSON *err_msg = cJSON_GetObjectItem(json, "err_msg");
                    ESP_LOGE(TAG, "百度 TTS 错误: err_no=%d, err_msg=%s",
                             err_no ? err_no->valueint : -1,
                             err_msg ? err_msg->valuestring : "未知");
                    cJSON_Delete(json);
                } else {
                    ESP_LOGE(TAG, "百度 TTS 错误响应: %s", err_buf);
                }
            }
            free(err_buf);
        } else {
            ESP_LOGE(TAG, "百度 TTS 返回 HTTP %d", status);
        }
        err = ESP_FAIL;

    } else {
        /* 未知响应，尝试读取响应体调试 */
        char dbg[128] = {0};
        if (peek_len > 0) memcpy(dbg, peek, peek_len);
        int total = peek_len;
        int read_len = esp_http_client_read(client, dbg + total, sizeof(dbg) - total - 1);
        if (read_len > 0) total += read_len;
        dbg[total] = '\0';
        ESP_LOGE(TAG, "TTS 未知响应 (HTTP %d, CT=%s, body=%d字节): %s",
                 status, content_type ? content_type : "null", total, dbg);
        err = ESP_FAIL;
    }

    /* 注意: content_type 由 esp_http_client 内部管理，cleanup 时自动释放，
     * 不要手动 free(content_type)，否则会导致 double-free */
    esp_http_client_cleanup(client);
    return err;
}
