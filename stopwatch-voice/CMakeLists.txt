# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 ayousanz
#
# Ralomi 990k on M5Stack CoreS3 (ESP32-S3, 16MB flash, 8MB quad PSRAM) with ESP-DL.
# The quantized model is not in Git: pass -DRALOMI_ESPDL=<path to .espdl>. The default is the
# 100-frame W16A16 chunk model, next to this folder in the Hugging Face esp32s3/ layout or
# written by scripts/device/stream_espdl.py in this repository.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED RALOMI_ESPDL)
  set(RALOMI_ESPDL "${CMAKE_CURRENT_LIST_DIR}/models/ralomi_w16a16_stream100.espdl")
endif()
# Action mode: KoeAct-JP v0.1.1 (scripts/fetch_koeact.py).
if(NOT DEFINED KOEACT_ESPDL)
  set(KOEACT_ESPDL "${CMAKE_CURRENT_LIST_DIR}/models/ralomi_e13_stream40.espdl")
endif()
if(NOT DEFINED A2_WEIGHTS)
  set(A2_WEIGHTS "${CMAKE_CURRENT_LIST_DIR}/models/a2_weights.bin")
endif()

set(TTS_WEIGHTS "${CMAKE_CURRENT_LIST_DIR}/models/saanotts-jp-v4-int8.bin")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# M5Unified 0.2.17 (I2C_Class.hpp) trips GCC 14's maybe-uninitialized under -Werror.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)
project(stopwatch_voice)
