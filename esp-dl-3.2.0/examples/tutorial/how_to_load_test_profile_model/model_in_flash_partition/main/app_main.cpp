#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "dl_model_base.hpp"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "inttypes.h"

/* WiFi SoftAP 相关头文件 */
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "MNIST_Inference";
static dl::Model *g_model = nullptr;
static TaskHandle_t tcp_server_task_handle = NULL;
static httpd_handle_t g_http_server = NULL;
static bool g_inference_running = false;
static SemaphoreHandle_t g_server_mutex = NULL;

/* ------------ 函数前置声明（修复编译错误）------------ */
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t partition_status_get_handler(httpd_req_t *req);
static esp_err_t switch_post_handler(httpd_req_t *req);
void stop_http_server(void);
void stop_inference_service(void);
void start_http_server(void);
esp_err_t switch_to_partition(const char* partition_label);
void print_running_partition(void);

/* WiFi SoftAP 配置 */
#define EXAMPLE_ESP_WIFI_SSID      "ESP32S3_MNIST"
#define EXAMPLE_ESP_WIFI_PASS      "12345678"
#define EXAMPLE_ESP_WIFI_CHANNEL   1
#define EXAMPLE_MAX_STA_CONN       4
#define EXAMPLE_GTK_REKEY_INTERVAL 0

/* TCP 服务器端口 */
#define TCP_PORT 8888

/* HTTP 服务器端口 */
#define HTTP_PORT 80

/* WiFi 事件处理 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}

/* 初始化 SoftAP */
void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.ap.ssid, EXAMPLE_ESP_WIFI_SSID);
    wifi_config.ap.ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID);
    wifi_config.ap.channel = EXAMPLE_ESP_WIFI_CHANNEL;
    strcpy((char*)wifi_config.ap.password, EXAMPLE_ESP_WIFI_PASS);
    wifi_config.ap.max_connection = EXAMPLE_MAX_STA_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.ap.pmf_cfg.required = true;
    wifi_config.ap.gtk_rekey_interval = EXAMPLE_GTK_REKEY_INTERVAL;

    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
}

/* Softmax 函数 */
void softmax(float *logits, float *probs, int size) {
    float max_logit = logits[0];
    for (int i = 1; i < size; i++) {
        if (logits[i] > max_logit) max_logit = logits[i];
    }
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        probs[i] = expf(logits[i] - max_logit);
        sum += probs[i];
    }
    for (int i = 0; i < size; i++) {
        probs[i] /= sum;
    }
}

