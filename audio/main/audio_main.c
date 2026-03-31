#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_log.h"

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
static void init_i2s(void)
{
    // I2S配置
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 8,
        .dma_buf_len = BUFFER_SIZE,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };
    
    // I2S引脚配置
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    
    // 安装I2S驱动
    esp_err_t err = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S driver install failed: %d", err);
        return;
    }
    
    err = i2s_set_pin(I2S_NUM, &pin_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S set pin failed: %d", err);
        return;
    }
    
    ESP_LOGI(TAG, "I2S initialized successfully");
    ESP_LOGI(TAG, "  BCLK: GPIO%d, LRC: GPIO%d, DIN: GPIO%d", 
             I2S_BCK_PIN, I2S_WS_PIN, I2S_DOUT_PIN);
}

// 音频播放任务
static void audio_task(void *pvParameters)
{
    // 分配缓冲区（立体声，16位）
    size_t buffer_size = BUFFER_SIZE * 2 * sizeof(int16_t);
    int16_t *buffer = (int16_t*)malloc(buffer_size);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        vTaskDelete(NULL);
        return;
    }
    
    float t = 0.0f;
    const float dt = 1.0f / SAMPLE_RATE;
    size_t bytes_written;
    uint32_t button_pin;
    
    ESP_LOGI(TAG, "Audio task started");
    ESP_LOGI(TAG, "Press BOOT button to switch channels");
    
    while (1) {
        // 检查按钮按下
        if (xQueueReceive(button_queue, &button_pin, 0) == pdTRUE) {
            current_mode = (current_mode + 1) % 3;
            const char* mode_str[] = {"Left Only (1kHz)", "Right Only (4kHz)", "Stereo (1kHz+4kHz)"};
            ESP_LOGI(TAG, "Mode: %s", mode_str[current_mode]);
            vTaskDelay(pdMS_TO_TICKS(200));  // 消抖
        }
        
        // 填充缓冲区
        for (int i = 0; i < BUFFER_SIZE; i++) {
            int16_t left = generate_sine(t, 1000.0f);   // 左声道 1kHz
            int16_t right = generate_sine(t, 4000.0f);  // 右声道 4kHz
            
            switch(current_mode) {
                case MODE_LEFT_ONLY:
                    buffer[i * 2] = left;      // 左声道
                    buffer[i * 2 + 1] = 0;     // 右声道静音
                    break;
                case MODE_RIGHT_ONLY:
                    buffer[i * 2] = 0;          // 左声道静音
                    buffer[i * 2 + 1] = right; // 右声道
                    break;
                case MODE_STEREO:
                    buffer[i * 2] = left;       // 左声道
                    buffer[i * 2 + 1] = right;  // 右声道
                    break;
            }
            
            t += dt;
            if (t >= 1.0f) t -= 1.0f;
        }
        
        // 播放音频
        i2s_write(I2S_NUM, buffer, buffer_size, &bytes_written, portMAX_DELAY);
        
        // 检查写入是否完整
        if (bytes_written != buffer_size) {
            ESP_LOGW(TAG, "Only wrote %d/%d bytes", bytes_written, buffer_size);
        }
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