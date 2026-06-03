/**
 * @file    WiFi.h
 * @brief   WiFi STA 模式管理
 *
 * 提供 WiFi 连接、状态查询、资源释放功能。
 * wifi_sta_init() 会阻塞等待获取 IP，内部自动同步 SNTP 时间。
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 WiFi STA 模式并阻塞等待获取 IP
 * @note   内部自动同步 SNTP 时间（HTTPS 证书校验需要）
 *         连接失败会自动重试（最多 30 次，间隔 10 秒）
 */
void wifi_sta_init(void);

/**
 * @brief  查询 WiFi 是否已连接
 * @return true 已连接，false 未连接
 */
bool wifi_is_connected(void);

/**
 * @brief  断开 WiFi 并释放资源
 */
void wifi_sta_deinit(void);

#ifdef __cplusplus
}
#endif
