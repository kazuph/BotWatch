idf_component_register(
    SRCS "../src/main.cpp" "../src/sound.cpp" "../src/tts_kana.cpp"
         "../src/quiz.cpp" "../src/spectrum.cpp" "../src/game_blockdrop.cpp"
         "../src/game_snake.cpp" "../src/game_flappy.cpp"
    INCLUDE_DIRS "../src"
    PRIV_REQUIRES nvs_flash esp_timer esp_driver_usb_serial_jtag
    EMBED_FILES "../kana_voice.bin")
