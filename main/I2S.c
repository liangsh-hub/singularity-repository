#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include "I2S.h"

#define TAG "I2S"

int32_t sample = 0;
size_t bytes_read = 0;
size_t bytes_written = 0;

/* 初始化 I2S*/
void i2s_init(void) {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX), // 主模式，接收和发送
        .sample_rate = I2S_SAMPLE_RATE,                                    // 采样率
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,                      // 16 位数据
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,                       // 单声道（左声道）
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,                 // 标准 I2S 格式
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,                          // 中断优先级
        .dma_buf_count = 8,                                                // DMA 缓冲区数量
        .dma_buf_len = I2S_BUFFER_SIZE,                                    // DMA 缓冲区长度
        .use_apll = false                                                  // 不使用 APLL
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK_GPIO,    // 位时钟
        .ws_io_num = I2S_WS_GPIO,      // 左右声道时钟
        .data_out_num = I2S_DOUT_GPIO, // 数据输出（连接到 MAX98357 的 DIN）
        .data_in_num = I2S_SD_GPIO     // 数据输入（连接到 INMP441 的 DOUT）
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
}

/* 处理 I2S 数据 */
void i2s_process_data(void) {
    while (1) {
        /* 从 INMP441 读取音频数据 */
        i2s_read(I2S_NUM_0, &sample, sizeof(sample), &bytes_read, portMAX_DELAY);

        /* 将音频数据发送到 MAX98357 */
        if (bytes_read > 0) {
            i2s_write(I2S_NUM_0, &sample, sizeof(sample), &bytes_written, portMAX_DELAY);
        }
    }
}