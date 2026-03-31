/* Wi-Fi iperf Example + WiFi NAT 转发（软路由） */

#include <errno.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_console.h"
#include "cmd_system.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lwip/opt.h"
#include "lwip/lwip_napt.h"
#include "dns_server.h"
#include "esp_ota_ops.h"        // 添加 OTA 支持
#include "esp_partition.h"

/* component manager */
#include "iperf.h"
#include "wifi_cmd.h"
#include "iperf_cmd.h"
#include "ping_cmd.h"

#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS || CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
#include "esp_wifi_he.h"
#endif
#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
extern int wifi_cmd_get_tx_statistics(int argc, char **argv);
extern int wifi_cmd_clr_tx_statistics(int argc, char **argv);
#endif
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
extern int wifi_cmd_get_rx_statistics(int argc, char **argv);
extern int wifi_cmd_clr_rx_statistics(int argc, char **argv);
#endif

#ifdef CONFIG_ESP_EXT_CONN_ENABLE
#include "esp_extconn.h"
#endif

// 要连接的WiFi（上网用）
#define EXAMPLE_ESP_WIFI_SSID      "Power"
#define EXAMPLE_ESP_WIFI_PASS      "Power123"
#define EXAMPLE_ESP_WIFI_MAXIMUM_RETRY  5

// ESP32 AP热点（给其他设备连）
#define EXAMPLE_ESP_WIFI_AP_SSID      "ESP32_Iperf_AP"
#define EXAMPLE_ESP_WIFI_AP_PASS      "12345678"
#define EXAMPLE_ESP_WIFI_AP_CHANNEL   6
#define EXAMPLE_ESP_WIFI_AP_MAX_STA_CONN  4

static const char *TAG = "WiFi_Iperf";
static int s_retry_num = 0;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

/* WiFi事件处理函数 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Station started, connecting to AP...");
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGI(TAG, "Disconnected from AP, reason: %d", event->reason);
        
        if (s_retry_num < EXAMPLE_ESP_WIFI_MAXIMUM_RETRY) {
            ESP_LOGI(TAG, "Retry to connect to the AP (retry: %d)", s_retry_num + 1);
            esp_wifi_connect();
            s_retry_num++;
        } else {
            ESP_LOGI(TAG, "Failed to connect to AP after %d retries", s_retry_num);
            ESP_LOGI(TAG, "Will continue with AP mode only");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;

        // 拿到IP后立刻开启NAT转发
        ip_napt_enable(event->ip_info.ip.addr, 1);
        ESP_LOGI(TAG, "NAT 转发已启动");
    }
}

/* 初始化STA */
static void wifi_init_sta(void)
{
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = false,
                .required = false
            },
        },
    };
    
    ESP_LOGI(TAG, "Connecting to %s...", EXAMPLE_ESP_WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
}

/* 初始化AP */
static void wifi_init_ap(void)
{
    // 手动配置AP的IP、网关、DNS
    esp_netif_ip_info_t ap_ip_info;
    IP4_ADDR(&ap_ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip_info.netmask, 255, 255, 255, 0);
    esp_netif_set_ip_info(ap_netif, &ap_ip_info);

    // 启动 DHCP 服务器
    esp_netif_dhcps_start(ap_netif);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_AP_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_AP_SSID),
            .channel = EXAMPLE_ESP_WIFI_AP_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_AP_PASS,
            .max_connection = EXAMPLE_ESP_WIFI_AP_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = { .required = false, },
        },
    };
    
    if (strlen(EXAMPLE_ESP_WIFI_AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    
    ESP_LOGI(TAG, "Starting AP: %s", EXAMPLE_ESP_WIFI_AP_SSID);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
}

/* 网络初始化 */
static void wifi_netif_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    
    assert(sta_netif);
    assert(ap_netif);
}


