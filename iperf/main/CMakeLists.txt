idf_component_register(SRCS "iperf_example_main.c"
                    INCLUDE_DIRS "."
		    REQUIRES esp_netif console driver esp_wifi nvs_flash iperf dns_server app_update)
