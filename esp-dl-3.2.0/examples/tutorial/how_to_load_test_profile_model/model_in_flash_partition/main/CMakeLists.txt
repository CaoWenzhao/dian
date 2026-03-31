set(srcs    app_main.cpp)

set(requires    esp-dl)

idf_component_register(SRCS ${srcs}
                       REQUIRES ${requires}
                       PRIV_REQUIRES esp_wifi nvs_flash esp_http_server app_update)

if (IDF_TARGET STREQUAL "esp32s3")
    set(image_file ${COMPONENT_DIR}/models/s3/model.espdl)
elseif (IDF_TARGET STREQUAL "esp32p4")
    set(image_file ${COMPONENT_DIR}/models/p4/model.espdl)
endif()

esptool_py_flash_to_partition(flash "model" "${image_file}")