/* 停止HTTP服务 */
void stop_http_server(void) {
    if (g_http_server) {
        httpd_stop(g_http_server);
        g_http_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

/* 停止推理服务 */
void stop_inference_service(void) {
    if (g_inference_running) {
        g_inference_running = false;
        if (tcp_server_task_handle) {
            vTaskDelay(pdMS_TO_TICKS(200));
            tcp_server_task_handle = NULL;
        }
        ESP_LOGI(TAG, "Inference service stopped");
    }
}

/* OTA 分区切换功能 */
void print_running_partition(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s at offset 0x%08" PRIx32,
             running->label, running->address);
}

esp_err_t switch_to_partition(const char* partition_label) {
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

/* 启动HTTP服务 */
void start_http_server(void) {
    if (g_http_server) {
        ESP_LOGW(TAG, "HTTP server already running");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.max_uri_handlers = 20;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.task_priority = 10;

    if (httpd_start(&g_http_server, &config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(g_http_server, &root_uri);

        httpd_uri_t partition_status_uri = { .uri = "/partition_status", .method = HTTP_GET, .handler = partition_status_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(g_http_server, &partition_status_uri);

        httpd_uri_t switch_uri = { .uri = "/switch", .method = HTTP_POST, .handler = switch_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(g_http_server, &switch_uri);

        ESP_LOGI(TAG, "HTTP server started on port %d", HTTP_PORT);
        ESP_LOGI(TAG, "Open http://192.168.4.1 to control");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        g_http_server = NULL;
    }
}

/* HTTP 请求处理函数 */
static esp_err_t root_get_handler(httpd_req_t *req) {
    const char* html_page =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "    <title>ESP32-S3 MNIST Inference</title>"
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
        "        .btn-base { background: #3498db; color: white; }"
        "        .status { margin-top: 10px; padding: 10px; background: #0f3460; border-radius: 8px; font-size: 14px; }"
        "        .info { color: #00d4ff; font-weight: bold; }"
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
        "    <h1>✨ ESP32-S3 MNIST 推理控制台 ✨</h1>"
        "    <div class='section'>"
        "        <h2>🔄 功能切换 (OTA分区)</h2>"
        "        <div class='feature'>"
        "            <strong>当前运行分区: <span id='runningPartition'>---</span></strong>"
        "        </div>"
        "        <button class='btn-switch' onclick='switchPartition(\"ota_0\")'>🚀 切换到 ota_0</button>"
        "        <button class='btn-switch' onclick='switchPartition(\"ota_1\")'>🚀 切换到 ota_1</button>"
        "        <button class='btn-base' onclick='switchPartition(\"factory\")'>🔙 切换回 Base 固件</button>"
        "        <div class='status'>"
        "            <strong>📌 说明</strong><br>"
        "            ota_0: 模型推理功能<br>"
        "            ota_1: 音频播放功能<br>"
        "            factory: Base 固件（默认功能）<br>"
        "            <small class='info'>※ 推理服务在TCP连接建立后自动启动，断开后自动停止并恢复HTTP服务</small><br>"
        "            <small>※ 切换分区后会重启设备</small>"
        "        </div>"
        "    </div>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

static esp_err_t partition_status_get_handler(httpd_req_t *req) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    char response[128];
    sprintf(response, "{\"running_partition\": \"%s\"}", running->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t switch_post_handler(httpd_req_t *req) {
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

/* TCP 服务器任务 */
static void tcp_server_task(void *pvParameters) {
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket");
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(listen_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(TCP_PORT);

    if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Socket bind failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 5) < 0) {
        ESP_LOGE(TAG, "Socket listen failed");
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP server started on port %d", TCP_PORT);
    g_inference_running = true;

    while (g_inference_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr*)&client_addr, &addr_len);

        if (client_sock < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(TAG, "Accept failed: %d", errno);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "Client connected");

        xSemaphoreTake(g_server_mutex, portMAX_DELAY);
        stop_http_server();
        xSemaphoreGive(g_server_mutex);

        static uint8_t discard_buf[56*56];
        static uint8_t buffer[56*56];
        static uint8_t img_28[28*28];
        static uint8_t peek_buf[56*56];
        bool client_connected = true;

        while (g_inference_running && client_connected) {
            const int IMG_SIZE = 56 * 56;

            while (g_inference_running && client_connected) {
                int ret = recv(client_sock, peek_buf, IMG_SIZE, MSG_PEEK | MSG_DONTWAIT);
                if (ret == IMG_SIZE) {
                    ret = recv(client_sock, discard_buf, IMG_SIZE, 0);
                    if (ret != IMG_SIZE) {
                        client_connected = false;
                        break;
                    }
                } else if (ret < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        client_connected = false;
                    }
                    break;
                } else {
                    break;
                }
            }

            if (!client_connected) break;

            int total = 0;
            while (total < IMG_SIZE && g_inference_running && client_connected) {
                int len = recv(client_sock, buffer + total, IMG_SIZE - total, 0);
                if (len <= 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        client_connected = false;
                    }
                    break;
                }
                total += len;
            }

            if (!client_connected || total != IMG_SIZE) break;

            for (int y = 0; y < 28; y++) {
                for (int x = 0; x < 28; x++) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; dy++) {
                        for (int dx = 0; dx < 2; dx++) {
                            sum += buffer[(y*2+dy)*56 + (x*2+dx)];
                        }
                    }
                    img_28[y*28 + x] = sum / 4;
                }
            }

            dl::TensorBase *input = g_model->get_input();
            if (!input || input->get_dtype() != dl::DATA_TYPE_FLOAT) {
                ESP_LOGE(TAG, "Input invalid");
                break;
            }

            float *input_data = (float*)input->get_element_ptr();
            for (int i = 0; i < 28*28; i++) {
                input_data[i] = img_28[i] / 255.0f;
            }

            g_model->run(dl::RUNTIME_MODE_SINGLE_CORE);
            vTaskDelay(pdMS_TO_TICKS(2)); // 轻微延时，大幅缓解卡顿

            dl::TensorBase *output = g_model->get_output();
            if (!output || output->get_dtype() != dl::DATA_TYPE_INT8) {
                ESP_LOGE(TAG, "Output invalid");
                break;
            }

            int8_t *int8_logits = (int8_t*)output->get_element_ptr();
            float scale = 1.0f / (1 << output->get_exponent());
            float logits[10], probs[10];
            for (int i = 0; i < 10; i++) {
                logits[i] = (float)int8_logits[i] * scale;
            }
            softmax(logits, probs, 10);

            int pred = 0;
            float maxp = probs[0];
            for (int i = 1; i < 10; i++) {
                if (probs[i] > maxp) {
                    maxp = probs[i];
                    pred = i;
                }
            }

            char response[256];
            snprintf(response, sizeof(response),
                     "{\"digit\":%d,\"confidence\":%.4f}\n",
                     pred, maxp);
            send(client_sock, response, strlen(response), 0);
            ESP_LOGI(TAG, "Predicted digit: %d, confidence: %.2f%%", pred, maxp*100);
        }

        close(client_sock);
        ESP_LOGI(TAG, "Client disconnected");

        xSemaphoreTake(g_server_mutex, portMAX_DELAY);
        stop_inference_service();
        start_http_server();
        xSemaphoreGive(g_server_mutex);
        break;
    }

    close(listen_sock);
    g_inference_running = false;
    tcp_server_task_handle = NULL;
    vTaskDelete(NULL);
}

extern "C" void app_main() {
    g_server_mutex = xSemaphoreCreateMutex();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    ESP_LOGI(TAG, "Starting WiFi SoftAP...");
    wifi_init_softap();

    ESP_LOGI(TAG, "Loading model from 'model' partition...");
    g_model = new dl::Model("model", fbs::MODEL_LOCATION_IN_FLASH_PARTITION,
                            0, dl::MEMORY_MANAGER_GREEDY, nullptr, false);
    if (!g_model) {
        ESP_LOGE(TAG, "Failed to create model");
        return;
    }
    ESP_LOGI(TAG, "Model loaded successfully");

    start_http_server();

    xTaskCreate(tcp_server_task, "tcp_server", 16384, NULL, 5, &tcp_server_task_handle);
    ESP_LOGI(TAG, "TCP server waiting for connections on port %d", TCP_PORT);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    delete g_model;
}