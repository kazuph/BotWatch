set(BOTWATCH_PET_COUNT 0)
set(pet_manifest "${CMAKE_CURRENT_LIST_DIR}/../assets/pets/manifest.json")
if(EXISTS "${pet_manifest}")
  file(READ "${pet_manifest}" pet_json)
  string(JSON BOTWATCH_PET_COUNT GET "${pet_json}" count)
  if(BOTWATCH_PET_COUNT LESS 0 OR BOTWATCH_PET_COUNT GREATER 3)
    message(FATAL_ERROR "supported pet count is 0..3")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pet_manifest}")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/pet_config.h" "#pragma once\nconstexpr int kPetCount=${BOTWATCH_PET_COUNT};\n")
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ayousanz
idf_component_register(SRCS "radio_app.cpp" "radio/transport/esp_now_radio.cpp" "pomodoro.cpp" "ear_game.cpp" "pitch_detector.cpp" "apps.cpp" "main.cpp" "features.cpp" "voice_screen.cpp" "ble_printer.cpp" "label_raster.cpp"
                            "tts.cpp" "tts_text.cpp" "action.cpp" "characters.cpp" "home.cpp" "koeact/a2.c"
                       INCLUDE_DIRS "." "koeact" "${CMAKE_CURRENT_BINARY_DIR}/generated"
                       PRIV_REQUIRES esp_wifi esp_netif esp_timer esp_psram esp_driver_usb_serial_jtag vfs bt nvs_flash
                                     esp_partition spi_flash json sanotts
                       EMBED_FILES "../assets/ui15.vlw" "../assets/ui17.vlw" "../assets/ui20.vlw" "../assets/ui22.vlw" "../assets/ui24.vlw" "../assets/ui32.vlw" "../assets/ui34.vlw" "../assets/ui38.vlw" "../assets/ui40.vlw" "../assets/ui56.vlw")

# `idf.py flash` (and build/flash_args) also writes the .espdl into the "model" partition, and
# the KoeAct files into "koeact" and "a2".
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  foreach(file "${RALOMI_ESPDL}" "${KOEACT_ESPDL}" "${A2_WEIGHTS}" "${TTS_WEIGHTS}")
    if(NOT EXISTS "${file}")
      message(FATAL_ERROR "model file not found: ${file} (scripts/fetch_model.py, scripts/fetch_koeact.py)")
    endif()
  endforeach()
  esptool_py_flash_to_partition(flash "model" "${RALOMI_ESPDL}")
  esptool_py_flash_to_partition(flash "koeact" "${KOEACT_ESPDL}")
  esptool_py_flash_to_partition(flash "a2" "${A2_WEIGHTS}")
  if(BOTWATCH_PET_COUNT GREATER 0)
    esptool_py_flash_to_partition(flash "sprites" "${CMAKE_CURRENT_LIST_DIR}/../assets/pets/sprites.bin")
  endif()
endif()

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  esptool_py_flash_to_partition(flash "tts" "${TTS_WEIGHTS}")
endif()

option(BOTWATCH_USE_LOCAL_PRESETS "Use the ignored presets_local.h when present" ON)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  set(preset_args)
  if(NOT BOTWATCH_USE_LOCAL_PRESETS)
    list(APPEND preset_args --sample)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOTWATCH_SAMPLE_PRESETS)
  endif()
  add_custom_target(botwatch_print_font
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../scripts/build_print_font.py" ${preset_args}
    BYPRODUCTS "${CMAKE_CURRENT_LIST_DIR}/print_font.inc" "${CMAKE_CURRENT_LIST_DIR}/../assets/print-font.json"
    VERBATIM)
  add_dependencies(${COMPONENT_LIB} botwatch_print_font)
endif()

# A measurement-only build links the real ESP-NOW transport and G.711 paths.
# The probe source lives in ignored evidence, never in the normal firmware.
if(BOTWATCH_RADIO_PROBE_DIR)
  target_sources(${COMPONENT_LIB} PRIVATE "${BOTWATCH_RADIO_PROBE_DIR}/radio_probe.cpp"
    "${BOTWATCH_RADIO_PROBE_DIR}/transport/esp_now_radio.cpp")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE BOTWATCH_RADIO_MEASURE)
  idf_component_get_property(wifi_lib esp_wifi COMPONENT_LIB)
  idf_component_get_property(netif_lib esp_netif COMPONENT_LIB)
  target_link_libraries(${COMPONENT_LIB} PRIVATE ${wifi_lib} ${netif_lib})
endif()
