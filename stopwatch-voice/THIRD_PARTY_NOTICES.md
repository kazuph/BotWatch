# Third-party notices

`ralomi_cores3_w16a16_stream100.bin` contains the Ralomi firmware and model (Apache-2.0,
Copyright 2026 ayousanz) linked with the software below. The list follows the archives in the
firmware's link map (ESP-IDF v5.5.5, GCC 14.2.0 esp-14.2.0_20260121). Full license texts are in
[`licenses/`](licenses/).

| Software | Version | License | Copyright | Text |
|---|---|---|---|---|
| ESP-IDF (bootloader, FreeRTOS port, drivers, heap, newlib glue, SPI flash, NVS, etc.) | v5.5.5 | Apache-2.0 | Espressif Systems (Shanghai) Co., Ltd. | [Apache-2.0.txt](licenses/Apache-2.0.txt) |
| FreeRTOS Kernel (in ESP-IDF) | ESP-IDF v5.5.5 | MIT | Amazon.com, Inc. or its affiliates | [FreeRTOS-Kernel_MIT.txt](licenses/FreeRTOS-Kernel_MIT.txt) |
| Xtensa HAL / runtime (in ESP-IDF) | ESP-IDF v5.5.5 | MIT | Cadence Design Systems, Inc. | [Xtensa-HAL_MIT.txt](licenses/Xtensa-HAL_MIT.txt) |
| TLSF memory allocator (in ESP-IDF) | ESP-IDF v5.5.5 | BSD-3-Clause | Matthew Conte | [TLSF_BSD-3-Clause.txt](licenses/TLSF_BSD-3-Clause.txt) |
| Mbed TLS (libmbedcrypto, in ESP-IDF) | ESP-IDF v5.5.5 | Apache-2.0 (chosen from Apache-2.0 OR GPL-2.0-or-later) | The Mbed TLS Contributors | [Mbed-TLS.txt](licenses/Mbed-TLS.txt) |
| newlib C/math library | esp-14.2.0_20260121 | newlib licenses (BSD-style and others) | see the file | [newlib_COPYING.NEWLIB.txt](licenses/newlib_COPYING.NEWLIB.txt) |
| libgcc, libstdc++ | GCC 14.2.0 | GPL-3.0 with GCC Runtime Library Exception | Free Software Foundation, Inc. | no notice required for compiled programs under the exception |
| ESP-DL (including the prebuilt `libfbs_model` model loader) | 3.3.13 | MIT | Espressif Systems (Shanghai) Co., Ltd. | [ESP-DL_MIT.txt](licenses/ESP-DL_MIT.txt) |
| FlatBuffers runtime (used by the ESP-DL model loader) | | Apache-2.0 | Google Inc. | [Apache-2.0.txt](licenses/Apache-2.0.txt) |
| ESP-DSP | 1.8.2 | Apache-2.0 | Espressif Systems (Shanghai) Co., Ltd. | [ESP-DSP_Apache-2.0.txt](licenses/ESP-DSP_Apache-2.0.txt) |
| M5Unified | 0.2.17 | MIT | M5Stack | [M5Unified_MIT.txt](licenses/M5Unified_MIT.txt) |
| M5GFX | 0.2.23 | MIT | M5Stack | [M5GFX_MIT.txt](licenses/M5GFX_MIT.txt) |
| LovyanGFX (M5GFX is based on it), with code from Adafruit_GFX, Adafruit_ILI9341 and TFT_eSPI | bundled in M5GFX 0.2.23 | FreeBSD / BSD / MIT | lovyan03; Adafruit Industries; Bodmer | [LovyanGFX_and_bundled.txt](licenses/LovyanGFX_and_bundled.txt) |
| efont Japanese 16px font (`efontJA_16`, bundled in M5GFX) | bundled in M5GFX 0.2.23 | BSD-3-Clause | /efont/ The Electronic Font Open Laboratory | [efont_BSD-3-Clause.txt](licenses/efont_BSD-3-Clause.txt) |

The built-in 6x8 GLCD font (`Font0`) comes from Adafruit_GFX and is covered by the Adafruit
notice in `LovyanGFX_and_bundled.txt`. The firmware uses no other M5GFX fonts.

The firmware source in [`firmware/`](firmware/) downloads these components from the ESP
Component Registry at build time (`main/idf_component.yml`, `dependencies.lock`); they are not
copied into this repository.

PTT 無線の `main/radio/`（接続手順、ESP-NOW、G.711）は M5Stack M5StopWatch-UserDemo の MIT ソースを移植しています。参照版は kazuph/M5StopWatch の `d9ecb3e461187cf362b3d4c5aa99cbf168bf7021`、`main/apps/app_transceiver/` です。各ファイルの著作権表示を保持し、会話モードは PTT のみに限定しています。
