#ifndef _TTS_H_
#define _TTS_H_

#include <stdbool.h>

/**
 * @brief 初始化 TTS 语音合成引擎
 *        使用内置小乐（xiaole）中文女声，16kHz / 16bit 输出
 */
void TTS_Init(void);

/**
 * @brief 合成并播放中文文本
 *
 * @param chinese_text  待合成的中文 UTF-8 文本，如 "你好世界"
 * @note  此函数为阻塞调用，播放完毕后返回。
 *        播放期间会设置 g_tts_playing 标志，feed_Task 应检查此标志
 *        暂停向 AFE 送入音频数据，避免喇叭声音被麦克风回采造成误识别。
 */
void TTS_Speak(const char *chinese_text);

/**
 * @brief 设置 TTS 语速
 *
 * @param speed  语速等级：0（最慢）~ 5（最快），默认 2
 */
void TTS_SetSpeed(int speed);

/**
 * @brief 查询 TTS 是否正在播放
 *
 * @return true  正在播放
 * @return false 空闲
 */
bool TTS_IsPlaying(void);

/**
 * @brief 销毁 TTS 引擎，释放资源
 */
void TTS_Deinit(void);

#endif /* _TTS_H_ */
