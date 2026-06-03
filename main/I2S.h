#ifndef _I2S_H_
#define _I2S_H_

#include "driver/i2s.h"

/* 引脚定义 */
#define I2S_SCK_GPIO 7  // BCLK
#define I2S_WS_GPIO 6   // LRCLK
#define I2S_DOUT_GPIO 5 // MAX98357 的 DIN (数据输出)
#define I2S_SD_GPIO 4   // INMP441 的 DOUT (数据输入)

/* I2S 配置 */
#define I2S_SAMPLE_RATE 16000 // 采样率
#define I2S_BUFFER_SIZE 512   // 缓冲区大小

/* 初始化 I2S 的函数声明 */
void i2s_init(void);

/* 处理 I2S 数据的函数声明 */
void i2s_process_data(void);

#endif