/**
 * @file    WiFi.c
 * @brief   WiFi STA 模式管理
 *
 * 初始化 WiFi、阻塞等待获取 IP，并同步 SNTP 时间。
 * SSID/密码通过下方宏定义配置，后续可迁移至 NVS。
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "WiFi.h"

/* ================================================================
 *  WiFi 配置 —— 填入路由器 SSID 和密码
 * ================================================================ */
#define WIFI_SSID           "Xiaomi2603"
#define WIFI_PASSWORD       "123456789@.@"
#define WIFI_MAX_RETRIES    30
#define WIFI_RETRY_DELAY_MS 10000

#define TAG "WiFi"

/* 事件组 bit 位 */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static volatile bool       s_wifi_connected   = false;
static int                 s_retry_count       = 0;

/* ---- SNTP 时间同步 -------------------------------------------------- */

static void sntp_sync_wait(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("cn.pool.ntp.org");
    esp_netif_sntp_init(&config);
    esp_netif_sntp_start();

    ESP_LOGI(TAG, "等待 SNTP 时间同步...");
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000));
    if (err == ESP_OK) {
        time_t now = 0;
        struct tm timeinfo = {0};
        time(&now);
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG, "SNTP 同步成功: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP 同步超时（%d），HTTPS 证书校验可能失败", err);
    }
}

/* ---- WiFi 事件处理 -------------------------------------------------- */

static void wifi_event_handler(void *arg,
                                esp_event_base_t event_base,
                                int32_t event_id,
                                void *event_data)
{
    if (event_base == WIFI_EVENT) {

        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();

        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *disconn =
                (wifi_event_sta_disconnected_t *)event_data;

            s_wifi_connected = false;
            ESP_LOGW(TAG, "WiFi 断开 (原因: %d)，重试 %d/%d",
                     disconn->reason, s_retry_count + 1, WIFI_MAX_RETRIES);

            if (s_retry_count < WIFI_MAX_RETRIES) {
                esp_wifi_connect();
                s_retry_count++;
                vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
            } else {
                ESP_LOGE(TAG, "WiFi 连接失败，已达最大重试次数");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }

    } else if (event_base == IP_EVENT) {

        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "已获取 IP: " IPSTR, IP2STR(&event->ip_info.ip));
            s_retry_count = 0;
            s_wifi_connected = true;
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

/* ---- 公开 API ------------------------------------------------------- */

void wifi_sta_init(void)
{
    /* 1. 初始化 NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 初始化网络栈 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* 3. 创建事件组 */
    s_wifi_event_group = xEventGroupCreate();
    assert(s_wifi_event_group);

    /* 4. 注册事件回调 */
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL));

    /* 5. 初始化 WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "正在连接 WiFi: %s ...", WIFI_SSID);

    /* 6. 阻塞等待连接结果 */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi 连接成功");

        /* 7. 设置静态 DNS（阿里 DNS 223.5.5.5 + 谷歌 8.8.8.8） */
        esp_netif_dns_info_t dns;
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_str_to_ip4("223.5.5.5", &dns.ip.u_addr.ip4);
        esp_netif_set_dns_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"),
                               ESP_NETIF_DNS_MAIN, &dns);
        esp_netif_str_to_ip4("8.8.8.8", &dns.ip.u_addr.ip4);
        esp_netif_set_dns_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"),
                               ESP_NETIF_DNS_BACKUP, &dns);
        ESP_LOGI(TAG, "DNS: 223.5.5.5, 8.8.8.8");

        /* 8. 同步 SNTP 时间（HTTPS 证书校验需要） */
        sntp_sync_wait();

    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi 连接失败");
    }
}

bool wifi_is_connected(void)
{
    return s_wifi_connected;
}

void wifi_sta_deinit(void)
{
    s_wifi_connected = false;

    /* 注销事件回调 */
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                 &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                 &wifi_event_handler);

    /* 停止并释放 WiFi */
    esp_wifi_stop();
    esp_wifi_deinit();

    /* 清理事件组 */
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    ESP_LOGI(TAG, "WiFi 资源已释放");
}
