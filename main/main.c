#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "model_path.h"
#include "esp_mn_models.h"
#include "esp_mn_iface.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_mn_speech_commands.h"
#include "I2S.h"
#include "led.h"
#include "TTS.h"

#define TAG "app"

extern volatile bool g_tts_playing;

typedef struct {
    wakenet_state_t     wakenet_mode;
    esp_mn_state_t      state;
    int                 command_id;
} sr_result_t;

static QueueHandle_t            g_result_que    = NULL;
int detect_flag = 0;
static esp_afe_sr_iface_t *afe_handle = NULL;
static volatile int task_flag = 0;
static model_iface_data_t       *model_data     = NULL;
static const esp_mn_iface_t     *multinet       = NULL;
const char *cmd_phoneme[12] = {
    "kai shi xue xi",
    "kai shi liao tian",
    "xue xi ren wu",
    "wu",
    "liang du",
    "liang du gao yi dian",
    "liang du di yi dian",
    "yin liang",
    "yin liang da yi dian",
    "yin liang xiao yi dian",
    "da kai tai deng",
    "guan bi tai deng"
};

void feed_Task(void *arg)
{
    /* 获取音频处理数据结构指针 */
    esp_afe_sr_data_t *afe_data = arg;

    /* 获取AFE单次处理需要的音频数据块大小（单位：采样点数） */
    int feed_chunksize = afe_handle->get_feed_chunksize(afe_data);  /* 每帧输入的样本数 */
    int feed_nch = afe_handle->get_feed_channel_num(afe_data); /* 通道数 */

    int16_t *feed_buff = (int16_t *) malloc(feed_chunksize * feed_nch * sizeof(int16_t));

    assert(feed_buff); /* 内存分配失败时触发断言 */
    /* 主循环：持续采集和输入音频数据 */
    while (task_flag) {
        size_t bytesIn = 0;
        esp_err_t result = i2s_read(I2S_NUM_0, feed_buff, feed_chunksize * feed_nch * sizeof(int16_t), &bytesIn, portMAX_DELAY);

        /* TTS 播放期间跳过 AFE 喂入，避免喇叭回采造成误识别 */
        if (!g_tts_playing) {
            afe_handle->feed(afe_data, feed_buff);
        }
    }

    /* 任务退出时释放资源 */
    if (feed_buff) {
        free(feed_buff); /* 释放音频缓冲区 */
        feed_buff = NULL;
    }
    vTaskDelete(NULL); /* 删除当前FreeRTOS任务 */
}


void detect_Task(void *arg)
{
    /* 获取音频处理数据结构指针（类型转换） */
    esp_afe_sr_data_t *afe_data = (esp_afe_sr_data_t *)arg;

    /* 获取AFE单次输出数据块大小（单位：采样点数） */
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);

    /* 分配缓冲区用于临时存储音频数据 */
    int16_t *buff = malloc(afe_chunksize * sizeof(int16_t));
    assert(buff); /* 确保内存分配成功 */

    printf("------------detect start------------\n");

    /* 主任务循环（受task_flag全局变量控制） */
    while (task_flag) {
        /* TTS 播放期间暂停检测，避免 AFE 管道残留数据触发误识别 */
        if (g_tts_playing) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* 从AFE获取处理后的音频数据 */
        afe_fetch_result_t* res = afe_handle->fetch(afe_data);

        /* 错误处理：获取数据失败时退出循环 */
        /* 但如果 TTS 正在播放（feed 已暂停），fetch 失败是预期行为 */
        if (!res || res->ret_value == ESP_FAIL) {
            printf("fetch error!\n");
            if (g_tts_playing) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            break;
        }

        /*---------- 唤醒词检测处理 ----------*/
        if (res->wakeup_state == WAKENET_DETECTED) { /* 检测到有效唤醒词 */
            printf("-----------LISTENING-----------\n");

            /* 构造唤醒检测结果结构体 */
            sr_result_t result = {
                .wakenet_mode = WAKENET_DETECTED,  /* 唤醒模式标识 */
                .state = ESP_MN_STATE_DETECTING,   /* 进入语音检测状态 */
                .command_id = 0,                   /* 初始命令ID */
            };

            /* 发送结果到消息队列（超时时间10 ticks） */
            xQueueSend(g_result_que, &result, 10);
            detect_flag = true; /* 设置检测标志开始监听指令 */

        } else if (res->wakeup_state == WAKENET_CHANNEL_VERIFIED) { 
            /* 通道验证成功（可选状态，视具体应用场景） */
            detect_flag = true;
            afe_handle->disable_wakenet(afe_data); /* 禁用唤醒网络以节省资源 */
        }

        /*---------- 语音指令识别处理 ----------*/
        if (detect_flag) {
            /* 初始化语音识别状态 */
            esp_mn_state_t mn_state = ESP_MN_STATE_DETECTING;

            /* 执行多网络语音识别检测 */
            mn_state = multinet->detect(model_data, res->data);

            /* 仍在检测中则继续循环 */
            if (ESP_MN_STATE_DETECTING == mn_state) {
                continue;
            }

            /* 处理超时状态 */
            if (ESP_MN_STATE_TIMEOUT == mn_state) {
                ESP_LOGW(TAG, "检测超时，退出检测"); /* 记录超时日志 */
                
                /* 构造超时结果结构体 */
                sr_result_t result = {
                    .wakenet_mode = WAKENET_NO_DETECT,
                    .state = mn_state,
                    .command_id = 0,
                };
                
                xQueueSend(g_result_que, &result, 10);  /* 发送超时通知 */
                afe_handle->enable_wakenet(afe_data);   /* 重新启用唤醒网络 */
                detect_flag = false; /* 重置检测标志 */
                continue;
            }

            /* 成功检测到语音指令 */
            if (ESP_MN_STATE_DETECTED == mn_state) {
                /* 获取多网络识别结果 */
                esp_mn_results_t *mn_result = multinet->get_results(model_data);
                
                /* 遍历打印所有候选结果 */
                for (int i = 0; i < mn_result->num; i++) {
                    ESP_LOGI(TAG, "TOP %d, command_id: %d, phrase_id: %d, prob: %f",
                            i + 1, mn_result->command_id[i],
                            mn_result->phrase_id[i], mn_result->prob[i]);
                    printf("model index:%d, word index:%d\n", mn_result->command_id[i], mn_result->phrase_id[i]);
                }

                /* 取置信度最高的指令ID */
                int sr_command_id = mn_result->command_id[0];
                ESP_LOGI(TAG, "Detected command : %d", sr_command_id);
                
                /* 构造指令识别结果结构体 */
                sr_result_t result = {
                    .wakenet_mode = WAKENET_NO_DETECT,
                    .state = mn_state,
                    .command_id = sr_command_id,
                };

                xQueueSend(g_result_que, &result, 10); /* 发送识别结果 */
            }
        }
    }

    /*---------- 资源清理 ----------*/
    if (buff) {
        free(buff);  /* 释放音频缓冲区内存 */
        buff = NULL; /* 避免野指针 */
    }

    vTaskDelete(NULL); /* 删除当前任务 */
}

