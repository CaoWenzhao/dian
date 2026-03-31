#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_log.h"

static bool is_playing = true;  // 控制播放状态
static float saved_t = 0.0f;    // 保存断点的时间

static const char *TAG = "AUDIO";

// I2S配置
#define I2S_NUM         I2S_NUM_0
#define I2S_BCK_PIN     15
#define I2S_WS_PIN      16
#define I2S_DOUT_PIN    7

// 音频参数
#define SAMPLE_RATE     48000
#define BUFFER_SIZE     1024
#define AMPLITUDE       16000
#define BOOT_BUTTON     GPIO_NUM_0

typedef enum {
    MODE_LEFT_ONLY,
    MODE_RIGHT_ONLY,
    MODE_STEREO
} channel_mode_t;

static channel_mode_t current_mode = MODE_STEREO;
static QueueHandle_t button_queue = NULL;

// 生成正弦波
static int16_t generate_sine(float t, float freq)
{
    return (int16_t)(AMPLITUDE * sinf(2.0f * M_PI * freq * t));
}

// 按钮中断处理
static void IRAM_ATTR button_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(button_queue, &gpio_num, NULL);
}

// 初始化按钮
static void init_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);
    button_queue = xQueueCreate(10, sizeof(uint32_t));
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BUTTON, button_isr_handler, (void*)BOOT_BUTTON);
    
    ESP_LOGI(TAG, "Button initialized on GPIO%d", BOOT_BUTTON);
}

// 初始化I2S（使用旧版稳定API）
static void audio_task(void *pvParameters) {
    size_t buffer_size = BUFFER_SIZE * 2 * sizeof(int16_t);
    int16_t *buffer = (int16_t*)malloc(buffer_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        vTaskDelete(NULL);
        return;
    }
    
    float t = saved_t;  // 从保存的断点开始
    const float dt = 1.0f / SAMPLE_RATE;
    size_t bytes_written;
    uint32_t button_pin;
    
    ESP_LOGI(TAG, "Audio task started");
    ESP_LOGI(TAG, "Press BOOT button or use Web UI to control");
    
    while (1) {
        // 检查按钮按下
        if (xQueueReceive(button_queue, &button_pin, 0) == pdTRUE) {
            current_mode = (current_mode + 1) % 3;
            saved_t = t;  // 保存断点
            const char* mode_str[] = {"Left Only (1kHz)", "Right Only (4kHz)", "Stereo (1kHz+4kHz)"};
            ESP_LOGI(TAG, "Mode: %s", mode_str[current_mode]);
            vTaskDelay(pdMS_TO_TICKS(200));  // 消抖
        }
        
        if (is_playing) {
            // 填充缓冲区
            for (int i = 0; i < BUFFER_SIZE; i++) {
                int16_t left = generate_sine(t, 1000.0f);
                int16_t right = generate_sine(t, 4000.0f);
                
                switch(current_mode) {
                    case MODE_LEFT_ONLY:
                        buffer[i * 2] = left;
                        buffer[i * 2 + 1] = 0;
                        break;
                    case MODE_RIGHT_ONLY:
                        buffer[i * 2] = 0;
                        buffer[i * 2 + 1] = right;
                        break;
                    case MODE_STEREO:
                        buffer[i * 2] = left;
                        buffer[i * 2 + 1] = right;
                        break;
                }
                
                t += dt;
                if (t >= 1.0f) t -= 1.0f;
            }
            
            // 播放音频
            i2s_write(I2S_NUM, buffer, buffer_size, &bytes_written, portMAX_DELAY);
            
            if (bytes_written != buffer_size) {
                ESP_LOGW(TAG, "Only wrote %d/%d bytes", bytes_written, buffer_size);
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    
    free(buffer);
}


void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "I2S Audio Demo with MAX98357A");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Hardware connections:");
    ESP_LOGI(TAG, "  MAX98357A BCLK -> GPIO%d", I2S_BCK_PIN);
    ESP_LOGI(TAG, "  MAX98357A LRC  -> GPIO%d", I2S_WS_PIN);
    ESP_LOGI(TAG, "  MAX98357A DIN  -> GPIO%d", I2S_DOUT_PIN);
    ESP_LOGI(TAG, "  MAX98357A VIN  -> 3.3V or 5V");
    ESP_LOGI(TAG, "  MAX98357A GND  -> GND");
    ESP_LOGI(TAG, "========================================");
    
    // 初始化I2S
    init_i2s();
    
    // 初始化按钮
    init_button();
    
    // 创建音频任务
    xTaskCreate(audio_task, "audio_task", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "System ready! Playing 1kHz (left) + 4kHz (right)");
    ESP_LOGI(TAG, "Press BOOT button to switch channels");
}