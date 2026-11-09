#include "ruby_ble.h"
#include <cstring>
#include <cstdio>
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace {
// UUID bytes are in BLE's little-endian order.
const ble_uuid128_t serviceUuid = BLE_UUID128_INIT(0x48,0xe5,0x22,0xf0,0x7a,0x62,0x4a,0xb5,0xd0,0x48,0x5d,0x9b,0x01,0x00,0xe4,0x47);
const ble_uuid128_t writeUuid = BLE_UUID128_INIT(0x48,0xe5,0x22,0xf0,0x7a,0x62,0x4a,0xb5,0xd0,0x48,0x5d,0x9b,0x02,0x00,0xe4,0x47);
const ble_uuid128_t notifyUuid = BLE_UUID128_INIT(0x48,0xe5,0x22,0xf0,0x7a,0x62,0x4a,0xb5,0xd0,0x48,0x5d,0x9b,0x03,0x00,0xe4,0x47);
uint16_t connection = BLE_HS_CONN_HANDLE_NONE, notifyHandle = 0;
uint8_t addressType = 0;
uint8_t* receiving = nullptr;
uint32_t total = 0, received = 0, expectedCrc = 0;
bool subscribed = false, stopping = false;
SemaphoreHandle_t notifyLock = nullptr;

void clearReceive() { free(receiving); receiving = nullptr; total = received = expectedCrc = 0; }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void advertise();
int gapEvent(ble_gap_event* ev, void*) {
    if (ev->type == BLE_GAP_EVENT_CONNECT) {
        if (!ev->connect.status) connection = ev->connect.conn_handle;
        else if (!stopping) advertise();
    } else if (ev->type == BLE_GAP_EVENT_DISCONNECT) {
        connection = BLE_HS_CONN_HANDLE_NONE; subscribed = false; clearReceive();
        if (!stopping) advertise();
    } else if (ev->type == BLE_GAP_EVENT_SUBSCRIBE && ev->subscribe.attr_handle == notifyHandle) {
        subscribed = ev->subscribe.cur_notify;
    }
    return 0;
}
void advertise() {
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = const_cast<ble_uuid128_t*>(&serviceUuid);
    fields.num_uuids128 = 1; fields.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields)) return;
    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND; params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(addressType, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
}
void onSync() {
    if (ble_hs_util_ensure_addr(0) || ble_hs_id_infer_auto(0, &addressType)) return;
    advertise();
}
int writeAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctx, void*) {
    const size_t size = OS_MBUF_PKTLEN(ctx->om);
    static uint8_t packet[501];
    if (!size || size > sizeof(packet) || ble_hs_mbuf_to_flat(ctx->om, packet, size, nullptr)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (packet[0] == 1 && size == 9) {
        clearReceive();
        total = u32(packet + 1); expectedCrc = u32(packet + 5);
        if (!total || total > heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
            clearReceive(); rubyBleNotify("error memory"); return 0;
        }
        receiving = static_cast<uint8_t*>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!receiving) { clearReceive(); rubyBleNotify("error memory"); return 0; }
        rubyBleNotify("ready"); return 0;
    }
    if (packet[0] == 2 && receiving && size > 1 && size - 1 <= total - received) {
        memcpy(receiving + received, packet + 1, size - 1); received += size - 1;
        char message[32]; snprintf(message, sizeof(message), "recv %lu", static_cast<unsigned long>(received));
        rubyBleNotify(message); return 0;
    }
    if (packet[0] == 3 && size == 1 && receiving) {
        if (received != total || esp_rom_crc32_le(0, receiving, total) != expectedCrc) {
            clearReceive(); rubyBleNotify("error crc or size"); return 0;
        }
        if (total < 20 || memcmp(receiving, "RITE0400", 8) ||
            (uint32_t(receiving[8]) << 24 | uint32_t(receiving[9]) << 16 | uint32_t(receiving[10]) << 8 | uint32_t(receiving[11])) != total) {
            clearReceive(); rubyBleNotify("error bytecode"); return 0;
        }
        if (!rubySubmit(receiving, total)) { clearReceive(); rubyBleNotify("error busy"); return 0; }
        receiving = nullptr; total = received = expectedCrc = 0; return 0;
    }
    clearReceive(); rubyBleNotify("error protocol"); return 0;
}
int notifyAccess(uint16_t, uint16_t, ble_gatt_access_ctxt*, void*) { return BLE_ATT_ERR_UNLIKELY; }
ble_gatt_chr_def characteristics[3]{};
ble_gatt_svc_def services[2]{};
void hostTask(void*) { nimble_port_run(); nimble_port_freertos_deinit(); }
}

namespace {
void sendNotification(uint8_t kind, const uint8_t* bytes, size_t length) {
    if (!notifyLock) return;
    xSemaphoreTake(notifyLock, portMAX_DELAY);
    if (!subscribed || connection == BLE_HS_CONN_HANDLE_NONE) { xSemaphoreGive(notifyLock); return; }
    size_t maxPayload = ble_att_mtu(connection) - 3;
    if (maxPayload <= 2) { xSemaphoreGive(notifyLock); return; }
    if (maxPayload > 512) maxPayload = 512;
    do {
        size_t n = length < maxPayload - 2 ? length : maxPayload - 2;
        static uint8_t packet[512];
        packet[0] = kind;
        packet[1] = n == length ? 1 : 0;
        memcpy(packet + 2, bytes, n);
        os_mbuf* data = ble_hs_mbuf_from_flat(packet, n + 2);
        if (!data || ble_gatts_notify_custom(connection, notifyHandle, data)) { xSemaphoreGive(notifyLock); return; }
        bytes += n; length -= n;
    } while (length);
    xSemaphoreGive(notifyLock);
}
}
void rubyBleNotify(const char* text) {
    sendNotification(1, reinterpret_cast<const uint8_t*>(text), strlen(text));
}
void rubyBleLog(const void* data, size_t size) {
    sendNotification(2, static_cast<const uint8_t*>(data), size);
}

bool rubyBleStart() {
    stopping = false; connection = BLE_HS_CONN_HANDLE_NONE; subscribed = false;
    notifyLock = xSemaphoreCreateMutex();
    if (!notifyLock) return false;
    if (nimble_port_init() != ESP_OK) { vSemaphoreDelete(notifyLock); notifyLock = nullptr; return false; }
    ble_svc_gap_init(); ble_svc_gatt_init();
    characteristics[0].uuid = &writeUuid.u; characteristics[0].access_cb = writeAccess;
    characteristics[0].flags = BLE_GATT_CHR_F_WRITE;
    characteristics[1].uuid = &notifyUuid.u; characteristics[1].access_cb = notifyAccess;
    characteristics[1].val_handle = &notifyHandle;
    characteristics[1].flags = BLE_GATT_CHR_F_NOTIFY;
    services[0].type = BLE_GATT_SVC_TYPE_PRIMARY; services[0].uuid = &serviceUuid.u;
    services[0].characteristics = characteristics;
    if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) { nimble_port_deinit(); vSemaphoreDelete(notifyLock); notifyLock = nullptr; return false; }
    ble_svc_gap_device_name_set("FACES Ruby");
    ble_hs_cfg.sync_cb = onSync;
    nimble_port_freertos_init(hostTask); return true;
}
void rubyBleStop() {
    stopping = true;
    nimble_port_stop(); nimble_port_deinit();
    clearReceive(); connection = BLE_HS_CONN_HANDLE_NONE; subscribed = false;
    if (notifyLock) { vSemaphoreDelete(notifyLock); notifyLock = nullptr; }
}
