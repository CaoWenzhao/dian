/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_server.h"

#include <inttypes.h>

#define MOUSE_BUTTON_LEFT   0x01
#define MOUSE_BUTTON_RIGHT  0x02
#define MOUSE_BUTTON_MIDDLE 0x04

#define APP_BUTTON (GPIO_NUM_0)

// WiFi AP配置
#define WIFI_AP_SSID           "ESP32_HID_AP"
#define WIFI_AP_PASS           "12345678"
#define WIFI_AP_CHANNEL        6
#define WIFI_AP_MAX_STA_CONN   4

// TCP服务器配置
#define TCP_SERVER_PORT        8888

static const char *TAG = "WiFi_HID";

/************* TinyUSB descriptors ****************/

#define TUSB_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_ITF_PROTOCOL_MOUSE))
};

const char* hid_string_descriptor[5] = {
    (char[]){0x09, 0x04},
    "Espressif",
    "ESP32-S3 HID Device",
    "12345678",
    "ESP32-S3 HID Interface",
};

static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

/********* TinyUSB HID callbacks ***************/

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
}

/********* WiFi AP 初始化 ***************/

static void wifi_ap_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .password = WIFI_AP_PASS,
            .max_connection = WIFI_AP_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    if (strlen(WIFI_AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started: %s", WIFI_AP_SSID);
    
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(ap_netif, &ip_info);
        ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&ip_info.ip));
    }
}

/********* 键盘映射 ***************/

typedef struct {
    const char *name;
    uint8_t code;
} key_map_t;

static const key_map_t key_map[] = {
    {"a", HID_KEY_A}, {"b", HID_KEY_B}, {"c", HID_KEY_C}, {"d", HID_KEY_D},
    {"e", HID_KEY_E}, {"f", HID_KEY_F}, {"g", HID_KEY_G}, {"h", HID_KEY_H},
    {"i", HID_KEY_I}, {"j", HID_KEY_J}, {"k", HID_KEY_K}, {"l", HID_KEY_L},
    {"m", HID_KEY_M}, {"n", HID_KEY_N}, {"o", HID_KEY_O}, {"p", HID_KEY_P},
    {"q", HID_KEY_Q}, {"r", HID_KEY_R}, {"s", HID_KEY_S}, {"t", HID_KEY_T},
    {"u", HID_KEY_U}, {"v", HID_KEY_V}, {"w", HID_KEY_W}, {"x", HID_KEY_X},
    {"y", HID_KEY_Y}, {"z", HID_KEY_Z},
    {"1", HID_KEY_1}, {"2", HID_KEY_2}, {"3", HID_KEY_3}, {"4", HID_KEY_4},
    {"5", HID_KEY_5}, {"6", HID_KEY_6}, {"7", HID_KEY_7}, {"8", HID_KEY_8},
    {"9", HID_KEY_9}, {"0", HID_KEY_0},
    {"enter", HID_KEY_ENTER}, {"space", HID_KEY_SPACE}, {"tab", HID_KEY_TAB},
    {"esc", HID_KEY_ESCAPE}, {"backspace", HID_KEY_BACKSPACE},
    {"up", HID_KEY_ARROW_UP}, {"down", HID_KEY_ARROW_DOWN},
    {"left", HID_KEY_ARROW_LEFT}, {"right", HID_KEY_ARROW_RIGHT},
};

static uint8_t get_keycode(const char *key_name)
{
    for (int i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
        if (strcmp(key_name, key_map[i].name) == 0) {
            return key_map[i].code;
        }
    }
    return 0;
}

/********* HID 发送函数 ***************/

static void send_keyboard(uint8_t modifiers, uint8_t *keys, int key_count)
{
    if (!tud_mounted()) return;
    
    uint8_t key_array[6] = {0};
    for (int i = 0; i < key_count && i < 6; i++) {
        key_array[i] = keys[i];
    }
    
    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, modifiers, key_array);
}

static void send_mouse(int8_t x, int8_t y, uint8_t buttons, int8_t wheel)
{
    if (!tud_mounted()) return;
    tud_hid_mouse_report(HID_ITF_PROTOCOL_MOUSE, buttons, x, y, wheel, 0);
}

