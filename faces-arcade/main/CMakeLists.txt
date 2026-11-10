set(app_sources "../src/main.cpp" "../src/sound.cpp" "../src/tts_kana.cpp" "../src/ui_icons.cpp"
    "../src/quiz.cpp" "../src/spectrum.cpp" "../src/game_blockdrop.cpp"
    "../src/game_snake.cpp" "../src/game_flappy.cpp" "../src/game_space3d.cpp" "../src/game_ten_seconds.cpp")
set(app_requires nvs_flash esp_timer esp_driver_usb_serial_jtag esp_driver_uart)
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND app_sources "../src/camera.cpp" "../src/photo_storage.cpp" "../src/vision.cpp"
        "../src/arcade_led.cpp" "../src/network_settings.cpp" "../src/network_serial.cpp"
        "../src/network_payload.cpp" "../src/network_result.cpp" "../src/network_wifi.cpp"
        "../src/network_http.cpp" "../src/http_response.cpp" "../src/network_job.cpp"
        "../src/network_ui.cpp" "../src/text_viewer.cpp")
    list(APPEND app_requires fatfs sdmmc esp_driver_sdspi esp_driver_gpio esp_wifi esp_netif
        esp_event json mbedtls http_parser microlink espressif__led_strip)
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "../src" "${CMAKE_CURRENT_BINARY_DIR}/generated"
    PRIV_REQUIRES ${app_requires} EMBED_FILES "../kana_voice.bin")
if(IDF_TARGET STREQUAL "esp32s3" AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(labels "${CMAKE_CURRENT_BINARY_DIR}/generated/imagenet_ja.h")
    add_custom_command(OUTPUT "${labels}"
        COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../scripts/generate_imagenet_labels.py" "${labels}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../assets/imagenet_ja.json"
                "${CMAKE_CURRENT_LIST_DIR}/../scripts/check_imagenet_labels.py"
                "${CMAKE_CURRENT_LIST_DIR}/../scripts/generate_imagenet_labels.py"
        VERBATIM)
    add_custom_target(faces_labels DEPENDS "${labels}")
    add_dependencies(${COMPONENT_LIB} faces_labels)
endif()
