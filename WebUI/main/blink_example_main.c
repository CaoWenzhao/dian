#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

// ==================== WS2812 配置 ====================
#define WS2812_GPIO_PIN      GPIO_NUM_48
#define WS2812_LED_NUM       1

// ==================== UART 配置 ====================
#define UART_PORT_NUM       UART_NUM_1
#define UART_TX_PIN         GPIO_NUM_17
#define UART_RX_PIN         GPIO_NUM_18
#define UART_BAUD_RATE      115200
#define UART_BUF_SIZE       1024

// ==================== 全局变量 ====================
static const char *TAG = "BaseFirmware";
static led_strip_handle_t led_strip = NULL;
static TaskHandle_t led_task_handle = NULL;
static TaskHandle_t uart_rx_task_handle = NULL;
static TaskHandle_t uart_tx_task_handle = NULL;

// 功能开关
static bool uart_enabled = false;
static bool hello_enabled = false;
static bool code_trigger_enabled = false;

// ==================== WS2812 初始化 ====================
void ws2812_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_GPIO_PIN,
        .max_leds = WS2812_LED_NUM,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false }
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = { .with_dma = false }
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    ESP_LOGI(TAG, "WS2812 initialized on GPIO %d", WS2812_GPIO_PIN);
}

void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(led_strip, 0, r, g, b);
    led_strip_refresh(led_strip);
}

void ws2812_off(void)
{
    ws2812_set_color(0, 0, 0);
}