static int cmd_switch_to_factory(int argc, char **argv)
{
    const esp_partition_t *factory_partition = NULL;
    const esp_partition_t *running_partition = NULL;
    esp_err_t ret;
    
    // 获取当前运行的分区
    running_partition = esp_ota_get_running_partition();
    if (running_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get running partition");
        return -1;
    }
    
    ESP_LOGI(TAG, "Current running partition: %s", running_partition->label);
    
    // 查找 factory 分区
    factory_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, 
                                                  ESP_PARTITION_SUBTYPE_APP_FACTORY, 
                                                  NULL);
    if (factory_partition == NULL) {
        ESP_LOGE(TAG, "Factory partition not found!");
        return -1;
    }
    
    ESP_LOGI(TAG, "Found factory partition: %s at offset 0x%08x", 
             factory_partition->label, factory_partition->address);
    
    // 如果当前已经在 factory 分区，不需要切换
    if (running_partition->address == factory_partition->address) {
        ESP_LOGI(TAG, "Already running from factory partition");
        return 0;
    }
    
    // 设置启动分区为 factory
    ret = esp_ota_set_boot_partition(factory_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(ret));
        return -1;
    }
    
    ESP_LOGI(TAG, "Boot partition set to factory. Rebooting in 3 seconds...");
    
    // 延迟3秒后重启
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    
    return 0;
}

/* OTA 命令：切换到 OTA_0 分区 */
static int cmd_switch_to_ota0(int argc, char **argv)
{
    const esp_partition_t *ota0_partition = NULL;
    const esp_partition_t *running_partition = NULL;
    esp_err_t ret;
    
    running_partition = esp_ota_get_running_partition();
    if (running_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get running partition");
        return -1;
    }
    
    ESP_LOGI(TAG, "Current running partition: %s", running_partition->label);
    
    // 查找 OTA_0 分区
    ota0_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, 
                                               ESP_PARTITION_SUBTYPE_APP_OTA_0, 
                                               NULL);
    if (ota0_partition == NULL) {
        ESP_LOGE(TAG, "OTA_0 partition not found!");
        return -1;
    }
    
    ESP_LOGI(TAG, "Found OTA_0 partition: %s at offset 0x%08x", 
             ota0_partition->label, ota0_partition->address);
    
    if (running_partition->address == ota0_partition->address) {
        ESP_LOGI(TAG, "Already running from OTA_0 partition");
        return 0;
    }
    
    ret = esp_ota_set_boot_partition(ota0_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(ret));
        return -1;
    }
    
    ESP_LOGI(TAG, "Boot partition set to OTA_0. Rebooting in 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    
    return 0;
}

/* OTA 命令：切换到 OTA_1 分区 */
static int cmd_switch_to_ota1(int argc, char **argv)
{
    const esp_partition_t *ota1_partition = NULL;
    const esp_partition_t *running_partition = NULL;
    esp_err_t ret;
    
    running_partition = esp_ota_get_running_partition();
    if (running_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get running partition");
        return -1;
    }
    
    ESP_LOGI(TAG, "Current running partition: %s", running_partition->label);
    
    // 查找 OTA_1 分区
    ota1_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, 
                                               ESP_PARTITION_SUBTYPE_APP_OTA_1, 
                                               NULL);
    if (ota1_partition == NULL) {
        ESP_LOGE(TAG, "OTA_1 partition not found!");
        return -1;
    }
    
    ESP_LOGI(TAG, "Found OTA_1 partition: %s at offset 0x%08x", 
             ota1_partition->label, ota1_partition->address);
    
    if (running_partition->address == ota1_partition->address) {
        ESP_LOGI(TAG, "Already running from OTA_1 partition");
        return 0;
    }
    
    ret = esp_ota_set_boot_partition(ota1_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(ret));
        return -1;
    }
    
    ESP_LOGI(TAG, "Boot partition set to OTA_1. Rebooting in 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    
    return 0;
}

