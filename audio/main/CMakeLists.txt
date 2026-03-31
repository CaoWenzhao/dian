idf_component_register(SRCS "audio_main.c"
                    REQUIRES app_update esp_wifi esp_netif esp_event driver esp_http_server
                    INCLUDE_DIRS ".")
