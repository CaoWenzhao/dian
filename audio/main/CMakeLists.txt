idf_component_register(SRCS "audio_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES 
                        driver
                        esp_driver_i2s
                    REQUIRES esp_driver_gpio
                    )
