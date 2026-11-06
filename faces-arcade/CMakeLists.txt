cmake_minimum_required(VERSION 3.16)
if(IDF_TARGET STREQUAL "esp32s3")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/scripts/prepare_microlink.py"
                    RESULT_VARIABLE microlink_status)
    if(NOT microlink_status EQUAL 0)
        message(FATAL_ERROR "Could not prepare pinned MicroLink")
    endif()
    set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/.dependencies/microlink/components/microlink"
                             "${CMAKE_CURRENT_LIST_DIR}/.dependencies/microlink/components/wireguard_lwip")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)
project(faces_arcade)
