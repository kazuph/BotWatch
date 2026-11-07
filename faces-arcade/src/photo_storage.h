#pragma once
#include <cstddef>
#include <cstdint>

// All operations run on the UI task after the display's transfers have finished.
namespace photo_storage {
bool mount(char* error, size_t error_size);
bool count_photos(unsigned* count, char* error, size_t error_size);
bool read_latest(uint8_t** data, size_t* size, char* error, size_t error_size);
bool read_index(unsigned index, unsigned* total, char* name, size_t name_size,
                uint8_t** data, size_t* size, char* error, size_t error_size);
// Deletes /DCIM/FACES/<name>.JPG, where name is like "IMG_0003" from read_index.
bool delete_photo(const char* name, char* error, size_t error_size);
bool send_latest(char* error, size_t error_size);
bool latest_path(char* path, size_t path_size, char* error, size_t error_size);
bool save_rgb565(const uint8_t* pixels, size_t size, int width, int height,
                 char* path, size_t path_size, char* error, size_t error_size);
}
