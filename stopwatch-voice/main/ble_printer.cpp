#include "ble_printer.h"

#include <cstdio>
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <algorithm>
#include "esp_bt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

namespace ble_printer {
namespace {
StaticSemaphore_t sync_storage;
SemaphoreHandle_t synced;

void on_sync() { xSemaphoreGive(synced); }

void host_task(void *)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}
}

static bool start(MemoryLog memory)
{
    const esp_err_t nvs = nvs_flash_init();
    if (nvs != ESP_OK) {
        printf("BLE_ERROR nvs_init=%s\n", esp_err_to_name(nvs));
        return false;
    }
    synced = xSemaphoreCreateBinaryStatic(&sync_storage);
    memory("ble_before_init", false);
    const esp_err_t initialized = nimble_port_init();
    printf("BLE_INIT result=%s external=%d controller=%d\n", esp_err_to_name(initialized),
           CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL, (int)esp_bt_controller_get_status());
    memory("ble_after_init", false);
    if (initialized != ESP_OK) { return false; }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    xSemaphoreTake(synced, portMAX_DELAY);
    memory("ble_started", false);
    return true;
}

static bool stop(MemoryLog memory)
{
    const int stopped = nimble_port_stop();
    printf("BLE_STOP result=%d\n", stopped);
    if (stopped != 0) { return false; }
    const esp_err_t deinitialized = nimble_port_deinit();
    printf("BLE_DEINIT result=%s controller=%d\n", esp_err_to_name(deinitialized),
           (int)esp_bt_controller_get_status());
    memory("ble_stopped", false);
    return deinitialized == ESP_OK && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE;
}

bool probe(MemoryLog memory) { return start(memory) && stop(memory); }

