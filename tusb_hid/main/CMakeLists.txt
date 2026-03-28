# main/CMakeLists.txt
idf_component_register(
    SRCS "tusb_hid_example_main.c"
    INCLUDE_DIRS "."
    REQUIRES 
        tinyusb
        nvs_flash
        freertos
        driver
        esp_wifi
        esp_netif
        esp_event
        lwip
    PRIV_REQUIRES 
        esp_driver_gpio
)