/********* 协议解析 ***************/

// 解析键盘事件: "K:modifiers:keycode1,keycode2,..."
static void parse_keyboard_event(char *data)
{
    char *saveptr;
    char *token = strtok_r(data, ":", &saveptr);
    if (!token || strcmp(token, "K") != 0) return;
    
    token = strtok_r(NULL, ":", &saveptr);
    if (!token) return;
    uint8_t modifiers = (uint8_t)atoi(token);
    
    token = strtok_r(NULL, ":", &saveptr);
    if (!token) return;
    
    uint8_t keys[6] = {0};
    int key_count = 0;
    
    if (strlen(token) > 0) {
        char *key_token = strtok_r(token, ",", &saveptr);
        while (key_token != NULL && key_count < 6) {
            keys[key_count++] = (uint8_t)atoi(key_token);
            key_token = strtok_r(NULL, ",", &saveptr);
        }
    }
    
    ESP_LOGI(TAG, "Binary keyboard: mod=0x%02x, keys=%d", modifiers, key_count);
    
    if (tud_mounted()) {
        if (key_count > 0) {
            send_keyboard(modifiers, keys, key_count);
        } else {
            send_keyboard(0, NULL, 0);
        }
    }
}

// 解析鼠标事件: "M:x:y:buttons:wheel"
static void parse_mouse_event(char *data)
{
    char *saveptr;
    char *token = strtok_r(data, ":", &saveptr);
    if (!token || strcmp(token, "M") != 0) return;
    
    token = strtok_r(NULL, ":", &saveptr);
    if (!token) return;
    int8_t x = (int8_t)atoi(token);
    
    token = strtok_r(NULL, ":", &saveptr);
    if (!token) return;
    int8_t y = (int8_t)atoi(token);
    
    token = strtok_r(NULL, ":", &saveptr);
    if (!token) return;
    uint8_t buttons = (uint8_t)atoi(token);
    
    token = strtok_r(NULL, ":", &saveptr);
    int8_t wheel = token ? (int8_t)atoi(token) : 0;
    
    ESP_LOGI(TAG, "Binary mouse: x=%d, y=%d, btn=0x%02x", x, y, buttons);
    
    if (tud_mounted()) {
        send_mouse(x, y, buttons, wheel);
    }
}

/********* OTA 分区切换 ***************/

void print_running_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s at offset 0x%08" PRIx32, 
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

/********* HTTP 请求处理 ***************/

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char* html_page = 
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "    <title>ESP32 HID Control Panel</title>"
        "    <meta charset='utf-8'>"
        "    <meta name='viewport' content='width=device-width, initial-scale=1'>"
        "    <style>"
        "        body { font-family: Arial, sans-serif; text-align: center; margin-top: 30px; background: #1a1a2e; color: #eee; }"
        "        h1 { color: #00d4ff; }"
        "        .section { background: #16213e; border-radius: 10px; padding: 20px; margin: 20px auto; max-width: 500px; }"
        "        .section h2 { color: #00d4ff; margin-top: 0; }"
        "        button { font-size: 16px; padding: 10px 20px; margin: 8px; border: none; border-radius: 8px; cursor: pointer; transition: all 0.3s; }"
        "        button:hover { transform: scale(1.02); opacity: 0.9; }"
        "        .btn-switch { background: #f39c12; color: #1a1a2e; }"
        "        .status { margin-top: 10px; padding: 10px; background: #0f3460; border-radius: 8px; font-size: 14px; }"
        "    </style>"
        "    <script>"
        "        function updateStatus() {"
        "            fetch('/partition_status')"
        "            .then(response => response.json())"
        "            .then(data => {"
        "                document.getElementById('runningPartition').innerHTML = data.running_partition;"
        "            });"
        "        }"
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
        "    <h1>✨ ESP32-S3 HID 控制台 ✨</h1>"
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

/********* TCP服务器任务 ***************/