namespace {
// Deadlines come from the existing printer sample and IDF blecent example.
constexpr int kScanMs = 8000;
constexpr int kConnectMs = 30000;
constexpr int kReplyMs = 15000;
constexpr int kCreditMs = 2000;  // docs/PROTOCOL.md
constexpr int kPacketDelayMs = 30;
constexpr int kFeedSettleMs = 3000;
constexpr char kPrinterName[] = "GHLP";  // Same discovered-name fragment as the proven Arduino sample.
constexpr uint8_t kPrinterManufacturer[] = {0x41, 0x59, 0x5e, 0x55, 0x09, 0x23, 0x95, 0xfd};
const ble_uuid16_t service_uuid = BLE_UUID16_INIT(0xff00);
struct Characteristic { uint16_t value = 0, end = 0, cccd = 0; } rx, tx, credit;
uint16_t service_start = 0, service_end = 0;
StaticSemaphore_t ready_storage, reply_storage, credit_storage, gone_storage;
SemaphoreHandle_t ready, reply, credited, gone;
std::atomic<int> failure{0}, credits{0};
std::atomic<uint16_t> connection{BLE_HS_CONN_HANDLE_NONE};
std::atomic<bool> connecting{false}, closing{false}, pending_status{false};
std::atomic<bool> pending_completion{false};
std::atomic<bool> pending_battery{false};
std::atomic<const char *> failure_operation{""};
int battery_percent = -1;
bool (*cancel_control)() = nullptr;
uint8_t status_code = 0;
uint8_t own_address_type = 0;
void fail(const char *operation, int code);

int mtu_exchanged(uint16_t, const ble_gatt_error *error, uint16_t mtu, void *)
{
    printf("BLE_MTU status=%d mtu=%u\n", error->status, mtu);
    if (error->status != 0 || mtu < 90 + 3) { fail("unsupported_print_mtu", error->status ? error->status : BLE_HS_ENOTSUP); }
    xSemaphoreGive(synced);
    return 0;
}

void fail(const char *operation, int code)
{
    int expected = 0;
    if (failure.compare_exchange_strong(expected, code ? code : BLE_HS_EAPP)) { failure_operation = operation; }
    printf("BLE_ERROR operation=%s code=%d\n", operation, code);
    xSemaphoreGive(ready);
    xSemaphoreGive(reply);
}

void checked(const char *operation, int code) { if (code != 0) { fail(operation, code); } }

bool wait_reply(SemaphoreHandle_t semaphore, int milliseconds)
{
    if (!cancel_control) { return xSemaphoreTake(semaphore, pdMS_TO_TICKS(milliseconds)) == pdTRUE; }
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(milliseconds);
    do {
        if (cancel_control()) { fail("cancelled_before_send", BLE_HS_EAPP); return false; }
        if (xSemaphoreTake(semaphore, pdMS_TO_TICKS(20)) == pdTRUE) { return true; }
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
    return false;
}

int subscribed(uint16_t handle, const ble_gatt_error *error, ble_gatt_attr *, void *arg)
{
    if (error->status != 0) { fail("subscribe", error->status); return 0; }
    if (arg == &credit) {
        const uint8_t enabled[] = {1, 0};
        checked("subscribe_rx", ble_gattc_write_flat(handle, rx.cccd, enabled, sizeof(enabled), subscribed, &rx));
    } else { xSemaphoreGive(ready); }
    return 0;
}

int descriptors(uint16_t handle, const ble_gatt_error *error, uint16_t, const ble_gatt_dsc *descriptor, void *arg)
{
    auto *target = static_cast<Characteristic *>(arg);
    if (error->status == 0) {
        if (ble_uuid_u16(&descriptor->uuid.u) == 0x2902) { target->cccd = descriptor->handle; }
    } else if (error->status == BLE_HS_EDONE) {
        if (!target->cccd) { fail("missing_cccd", BLE_HS_ENOENT); }
        else if (target == &credit) {
            checked("discover_rx_cccd", ble_gattc_disc_all_dscs(handle, rx.value, rx.end, descriptors, &rx));
        } else {
            const uint8_t enabled[] = {1, 0};
            checked("subscribe_credit", ble_gattc_write_flat(handle, credit.cccd, enabled, sizeof(enabled), subscribed, &credit));
        }
    } else { fail("discover_cccd", error->status); }
    return 0;
}

int characteristics(uint16_t handle, const ble_gatt_error *error, const ble_gatt_chr *characteristic, void *)
{
    if (error->status == 0) {
        for (auto *item : {&rx, &tx, &credit}) {
            if (item->value && !item->end) { item->end = characteristic->def_handle - 1; }
        }
        const uint16_t uuid = ble_uuid_u16(&characteristic->uuid.u);
        if (uuid == 0xff01) { rx.value = characteristic->val_handle; }
        if (uuid == 0xff02) {
            tx.value = characteristic->val_handle;
            if (!(characteristic->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP)) { fail("unsupported_tx_properties", BLE_HS_ENOTSUP); }
        }
        if (uuid == 0xff03) { credit.value = characteristic->val_handle; }
    } else if (error->status == BLE_HS_EDONE) {
        for (auto *item : {&rx, &tx, &credit}) { if (!item->end) { item->end = service_end; } }
        if (!rx.value || !tx.value || !credit.value) { fail("missing_characteristic", BLE_HS_ENOENT); }
        else { checked("discover_credit_cccd", ble_gattc_disc_all_dscs(handle, credit.value, credit.end, descriptors, &credit)); }
    } else { fail("discover_characteristics", error->status); }
    return 0;
}

int services(uint16_t handle, const ble_gatt_error *error, const ble_gatt_svc *service, void *)
{
    if (error->status == 0) { service_start = service->start_handle; service_end = service->end_handle; }
    else if (error->status == BLE_HS_EDONE) {
        if (!service_start) { fail("missing_service", BLE_HS_ENOENT); }
        else { checked("discover_characteristics", ble_gattc_disc_all_chrs(handle, service_start, service_end, characteristics, nullptr)); }
    } else { fail("discover_service", error->status); }
    return 0;
}

int gap(ble_gap_event *event, void *)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (connecting.load()) { break; }
        ble_hs_adv_fields fields{};
        const int parsed = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
        printf("BLE_ADVERTISEMENT parsed=%d type=%d rssi=%d name_bytes=%u name=", parsed,
               event->disc.event_type, event->disc.rssi, (unsigned)fields.name_len);
        if (parsed == 0 && fields.name) {
            for (int i = 0; i < fields.name_len; ++i) { printf("%02x", fields.name[i]); }
        }
        printf("\n");
        const bool named = parsed == 0 && fields.name &&
            std::search(fields.name, fields.name + fields.name_len, kPrinterName,
                        kPrinterName + sizeof(kPrinterName) - 1) != fields.name + fields.name_len;
        bool service = false;
        if (parsed == 0) {
            for (int i = 0; i < fields.num_uuids16; ++i) { service |= fields.uuids16[i].value == 0x0a71; }
        }
        const bool manufacturer = parsed == 0 && fields.mfg_data &&
            fields.mfg_data_len == sizeof(kPrinterManufacturer) &&
            std::memcmp(fields.mfg_data, kPrinterManufacturer, sizeof(kPrinterManufacturer)) == 0;
        if (named || service || manufacturer) {
            printf("BLE_TARGET name=%d service=%d manufacturer=%d\n", named, service, manufacturer);
            connecting = true;
            checked("scan_cancel", ble_gap_disc_cancel());
            checked("connect", ble_gap_connect(own_address_type, &event->disc.addr, kConnectMs, nullptr, gap, nullptr));
        }
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!connecting.load() && !closing.load()) { fail("printer_not_found", BLE_HS_ENOENT); }
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) { fail("connect_complete", event->connect.status); }
        else {
            connection = event->connect.conn_handle;
            checked("discover_service", ble_gattc_disc_svc_by_uuid(connection.load(), &service_uuid.u, services, nullptr));
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connection = BLE_HS_CONN_HANDLE_NONE;
        xSemaphoreGive(gone);
        if (!closing.load()) { fail("disconnected", event->disconnect.reason); }
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        const int length = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t bytes[3]{};
        if (event->notify_rx.attr_handle == credit.value && length <= (int)sizeof(bytes)) {
            os_mbuf_copydata(event->notify_rx.om, 0, length, bytes);
            if (length == 2 && bytes[0] == 1) { credits.fetch_add(bytes[1]); xSemaphoreGive(credited); }
        } else if (event->notify_rx.attr_handle == rx.value) {
            if (pending_battery.load()) {
                uint8_t battery[5]{};
                if (length != sizeof(battery) || os_mbuf_copydata(event->notify_rx.om, 0, length, battery) != 0 || battery[4] > 100) {
                    fail("unsupported_battery_reply", BLE_HS_ENOTSUP);
                } else { battery_percent = battery[4]; printf("BLE_BATTERY percent=%d\n", battery_percent); }
                pending_battery = false; xSemaphoreGive(reply); break;
            }
            if (!pending_status.load() && !pending_completion.load()) { printf("BLE_UNSOLICITED bytes=%d\n", length); break; }
            if (length != 1) { fail("unsupported_status_shape", BLE_HS_ENOTSUP); break; }
            os_mbuf_copydata(event->notify_rx.om, 0, 1, &status_code);
            if (pending_completion.load()) {
                printf("BLE_COMPLETION code=%02x\n", status_code);
                pending_completion = false;
                if (status_code != 0xaa) { fail("unknown_print_completion", BLE_HS_ENOTSUP); }
            } else { pending_status = false; }
            xSemaphoreGive(reply);
        }
        break;
    }
    default: break;
    }
    return 0;
}
}