// LED 呼吸特效
static void led_effect_task(void *pvParameters)
{
    uint8_t hue = 0;
    uint8_t brightness = 0;
    int8_t direction = 2;
    
    while (1) {
        uint8_t r, g, b;
        uint8_t shifted_hue = hue;
        
        if (shifted_hue < 85) {
            r = shifted_hue * 3;
            g = 255 - shifted_hue * 3;
            b = 0;
        } else if (shifted_hue < 170) {
            shifted_hue -= 85;
            r = 255 - shifted_hue * 3;
            g = 0;
            b = shifted_hue * 3;
        } else {
            shifted_hue -= 170;
            r = 0;
            g = shifted_hue * 3;
            b = 255 - shifted_hue * 3;
        }
        
        r = r * brightness / 255;
        g = g * brightness / 255;
        b = b * brightness / 255;
        
        ws2812_set_color(r, g, b);
        
        hue = (hue + 1) % 256;
        brightness += direction;
        if (brightness >= 255) {
            brightness = 255;
            direction = -2;
        } else if (brightness <= 0) {
            brightness = 0;
            direction = 2;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void start_led_effect(void)
{
    if (led_task_handle == NULL) {
        xTaskCreate(led_effect_task, "led_effect", 2048, NULL, 5, &led_task_handle);
        ESP_LOGI(TAG, "Started LED breathing effect");
    }
}

// ==================== UART 功能 ====================
void uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    ESP_LOGI(TAG, "UART initialized on TX:GPIO%d, RX:GPIO%d, baud:%d", UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);
}

void uart_send_data(const char* data)
{
    if (!uart_enabled) return;
    uart_write_bytes(UART_PORT_NUM, data, strlen(data));
    ESP_LOGI(TAG, "UART sent: %s", data);
}

void uart_send_codes(void)
{
    uart_send_data("GEL37KXHDU9G\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_send_data("FXLKNKWHVURC\r\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_send_data("CE4K7KEYCUPQ\r\n");
}

// UART 接收任务
static void uart_rx_task(void *pvParameters)
{
    uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE);
    char receive_buffer[256] = {0};
    int buffer_pos = 0;
    
    while (1) {
        if (uart_enabled && code_trigger_enabled) {
            int len = uart_read_bytes(UART_PORT_NUM, data, UART_BUF_SIZE, pdMS_TO_TICKS(100));
            if (len > 0) {
                for (int i = 0; i < len && buffer_pos < sizeof(receive_buffer) - 1; i++) {
                    receive_buffer[buffer_pos++] = data[i];
                }
                receive_buffer[buffer_pos] = '\0';
                
                // 检测 "111" 触发
                if (strstr(receive_buffer, "111") != NULL) {
                    ESP_LOGI(TAG, "Received '111', triggering code sending");
                    uart_send_codes();
                    buffer_pos = 0;
                    memset(receive_buffer, 0, sizeof(receive_buffer));
                }
                
                if (buffer_pos > 200) {
                    buffer_pos = 0;
                    memset(receive_buffer, 0, sizeof(receive_buffer));
                }
                ESP_LOGI(TAG, "UART received (%d bytes)", len);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    free(data);
}

// UART 发送任务 - 1Hz 发送 Hello
static void uart_tx_task(void *pvParameters)
{
    uint32_t hello_counter = 0;
    
    while (1) {
        if (uart_enabled && hello_enabled) {
            char hello_msg[64];
            sprintf(hello_msg, "Hello World %lu\r\n", hello_counter++);
            uart_send_data(hello_msg);
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ==================== OTA 分区切换 ====================
void print_running_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s at offset 0x%08x", 
             running->label, running->address);
}

esp_err_t switch_to_partition(const char* partition_label)
{
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, 
        ESP_PARTITION_SUBTYPE_ANY, 
        partition_label);
    
    if (partition == NULL) {
        ESP_LOGE(TAG, "Partition %s not found", partition_label);
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition");
        return err;
    }
    
    ESP_LOGI(TAG, "Will boot from %s next, restarting...", partition_label);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// ==================== WiFi SoftAP 配置 ====================
#define ESP_WIFI_SSID      "ESP32_Control_Panel"
#define ESP_WIFI_PASS      "12345678"

void wifi_init_softap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .password = ESP_WIFI_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi SoftAP started: %s", ESP_WIFI_SSID);
}

// ==================== HTTP 请求处理 ====================
static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char* html_page = 
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "    <title>ESP32 Base Control Panel</title>"
        "    <meta charset='utf-8'>"
        "    <meta name='viewport' content='width=device-width, initial-scale=1'>"
        "    <style>"
        "        body { font-family: Arial, sans-serif; text-align: center; margin-top: 30px; background: #1a1a2e; color: #eee; }"
        "        h1 { color: #00d4ff; }"
        "        .section { background: #16213e; border-radius: 10px; padding: 20px; margin: 20px auto; max-width: 500px; }"
        "        .section h2 { color: #00d4ff; margin-top: 0; }"
        "        button { font-size: 16px; padding: 10px 20px; margin: 8px; border: none; border-radius: 8px; cursor: pointer; transition: all 0.3s; }"
        "        button:hover { transform: scale(1.02); opacity: 0.9; }"
        "        .btn-on { background: #00d4ff; color: #1a1a2e; }"
        "        .btn-off { background: #2c3e50; color: white; }"
        "        .btn-enable { background: #4ecdc4; color: #1a1a2e; }"
        "        .btn-disable { background: #e74c3c; color: white; }"
        "        .btn-switch { background: #f39c12; color: #1a1a2e; }"
        "        .status { margin-top: 10px; padding: 10px; background: #0f3460; border-radius: 8px; font-size: 14px; }"
        "        .badge { display: inline-block; padding: 4px 12px; border-radius: 20px; font-size: 12px; margin-left: 10px; }"
        "        .badge-on { background: #00d4ff; color: #1a1a2e; }"
        "        .badge-off { background: #2c3e50; color: #aaa; }"
        "        .feature { margin: 15px 0; padding: 10px; border-left: 3px solid #00d4ff; text-align: left; }"
        "    </style>"
        "    <script>"
        "        function updateStatus() {"
        "            fetch('/uart_status')"
        "            .then(response => response.json())"
        "            .then(data => {"
        "                document.getElementById('uartEnabled').innerHTML = data.uart_enabled ?"
        "                    '<span class=\"badge badge-on\">● 已启用</span>' :"
        "                    '<span class=\"badge badge-off\">● 已禁用</span>';"
        "                document.getElementById('helloEnabled').innerHTML = data.hello_enabled ?"
        "                    '<span class=\"badge badge-on\">● 运行中</span>' :"
        "                    '<span class=\"badge badge-off\">● 已停止</span>';"
        "                document.getElementById('codeEnabled').innerHTML = data.code_trigger_enabled ?"
        "                    '<span class=\"badge badge-on\">● 监听中</span>' :"
        "                    '<span class=\"badge badge-off\">● 已关闭</span>';"
        "            });"
        "            fetch('/partition_status')"
        "            .then(response => response.json())"
        "            .then(data => {"
        "                document.getElementById('runningPartition').innerHTML = data.running_partition;"
        "            });"
        "        }"
        "        function uartOn() { fetch('/uart?action=on', { method: 'POST' }).then(() => updateStatus()); }"
        "        function uartOff() { fetch('/uart?action=off', { method: 'POST' }).then(() => updateStatus()); }"
        "        function setHello(enabled) { fetch('/uart?action=' + (enabled ? 'hello_start' : 'hello_stop'), { method: 'POST' }).then(() => updateStatus()); }"
        "        function setCodeTrigger(enabled) { fetch('/uart?action=' + (enabled ? 'code_start' : 'code_stop'), { method: 'POST' }).then(() => updateStatus()); }"
        "        function ledOn() { fetch('/led?state=on', { method: 'POST' }).then(() => updateStatus()); }"
        "        function ledOff() { fetch('/led?state=off', { method: 'POST' }).then(() => updateStatus()); }"
        "        function switchPartition(partition) {"
        "            if(confirm('切换到 ' + partition + ' 分区并重启？')) {"
        "                fetch('/switch?partition=' + partition, { method: 'POST' })"
        "                .then(response => response.text())"
        "                .then(data => alert(data));"
        "            }"
        "        }"
        "        setInterval(updateStatus, 2000);"
        "        window.onload = updateStatus;"
        "    </script>"
        "</head>"
        "<body>"
        "    <h1>✨ ESP32-S3 基础控制台 ✨</h1>"
        "    <div class='section'>"
        "        <h2>🔆 LED 控制</h2>"
        "        <button class='btn-on' onclick='ledOn()'>开启特效</button>"
        "        <button class='btn-off' onclick='ledOff()'>关闭 LED</button>"
        "    </div>"
        "    <div class='section'>"
        "        <h2>🔌 UART 串口控制</h2>"
        "        <div class='feature'>"
        "            <strong>🔓 总开关</strong>"
        "            <button class='btn-enable' onclick='uartOn()'>启用 UART</button>"
        "            <button class='btn-disable' onclick='uartOff()'>禁用 UART</button>"
        "            <span id='uartEnabled' class='badge badge-off'>● 未启用</span>"
        "        </div>"
        "        <div class='feature'>"
        "            <strong>💬 1Hz 发送 Hello World</strong><br>"
        "            <small>每秒发送一次 \"Hello World X\"</small><br>"
        "            <button class='btn-enable' onclick='setHello(true)'>▶ 启动</button>"
        "            <button class='btn-disable' onclick='setHello(false)'>⏹ 停止</button>"
        "            <span id='helloEnabled' class='badge badge-off'>● 已停止</span>"
        "        </div>"
        "        <div class='feature'>"
        "            <strong>📋 触发发送固定代码</strong><br>"
        "            <small>当收到 \"111\" 时，自动发送三行代码（支持多次触发）</small><br>"
        "            <button class='btn-enable' onclick='setCodeTrigger(true)'>👂 开启监听</button>"
        "            <button class='btn-disable' onclick='setCodeTrigger(false)'>🔇 关闭监听</button>"
        "            <span id='codeEnabled' class='badge badge-off'>● 已关闭</span>"
        "        </div>"
        "        <div class='status'>"
        "            <strong>📡 UART 信息</strong><br>"
        "            端口: UART1 | 波特率: 115200 | TX: GPIO17 | RX: GPIO18"
        "        </div>"
        "    </div>"
        "    <div class='section'>"
        "        <h2>🔄 功能切换 (OTA分区)</h2>"
        "        <div class='feature'>"
        "            <strong>当前运行分区: <span id='runningPartition'>---</span></strong>"
        "        </div>"
        "        <button class='btn-switch' onclick='switchPartition(\"ota_0\")'>🚀 切换到 ota_0</button>"
        "        <button class='btn-switch' onclick='switchPartition(\"ota_1\")'>🚀 切换到 ota_1</button>"
        "        <div class='status'>"
        "            <strong>📌 说明</strong><br>"
        "            ota_0: 模型推理功能<br>"
        "            ota_1: 音频播放功能<br>"
        "            <small>※ 切换后会重启设备</small>"
        "        </div>"
        "    </div>"
        "</body>"
        "</html>";
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

// UART状态查询接口
static esp_err_t uart_status_get_handler(httpd_req_t *req)
{
    char response[128];
    sprintf(response, "{\"uart_enabled\": %s, \"hello_enabled\": %s, \"code_trigger_enabled\": %s}", 
            uart_enabled ? "true" : "false",
            hello_enabled ? "true" : "false",
            code_trigger_enabled ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// 分区状态查询接口
static esp_err_t partition_status_get_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    char response[128];
    sprintf(response, "{\"running_partition\": \"%s\"}", running->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// UART控制接口
static esp_err_t uart_post_handler(httpd_req_t *req)
{
    char query[64];
    char action[32] = {0};
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "action", action, sizeof(action));
    }
    
    if (strcmp(action, "on") == 0) {
        uart_enabled = true;
        httpd_resp_send(req, "UART enabled", 12);
    } 
    else if (strcmp(action, "off") == 0) {
        uart_enabled = false;
        hello_enabled = false;
        code_trigger_enabled = false;
        httpd_resp_send(req, "UART disabled", 13);
    }
    else if (strcmp(action, "hello_start") == 0) {
        if (uart_enabled) {
            hello_enabled = true;
            httpd_resp_send(req, "Hello sending started", 22);
        } else {
            httpd_resp_send(req, "Please enable UART first", 24);
        }
    }
    else if (strcmp(action, "hello_stop") == 0) {
        hello_enabled = false;
        httpd_resp_send(req, "Hello sending stopped", 22);
    }
    else if (strcmp(action, "code_start") == 0) {
        if (uart_enabled) {
            code_trigger_enabled = true;
            httpd_resp_send(req, "Code trigger enabled, listening for '111'", 37);
        } else {
            httpd_resp_send(req, "Please enable UART first", 24);
        }
    }
    else if (strcmp(action, "code_stop") == 0) {
        code_trigger_enabled = false;
        httpd_resp_send(req, "Code trigger disabled", 21);
    }
    else {
        httpd_resp_send(req, "Unknown action", 14);
    }
    
    return ESP_OK;
}

// LED控制接口
static esp_err_t led_post_handler(httpd_req_t *req)
{
    char query[32];
    char state[16] = {0};
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "state", state, sizeof(state));
    }
    
    if (strcmp(state, "off") == 0) {
        if (led_task_handle != NULL) {
            vTaskDelete(led_task_handle);
            led_task_handle = NULL;
        }
        ws2812_off();
        ESP_LOGI(TAG, "LED turned off");
    } else {
        start_led_effect();
        ESP_LOGI(TAG, "LED effect started");
    }
    
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

// 分区切换接口
static esp_err_t switch_post_handler(httpd_req_t *req)
{
    char query[64];
    char partition[32] = {0};
    
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "partition", partition, sizeof(partition));
    }
    
    if (strlen(partition) > 0) {
        ESP_LOGI(TAG, "Switching to partition: %s", partition);
        switch_to_partition(partition);
        httpd_resp_send(req, "Switching partition...", 22);
    } else {
        httpd_resp_send(req, "Missing partition parameter", 27);
    }
    return ESP_OK;
}

// ==================== 主函数 ====================
void app_main(void)
{
    ESP_LOGI(TAG, "Base Firmware Starting...");
    
    // 打印当前分区
    print_running_partition();
    
    // 初始化硬件
    ws2812_init();
    uart_init();
    wifi_init_softap();
    start_led_effect();
    
    // 创建 UART 任务
    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 3, &uart_rx_task_handle);
    xTaskCreate(uart_tx_task, "uart_tx", 4096, NULL, 3, &uart_tx_task_handle);
    
    // 启动 HTTP 服务器
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.lru_purge_enable = true;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &root_uri);
        
        httpd_uri_t led_uri = { .uri = "/led", .method = HTTP_POST, .handler = led_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &led_uri);
        
        httpd_uri_t uart_uri = { .uri = "/uart", .method = HTTP_POST, .handler = uart_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uart_uri);
        
        httpd_uri_t uart_status_uri = { .uri = "/uart_status", .method = HTTP_GET, .handler = uart_status_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uart_status_uri);
        
        httpd_uri_t partition_status_uri = { .uri = "/partition_status", .method = HTTP_GET, .handler = partition_status_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &partition_status_uri);
        
        httpd_uri_t switch_uri = { .uri = "/switch", .method = HTTP_POST, .handler = switch_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &switch_uri);
        
        ESP_LOGI(TAG, "HTTP server started on 192.168.4.1");
        ESP_LOGI(TAG, "Open http://192.168.4.1 to control");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}