static void tcp_server_task(void *pvParameters)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(TCP_SERVER_PORT);
    
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Socket create failed");
        vTaskDelete(NULL);
        return;
    }
    
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    
    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "Socket listen failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "TCP server on port %d", TCP_SERVER_PORT);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (client_sock < 0) {
            continue;
        }
        
        char client_ip[16];
        inet_ntoa_r(client_addr.sin_addr, client_ip, sizeof(client_ip));
        ESP_LOGI(TAG, "Client connected: %s", client_ip);
        
        const char *welcome = "ESP32-S3 HID Control Server\r\n"
                             "Text commands:\r\n"
                             "  key a           - Press 'a'\r\n"
                             "  key ctrl c      - Ctrl+C\r\n"
                             "  mouse 10 20     - Move mouse\r\n"
                             "  type Hello      - Type text\r\n"
                             "Binary protocol:\r\n"
                             "  K:modifiers:keys\r\n"
                             "  M:x:y:buttons:wheel\r\n"
                             "> ";
        send(client_sock, welcome, strlen(welcome), 0);
        
        char cmd_buffer[256];
        int buffer_pos = 0;
        
        while (1) {
            char rx_char;
            int len = recv(client_sock, &rx_char, 1, 0);
            
            if (len <= 0) {
                ESP_LOGI(TAG, "Client disconnected");
                break;
            }
            
            if (rx_char == '\n') {
                if (buffer_pos > 0) {
                    cmd_buffer[buffer_pos] = '\0';
                    ESP_LOGI(TAG, "Command: %s", cmd_buffer);
                    
                    // 解析命令
                    if (cmd_buffer[0] == 'K' || cmd_buffer[0] == 'M') {
                        // 二进制协议
                        if (cmd_buffer[0] == 'K') {
                            parse_keyboard_event(cmd_buffer);
                        } else if (cmd_buffer[0] == 'M') {
                            parse_mouse_event(cmd_buffer);
                        }
                        send(client_sock, "OK\r\n> ", 6, 0);
                    } else {
                        // 文本命令
                        if (strncmp(cmd_buffer, "key ", 4) == 0) {
                            char *args = cmd_buffer + 4;
                            char *token = strtok(args, " ");
                            uint8_t modifiers = 0;
                            uint8_t keys[6] = {0};
                            int key_count = 0;
                            
                            while (token != NULL && key_count < 6) {
                                if (strcmp(token, "ctrl") == 0) {
                                    modifiers |= KEYBOARD_MODIFIER_LEFTCTRL;
                                } else if (strcmp(token, "shift") == 0) {
                                    modifiers |= KEYBOARD_MODIFIER_LEFTSHIFT;
                                } else if (strcmp(token, "alt") == 0) {
                                    modifiers |= KEYBOARD_MODIFIER_LEFTALT;
                                } else if (strcmp(token, "gui") == 0) {
                                    modifiers |= KEYBOARD_MODIFIER_LEFTGUI;
                                } else {
                                    uint8_t code = get_keycode(token);
                                    if (code) keys[key_count++] = code;
                                }
                                token = strtok(NULL, " ");
                            }
                            
                            if (key_count > 0 || modifiers > 0) {
                                ESP_LOGI(TAG, "Text key: mod=0x%02x, keys=%d", modifiers, key_count);
                                send_keyboard(modifiers, keys, key_count);
                                vTaskDelay(pdMS_TO_TICKS(50));
                                send_keyboard(0, NULL, 0);
                            }
                            send(client_sock, "OK\r\n> ", 6, 0);
                            
                        } else if (strncmp(cmd_buffer, "mouse ", 6) == 0) {
                            int x, y, buttons = 0;
                            sscanf(cmd_buffer + 6, "%d %d %d", &x, &y, &buttons);
                            if (x > 127) x = 127;
                            if (x < -127) x = -127;
                            if (y > 127) y = 127;
                            if (y < -127) y = -127;
                            ESP_LOGI(TAG, "Text mouse: x=%d, y=%d", x, y);
                            send_mouse((int8_t)x, (int8_t)y, buttons, 0);
                            if (buttons > 0) {
                                vTaskDelay(pdMS_TO_TICKS(50));
                                send_mouse(0, 0, 0, 0);
                            }
                            send(client_sock, "OK\r\n> ", 6, 0);
                            
                        } else if (strncmp(cmd_buffer, "type ", 5) == 0) {
                            char *text = cmd_buffer + 5;
                            ESP_LOGI(TAG, "Typing: %s", text);
                            for (char *p = text; *p; p++) {
                                uint8_t keycode = 0, mod = 0;
                                if (*p >= 'a' && *p <= 'z') {
                                    keycode = HID_KEY_A + (*p - 'a');
                                } else if (*p >= 'A' && *p <= 'Z') {
                                    keycode = HID_KEY_A + (*p - 'A');
                                    mod = KEYBOARD_MODIFIER_LEFTSHIFT;
                                } else if (*p == ' ') {
                                    keycode = HID_KEY_SPACE;
                                } else continue;
                                
                                if (keycode) {
                                    uint8_t k[6] = {keycode};
                                    send_keyboard(mod, k, 1);
                                    vTaskDelay(pdMS_TO_TICKS(50));
                                    send_keyboard(0, NULL, 0);
                                    vTaskDelay(pdMS_TO_TICKS(20));
                                }
                            }
                            send(client_sock, "OK\r\n> ", 6, 0);
                            
                        } else if (strcmp(cmd_buffer, "help") == 0) {
                            const char *help = "Commands:\n"
                                              "  key <key>       - Press a key\n"
                                              "  key ctrl c      - Ctrl+C\n"
                                              "  mouse <x> <y>   - Move mouse\n"
                                              "  type <text>     - Type text\n"
                                              "  help            - Show help\n";
                            send(client_sock, help, strlen(help), 0);
                            send(client_sock, "> ", 2, 0);
                        } else {
                            send(client_sock, "Unknown\r\n> ", 11, 0);
                        }
                    }
                    
                    buffer_pos = 0;
                }
            } else if (rx_char != '\r') {
                if (buffer_pos < sizeof(cmd_buffer) - 1) {
                    cmd_buffer[buffer_pos++] = rx_char;
                }
            }
        }
        
        close(client_sock);
    }
}