static PrintResult transact(MemoryLog memory, const uint8_t *job, size_t bytes,
                            bool (*cancelled)(), void (*started)())
{
    ready = xSemaphoreCreateBinaryStatic(&ready_storage);
    reply = xSemaphoreCreateBinaryStatic(&reply_storage);
    credited = xSemaphoreCreateBinaryStatic(&credit_storage);
    gone = xSemaphoreCreateBinaryStatic(&gone_storage);
    rx = {}; tx = {}; credit = {};
    service_start = service_end = 0;
    failure = 0; credits = 0; connection = BLE_HS_CONN_HANDLE_NONE;
    connecting = false; closing = false; pending_status = false; pending_completion = false; pending_battery = false;
    failure_operation = ""; battery_percent = -1; status_code = 0;
    cancel_control = cancelled;
    if (!start(memory)) { return {false, 0, "ble_init", -1, -1}; }
    checked("address", ble_hs_util_ensure_addr(0));
    checked("own_address", ble_hs_id_infer_auto(0, &own_address_type));
    if (!failure.load()) {
        ble_gap_disc_params parameters{};
        parameters.passive = 0;
        parameters.filter_duplicates = 1;
        checked("scan", ble_gap_disc(own_address_type, kScanMs, &parameters, gap, nullptr));
        if (!wait_reply(ready, kScanMs + kConnectMs + kReplyMs)) { fail("connect_timeout", BLE_HS_ETIMEOUT); }
    }
    if (!failure.load()) {
        memory("ble_connected", false);
        if (credits.load() == 0 && !wait_reply(credited, kCreditMs)) { fail("credit_timeout", BLE_HS_ETIMEOUT); }
        if (credits.load() <= 0) { fail("missing_credit", BLE_HS_ETIMEOUT); }
    }
    if (!failure.load()) {
        const uint8_t command[] = {0x10, 0xff, 0x40};
        pending_status = true;
        const int sent = ble_gattc_write_no_rsp_flat(connection.load(), tx.value, command, sizeof(command));
        checked("status_write", sent);
        if (sent == 0) {
            credits.fetch_sub(1);
            vTaskDelay(pdMS_TO_TICKS(kPacketDelayMs));
            if (!wait_reply(reply, kReplyMs)) { fail("status_timeout", BLE_HS_ETIMEOUT); }
            if (!failure.load()) {
                printf("BLE_STATUS code=%02x known_normal=%d\n", status_code, status_code == 0);
                if (status_code != 0) { fail("unknown_status", BLE_HS_ENOTSUP); }
            }
        }
        memory("ble_sent", false);
    }
    size_t sent_bytes = 0;
    if (!failure.load() && job && bytes) {
        checked("preferred_mtu", ble_att_set_preferred_mtu(100));  // Printer's documented MTU.
        checked("exchange_mtu", ble_gattc_exchange_mtu(connection.load(), mtu_exchanged, nullptr));
        if (!failure.load() && !wait_reply(synced, kReplyMs)) { fail("mtu_timeout", BLE_HS_ETIMEOUT); }
        pending_completion = !failure.load();
        while (sent_bytes < bytes && !failure.load()) {
            while (credits.load() <= 0 && !failure.load()) {
                if (!wait_reply(credited, kCreditMs)) { fail("print_credit_timeout", BLE_HS_ETIMEOUT); }
            }
            if (failure.load()) { break; }
            if (cancel_control && cancel_control()) { fail("cancelled_before_send", BLE_HS_EAPP); break; }
            // Redraw "sending" before the first packet: drawing after it paused the stream ~100 ms
            // right as the printer started.
            if (cancel_control) { cancel_control = nullptr; if (started) { started(); } }
            const size_t count = std::min(bytes - sent_bytes, size_t{90});
            const int64_t sent_at = esp_timer_get_time();
            const int written = ble_gattc_write_no_rsp_flat(connection.load(), tx.value, job + sent_bytes, count);
            checked("print_write", written);
            if (written != 0) { break; }
            credits.fetch_sub(1);
            sent_bytes += count;
            printf("BLE_PACKET bytes=%u sent=%u total=%u credits=%d time_us=%lld\n", (unsigned)count,
                   (unsigned)sent_bytes, (unsigned)bytes, credits.load(), (long long)sent_at);
            while (esp_timer_get_time() - sent_at < kPacketDelayMs * 1000) { vTaskDelay(1); }
        }
        if (!failure.load() && !wait_reply(reply, kReplyMs)) { fail("print_completion_timeout", BLE_HS_ETIMEOUT); }
        // AA arrives while the tail feed is still running (docs/PROTOCOL.md); disconnecting at once left
        // the text inside the printer on paper. Stay connected like labelprinter/ble.py does.
        if (!failure.load()) { vTaskDelay(pdMS_TO_TICKS(kFeedSettleMs)); }
        // The battery query used to precede the job; since then labels lost their first millimetres on
        // paper (framed start gap, half of 本), so it now waits until the label is out. The label is
        // already printed here, so an unanswered query only hides the battery level.
        const uint8_t battery_command[] = {0x10, 0xff, 0x50, 0xf1};
        if (!failure.load() && (credits.load() > 0 || wait_reply(credited, kCreditMs))) {
            pending_battery = true;
            checked("battery_write", ble_gattc_write_no_rsp_flat(connection.load(), tx.value, battery_command, sizeof(battery_command)));
            credits.fetch_sub(1);
            if (!failure.load() && !wait_reply(reply, kCreditMs)) { pending_battery = false; printf("BLE_BATTERY unanswered\n"); }
        }
        memory("ble_printed", false);
    }
    closing = true;
    pending_status = false;
    pending_completion = false;
    pending_battery = false; cancel_control = nullptr;
    if (ble_gap_disc_active()) { checked("cancel_scan", ble_gap_disc_cancel()); }
    if (ble_gap_conn_active()) { checked("cancel_connect", ble_gap_conn_cancel()); }
    if (connection.load() != BLE_HS_CONN_HANDLE_NONE) {
        checked("disconnect", ble_gap_terminate(connection.load(), BLE_ERR_REM_USER_CONN_TERM));
        if (xSemaphoreTake(gone, pdMS_TO_TICKS(kReplyMs)) != pdTRUE) { fail("disconnect_timeout", BLE_HS_ETIMEOUT); }
    }
    const bool stopped = stop(memory);
    const bool success = !failure.load() && stopped;
    if (job) {
        printf("BLE_PRINT success=%d sent=%u total=%u completion=%02x uncertain=%d\n", success,
               (unsigned)sent_bytes, (unsigned)bytes, status_code, !success && sent_bytes > 0);
    } else { printf("BLE_QUERY %s\n", success ? "PASS" : "FAIL"); }
    return {success, sent_bytes, failure_operation.load(), status_code, battery_percent};
}

bool query(MemoryLog memory) { return transact(memory, nullptr, 0, nullptr, nullptr).success; }
PrintResult print(MemoryLog memory, const uint8_t *job, size_t bytes, bool (*cancelled)(), void (*started)())
{ return transact(memory, job, bytes, cancelled, started); }
}