void sr_handler_Task(void *pvParam)
{
    QueueHandle_t xQueue = (QueueHandle_t) pvParam;

    while (true) {
        sr_result_t result;
        xQueueReceive(xQueue, &result, portMAX_DELAY);

        ESP_LOGD(TAG, "cmd:%d, wakemode:%d,state:%d", result.command_id, result.wakenet_mode, result.state);

        if (ESP_MN_STATE_TIMEOUT == result.state) {
            ESP_LOGD(TAG, "timeout");
            continue;
        }

        if (WAKENET_DETECTED == result.wakenet_mode) {
            ESP_LOGD(TAG, "wakenet detected");
            continue;
        }

        if (ESP_MN_STATE_DETECTED == result.state) {
            ESP_LOGI(TAG, "识别到命令ID: %d", result.command_id);

            switch(result.command_id) {
                case 0:
                    ESP_LOGI(TAG, "执行：开始学习");
                    TTS_Speak("好的，开始学习模式");
                    break;
                case 1:
                    ESP_LOGI(TAG, "执行：开始聊天");
                    TTS_Speak("好的，进入聊天模式");
                    break;
                case 2:
                    ESP_LOGI(TAG, "执行：学习任务");
                    TTS_Speak("好的，查看学习任务");
                    break;
                case 3:
                    ESP_LOGI(TAG, "执行：无");
                    break;
                case 4:
                    ESP_LOGI(TAG, "执行：亮度");
                    TTS_Speak("正在调节亮度");
                    break;
                case 5:
                    ESP_LOGI(TAG, "执行：亮度高一点");
                    TTS_Speak("好的，亮度已调高");
                    break;
                case 6:
                    ESP_LOGI(TAG, "执行：亮度低一点");
                    TTS_Speak("好的，亮度已调低");
                    break;
                case 7:
                    ESP_LOGI(TAG, "执行：音量");
                    TTS_Speak("正在调节音量");
                    break;
                case 8:
                    ESP_LOGI(TAG, "执行：音量大一点");
                    TTS_Speak("好的，音量已调大");
                    break;
                case 9:
                    ESP_LOGI(TAG, "执行：音量小一点");
                    TTS_Speak("好的，音量已调小");
                    break;
                case 10:
                    ESP_LOGI(TAG, "执行：打开台灯");
                    LED(1);
                    TTS_Speak("好的，已打开台灯");
                    break;
                case 11:
                    ESP_LOGI(TAG, "执行：关闭台灯");
                    LED(0);
                    TTS_Speak("好的，已关闭台灯");
                    break;
                default:
                    ESP_LOGW(TAG, "识别到未知命令");
                    break;
            }
        }
    }
}

int app_main()
{
    led_init();
    /* 初始化NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    TTS_Init();
    i2s_init();
    /* 匹配分区表 model 分区 */
    srmodel_list_t *models = esp_srmodel_init("model");
    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);

    afe_config->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    afe_config->aec_init = false;
    afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);

    char *mn_name = esp_srmodel_filter(models, ESP_MN_CHINESE, NULL);
    if (NULL == mn_name) {
        printf("No multinet model found");
        return ESP_FAIL;
    } else {
        multinet = esp_mn_handle_from_name(mn_name);
        model_data = multinet->create(mn_name, 5760); /* 设置唤醒超时时间 */
        printf( "load multinet:%s", mn_name);
        esp_mn_commands_clear(); /* 清除唤醒指令列表 */
        for (int i = 0; i < sizeof(cmd_phoneme) / sizeof(cmd_phoneme[0]); i++) {
            esp_mn_commands_add(i, (char *)cmd_phoneme[i]); /* 逐个将唤醒指令放入 */
        }
        esp_mn_commands_update(); /* 更新命令词列表 */
        esp_mn_commands_print();
        multinet->print_active_speech_commands(model_data); /* 输出目前激活的命令词 */
    }

    afe_config_free(afe_config);

    task_flag = 1;

    g_result_que = xQueueCreate(10, sizeof(sr_result_t));

    xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void*)afe_data, 5, NULL, 0);
    xTaskCreatePinnedToCore(&detect_Task, "detect", 4 * 1024, (void*)afe_data, 5, NULL, 1);
    xTaskCreatePinnedToCore(&sr_handler_Task, "SR Handler Task", 8 * 1024, g_result_que, 1, NULL, 1);

    return 0;
}