/********* USB监控任务 ***************/

static void usb_monitor_task(void *pvParameters)
{
    bool last_state = false;
    
    while (1) {
        bool mounted = tud_mounted();
        if (mounted != last_state) {
            if (mounted) {
                ESP_LOGI(TAG, "✅ USB HID MOUNTED");
            } else {
                ESP_LOGW(TAG, "⚠️ USB HID NOT MOUNTED");
            }
            last_state = mounted;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/********* 主函数 ***************/

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    wifi_ap_init();
    
    ESP_LOGI(TAG, "Initializing USB...");
    tinyusb_config_t tusb_cfg = {
        .device_descriptor = NULL,
        .string_descriptor = hid_string_descriptor,
        .string_descriptor_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]),
        .external_phy = false,
        .configuration_descriptor = hid_configuration_descriptor,
    };
    
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB initialized");
    
    // 打印当前分区
    print_running_partition();
    
    // 启动 HTTP 服务器
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.lru_purge_enable = true;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &root_uri);
        
        httpd_uri_t partition_status_uri = { .uri = "/partition_status", .method = HTTP_GET, .handler = partition_status_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &partition_status_uri);
        
        httpd_uri_t switch_uri = { .uri = "/switch", .method = HTTP_POST, .handler = switch_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &switch_uri);
        
        ESP_LOGI(TAG, "HTTP server started on 192.168.4.1");
        ESP_LOGI(TAG, "Open http://192.168.4.1 to control");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
    
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
    xTaskCreate(usb_monitor_task, "usb_monitor", 2048, NULL, 3, NULL);
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-S3 WiFi + USB HID Device");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "WiFi AP: %s (pwd: %s)", WIFI_AP_SSID, WIFI_AP_PASS);
    ESP_LOGI(TAG, "TCP Port: %d", TCP_SERVER_PORT);
    ESP_LOGI(TAG, "Commands: key a, mouse 10 20, type Hello");
    ESP_LOGI(TAG, "Binary: K:0:4, M:10:20:0:0");
    ESP_LOGI(TAG, "========================================");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
