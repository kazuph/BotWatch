#include "photo_storage.h"
#include <M5Unified.h>
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_vfs_fat.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace photo_storage {
namespace {
constexpr char kMount[] = "/sdcard";
constexpr char kPhotoDir[] = "/sdcard/DCIM/FACES";
sdmmc_card_t* card = nullptr;
class SdBusGuard {
public:
    SdBusGuard() { M5.Display.waitDMA(); gpio_set_direction(GPIO_NUM_35, GPIO_MODE_INPUT); }
    ~SdBusGuard() { gpio_set_direction(GPIO_NUM_35, GPIO_MODE_OUTPUT); }
};

bool fail(char* error, size_t size, const char* operation, int code) {
    const char* message = "SD の操作に失敗";
    if (code == ENOSPC) message = "SD の容量か番号が不足";
    else if (!strcmp(operation, "SD mount")) message = "SD を使えません";
    else if (!strcmp(operation, "no photo")) message = "写真がありません";
    else if (!strcmp(operation, "JPEG memory") || !strcmp(operation, "memory")) message = "メモリ不足";
    else if (!strncmp(operation, "JPEG", 4)) message = "JPEG 変換に失敗";
    else if (!strcmp(operation, "image")) message = "画像データが不正";
    snprintf(error, size, "%s", message);
    printf("CAMERA %s error=%d\n", operation, code);
    return false;
}

bool make_dir(const char* path, char* error, size_t size) {
    if (mkdir(path, 0775) == 0 || errno == EEXIST) return true;
    return fail(error, size, "mkdir", errno);
}
bool max_photo_number(int* result, char* error, size_t error_size, unsigned* count = nullptr) {
    DIR* dir = opendir(kPhotoDir);
    if (!dir) {
        if (errno == ENOENT) { *result = -1; if (count) *count = 0; return true; }
        return fail(error, error_size, "opendir", errno);
    }
    int max_number = -1;
    unsigned total = 0;
    int read_error = 0;
    for (;;) {
        errno = 0;
        dirent* entry = readdir(dir);
        if (!entry) { read_error = errno; break; }
        const char* name = entry->d_name;
        if (strlen(name) != 12 || memcmp(name, "IMG_", 4) != 0 || strcmp(name + 8, ".JPG") != 0)
            continue;
        if (!std::all_of(name + 4, name + 8, [](unsigned char c) { return std::isdigit(c); }))
            continue;
        const int number = (name[4] - '0') * 1000 + (name[5] - '0') * 100 +
                                (name[6] - '0') * 10 + (name[7] - '0');
        ++total;
        max_number = std::max(max_number, number);
    }
    const int close_result = closedir(dir);
    if (read_error) return fail(error, error_size, "readdir", read_error);
    if (close_result != 0) return fail(error, error_size, "closedir", errno);
    *result = max_number;
    if (count) *count = total;
    return true;
}

}

static bool mount_sd(char* error, size_t error_size) {
    if (card) return true;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = GPIO_NUM_4;
    slot.host_id = SPI2_HOST;
    esp_vfs_fat_sdmmc_mount_config_t config = {};
    config.format_if_mount_failed = false;
    config.max_files = 1;
    const esp_err_t result = esp_vfs_fat_sdspi_mount(kMount, &host, &slot, &config, &card);
    if (result != ESP_OK) return fail(error, error_size, "SD mount", result);
    return true;
}

bool mount(char* error, size_t error_size) {
    SdBusGuard guard;
    return mount_sd(error, error_size);
}

bool count_photos(unsigned* count, char* error, size_t error_size) {
    if (!count) return false;
    SdBusGuard guard;
    if (!mount_sd(error, error_size)) return false;
    int latest = -1;
    return max_photo_number(&latest, error, error_size, count);
}

static bool find_latest(char* path, size_t path_size, char* error, size_t error_size) {
    if (!mount_sd(error, error_size)) return false;
    int number = -1;
    if (!max_photo_number(&number, error, error_size)) return false;
    if (number < 0) return fail(error, error_size, "no photo", ENOENT);
    if (snprintf(path, path_size, "%s/IMG_%04d.JPG", kPhotoDir, number) >= int(path_size))
        return fail(error, error_size, "path", ENAMETOOLONG);
    return true;
}

bool latest_path(char* path, size_t path_size, char* error, size_t error_size) {
    SdBusGuard guard;
    return find_latest(path, path_size, error, error_size);
}

bool send_latest(char* error, size_t error_size) {
    SdBusGuard guard;
    char path[64];
    if (!find_latest(path, sizeof(path), error, error_size)) return false;
    FILE* file = fopen(path, "rb");
    if (!file) return fail(error, error_size, "open", errno);
    struct stat info = {};
    if (fstat(fileno(file), &info) != 0 || info.st_size <= 0) {
        int code = errno ? errno : EIO;
        fclose(file);
        return fail(error, error_size, "stat", code);
    }
    printf("JPEG %ld\n", long(info.st_size));
    uint8_t buffer[1024];
    size_t left = size_t(info.st_size);
    while (left) {
        const size_t count = fread(buffer, 1, std::min(left, sizeof(buffer)), file);
        if (!count) { fclose(file); return fail(error, error_size, "read", EIO); }
        if (fwrite(buffer, 1, count, stdout) != count) {
            fclose(file); return fail(error, error_size, "serial write", EIO);
        }
        left -= count;
    }
    if (fclose(file) != 0) return fail(error, error_size, "close", errno);
    fflush(stdout);
    return true;
}

bool read_latest(uint8_t** data, size_t* size, char* error, size_t error_size) {
    if (!data || !size) return fail(error, error_size, "buffer", EINVAL);
    *data = nullptr;
    *size = 0;
    SdBusGuard guard;
    char path[64];
    if (!find_latest(path, sizeof(path), error, error_size)) return false;
    FILE* file = fopen(path, "rb");
    if (!file) return fail(error, error_size, "open", errno);
    struct stat info = {};
    if (fstat(fileno(file), &info) != 0 || info.st_size <= 0) {
        int code = errno ? errno : EIO;
        fclose(file);
        return fail(error, error_size, "stat", code);
    }
    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(info.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) { fclose(file); return fail(error, error_size, "memory", ENOMEM); }
    const size_t count = fread(buffer, 1, info.st_size, file);
    int close_result = fclose(file);
    if (count != size_t(info.st_size) || close_result != 0) {
        free(buffer);
        return fail(error, error_size, "read/close", EIO);
    }
    *data = buffer;
    *size = count;
    return true;
}

bool save_rgb565(const uint8_t* pixels, size_t size, int width, int height,
                 char* path, size_t path_size, char* error, size_t error_size) {
    SdBusGuard guard;
    if (!pixels || width <= 0 || height <= 0 || size != size_t(width) * height * 2)
        return fail(error, error_size, "image", EINVAL);
    if (!mount_sd(error, error_size) || !make_dir("/sdcard/DCIM", error, error_size) ||
        !make_dir(kPhotoDir, error, error_size)) return false;
    int max_number = -1;
    if (!max_photo_number(&max_number, error, error_size)) return false;
    if (max_number >= 9999) return fail(error, error_size, "photo number full", ENOSPC);
    const int next_number = max_number < 0 ? 1 : max_number + 1;
    if (snprintf(path, path_size, "%s/IMG_%04d.JPG", kPhotoDir, next_number) >= int(path_size))
        return fail(error, error_size, "path", ENAMETOOLONG);

    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = width;
    config.height = height;
    config.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    jpeg_enc_handle_t encoder = nullptr;
    jpeg_error_t status = jpeg_enc_open(&config, &encoder);
    if (status != JPEG_ERR_OK) return fail(error, error_size, "JPEG open", status);
    auto* input = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* output = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!input || !output) {
        free(input); free(output); jpeg_enc_close(encoder);
        return fail(error, error_size, "JPEG memory", ENOMEM);
    }
    memcpy(input, pixels, size);
    int output_size = 0;
    status = jpeg_enc_process(encoder, input, size, output, size, &output_size);
    free(input);
    jpeg_enc_close(encoder);
    if (status != JPEG_ERR_OK || output_size <= 0) {
        free(output);
        return fail(error, error_size, "JPEG encode", status);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0664);
    if (fd < 0) { free(output); return fail(error, error_size, "open", errno); }
    size_t written = 0;
    int write_error = 0;
    while (written < size_t(output_size)) {
        ssize_t n = write(fd, output + written, output_size - written);
        if (n <= 0) { write_error = n < 0 ? errno : EIO; break; }
        written += size_t(n);
    }
    free(output);
    if (!write_error && fsync(fd) != 0) write_error = errno;
    if (close(fd) != 0 && !write_error) write_error = errno;
    if (write_error) {
        unlink(path);
        return fail(error, error_size, "write/close", write_error);
    }
    return true;
}
}