/* OTA 命令：显示当前分区信息 */
static int cmd_show_partition(int argc, char **argv)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    
    if (running) {
        ESP_LOGI(TAG, "Running partition: %s (type: 0x%02x, subtype: 0x%02x, addr: 0x%08x, size: %d)",
                 running->label, running->type, running->subtype, 
                 running->address, running->size);
    }
    
    if (boot) {
        ESP_LOGI(TAG, "Boot partition: %s (type: 0x%02x, subtype: 0x%02x, addr: 0x%08x, size: %d)",
                 boot->label, boot->type, boot->subtype, 
                 boot->address, boot->size);
    }
    
    // 列出所有 APP 分区
    ESP_LOGI(TAG, "All APP partitions:");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, 
                                                     ESP_PARTITION_SUBTYPE_ANY, 
                                                     NULL);
    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        ESP_LOGI(TAG, "  - %s: addr=0x%08x, size=%d", p->label, p->address, p->size);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    
    return 0;
}

/* 注册 OTA 命令 */
static void register_ota_commands(void)
{
    esp_console_cmd_t cmd = {
        .command = "switch_to_factory",
        .help = "Switch to factory partition and reboot",
        .hint = NULL,
        .func = &cmd_switch_to_factory,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    
    cmd.command = "switch_to_ota0";
    cmd.help = "Switch to OTA_0 partition and reboot";
    cmd.func = &cmd_switch_to_ota0;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    
    cmd.command = "switch_to_ota1";
    cmd.help = "Switch to OTA_1 partition and reboot";
    cmd.func = &cmd_switch_to_ota1;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    
    cmd.command = "show_partition";
    cmd.help = "Show current partition information";
    cmd.func = &cmd_show_partition;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void iperf_hook_show_wifi_stats(iperf_traffic_type_t type, iperf_status_t status)
{
    if (status == IPERF_STARTED) {
#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
        if (type != IPERF_UDP_SERVER) {
            wifi_cmd_clr_tx_statistics(0, NULL);
        }
#endif
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
        if (type != IPERF_UDP_CLIENT) {
            wifi_cmd_clr_rx_statistics(0, NULL);
        }
#endif
    }

    if (status == IPERF_STOPPED) {
#if CONFIG_ESP_WIFI_ENABLE_WIFI_TX_STATS
        if (type != IPERF_UDP_SERVER) {
            wifi_cmd_get_tx_statistics(0, NULL);
        }
#endif
#if CONFIG_ESP_WIFI_ENABLE_WIFI_RX_STATS
        if (type != IPERF_UDP_CLIENT) {
            wifi_cmd_get_rx_statistics(0, NULL);
        }
#endif
    }
}

void app_main(void)
{
#if CONFIG_ESP_EXT_CONN_ENABLE
    esp_extconn_config_t ext_config = ESP_EXTCONN_CONFIG_DEFAULT();
    esp_extconn_init(&ext_config);
#endif

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 网络初始化
    wifi_netif_init();
    
    // WiFi初始化
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 注册事件
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    
    // AP+STA 模式
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_init_ap();
    wifi_init_sta();
    ESP_ERROR_CHECK(esp_wifi_start());

    // 打印信息
    wifi_ap_record_t ap_info;
    esp_err_t sta_status = esp_wifi_sta_get_ap_info(&ap_info);
    if (sta_status == ESP_OK) {
        ESP_LOGI(TAG, "Connected to: %s", EXAMPLE_ESP_WIFI_SSID);
    }

    esp_netif_ip_info_t ip_info;
    if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&ip_info.ip));
    }
    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&ip_info.ip));
    }

    // 控制台初始化
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "iperf>";

#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
#endif

    register_system();
    app_register_all_wifi_commands();
    app_register_iperf_commands();
    ping_cmd_register_ping();
    app_register_iperf_hook_func(iperf_hook_show_wifi_stats);

     register_ota_commands();
    ESP_LOGI(TAG, "系统启动完成，NAT 转发已启用");
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}