idf_component_register(SRCS "blink_example_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES 
                        nvs_flash
                        esp_wifi
                        esp_event
                        esp_netif
                        esp_http_server
                        freertos
                        led_strip
                        driver
                        app_update
                        bootloader_support)
set(image_file "${CMAKE_CURRENT_SOURCE_DIR}/../model/model.espdl")
# esptool_py_flash_to_partition(flash "model" "${image_file}")