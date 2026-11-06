#pragma once
#include <cstddef>
#include <cstdint>

// All operations run on the UI task after the display's transfers have finished.
namespace photo_storage {
bool mount(char* error, size_t error_size);
bool count_photos(unsigned* count, char* error, size_t error_size);
bool read_latest(uint8_t** data, size_t* size, char* error, size_t error_size);
bool send_latest(char* error, size_t error_size);
bool latest_path(char* path, size_t path_size, char* error, size_t error_size);
bool save_rgb565(const uint8_t* pixels, size_t size, int width, int height,
                 char* path, size_t path_size, char* error, size_t error_size);
}
