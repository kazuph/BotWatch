#include "network_serial.h"
#include "network_settings.h"
#include "esp_err.h"
#include <cstdio>
#include <cstring>

bool network_serial_command(char* line) {
    if (!line || (line[0] != 'W' && line[0] != 'K' && line[0] != 'X')) return false;
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (line[0] == 'W') {
        char* separator = strchr(line + 1, '\t');
        if (separator) {
            *separator = '\0';
            result = network_settings::set_wifi(line + 1, separator + 1);
        }
    } else if (line[0] == 'K') {
        result = network_settings::set_auth_key(line + 1);
    } else if (line[0] == 'X') {
        if (!line[1]) result = network_settings::clear();
    }
    if (result == ESP_OK) puts(line[0] == 'X' ? "消去した" : "保存した");
    else puts("保存できません");
    return true;
}
