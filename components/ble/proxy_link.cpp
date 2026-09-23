#include "proxy_link.h"

#include "ble.h"
#include "deskproto.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace ble::proxy {
namespace {
constexpr char TAG[]  = "desklink";
constexpr char NAME[] = "desk-companion";

constexpr ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT(0x2d, 0x71, 0x9a, 0x4c, 0x8e, 0x3b, 0x4f,
                                                        0x6a, 0x9c, 0x1d, 0x5e, 0x77, 0x01, 0x00,
                                                        0xa5, 0xde);
constexpr ble_uuid128_t ECHO_UUID    = BLE_UUID128_INIT(0x2d, 0x71, 0x9a, 0x4c, 0x8e, 0x3b, 0x4f,
                                                        0x6a, 0x9c, 0x1d, 0x5e, 0x77, 0x02, 0x00,
                                                        0xa5, 0xde);

constexpr std::uint16_t ITVL_MIN_UNITS = 6;   // 7.5 ms, the floor the spec allows
constexpr std::uint16_t ITVL_MAX_UNITS = 8;   // 10 ms
constexpr std::uint16_t SUPERVISION_UNITS = 400;  // 4 s, in 10 ms units

constexpr std::int64_t PROBE_TIMEOUT_US = 1000000;

std::uint16_t s_conn  = BLE_HS_CONN_HANDLE_NONE;
std::uint16_t s_echo  = 0;
bool          s_connecting = false;

void (*s_rescan)() = nullptr;

std::atomic<bool> s_up{false};

std::atomic<std::uint32_t> s_seq{0};
std::atomic<std::int64_t>  s_sent_us{0};
std::atomic<bool>          s_waiting{false};
std::atomic<std::uint32_t> s_probe_seq{0};

portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
constexpr int SAMPLE_MAX = 256;
int s_samples[SAMPLE_MAX];
int s_sample_count = 0;
int s_lost         = 0;

SemaphoreHandle_t s_send_lock = nullptr;
StaticSemaphore_t s_send_lock_ctrl;

std::uint32_t s_last_sent_seq = 0;

bool send(deskproto::Op op, deskproto::Motion direction, std::uint8_t preset)
{
    if (!s_up.load(std::memory_order_relaxed) || s_echo == 0 || s_send_lock == nullptr) {
        return false;
    }
    deskproto::Command command{};
    command.op        = op;
    command.direction = direction;
    command.preset    = preset;
    command.seq       = s_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    s_last_sent_seq   = command.seq;

    std::uint8_t packet[deskproto::COMMAND_LEN];
    deskproto::encode(command, packet);

    const bool timing = !s_waiting.exchange(true, std::memory_order_relaxed);
    if (timing) {
        s_sent_us.store(esp_timer_get_time(), std::memory_order_relaxed);
        s_probe_seq.store(command.seq, std::memory_order_relaxed);
    }

    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    const int rc = ble_gattc_write_no_rsp_flat(s_conn, s_echo, packet, sizeof(packet));
    xSemaphoreGive(s_send_lock);

    if (timing && rc != 0) {
        s_waiting.store(false, std::memory_order_relaxed);
    }

    if (rc != 0) {
        static std::int64_t complained = 0;
        if (esp_timer_get_time() - complained > 5000000) {
            complained = esp_timer_get_time();
            ESP_LOGW(TAG, "write refused (%d)", rc);
        }
    }
    return rc == 0;
}

void record(int microseconds)
{
    portENTER_CRITICAL(&s_stats_lock);
    if (s_sample_count < SAMPLE_MAX) {
        s_samples[s_sample_count++] = microseconds;
    } else {
        std::memmove(s_samples, s_samples + 1, sizeof(int) * (SAMPLE_MAX - 1));
        s_samples[SAMPLE_MAX - 1] = microseconds;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

bool advert_is_proxy(const ble_gap_disc_desc &advert)
{
    ble_hs_adv_fields fields{};
    if (ble_hs_adv_parse_fields(&fields, advert.data, advert.length_data) != 0) {
        return false;
    }
    return fields.name != nullptr && fields.name_len == sizeof(NAME) - 1 &&
           std::memcmp(fields.name, NAME, fields.name_len) == 0;
}

int on_chr(std::uint16_t conn, const ble_gatt_error *error, const ble_gatt_chr *chr, void *)
{
    if (error->status == 0 && chr != nullptr) {
        s_echo = chr->val_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        return 0;
    }
    if (s_echo == 0) {
        ESP_LOGE(TAG, "no echo characteristic; dropping");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    ble_gap_upd_params params{};
    params.itvl_min            = ITVL_MIN_UNITS;
    params.itvl_max            = ITVL_MAX_UNITS;
    params.latency             = 0;
    params.supervision_timeout = SUPERVISION_UNITS;
    ble_gap_update_params(conn, &params);

    s_up.store(true, std::memory_order_relaxed);
    ESP_LOGI(TAG, "linked to the desk proxy");
    send(deskproto::Op::Wake, deskproto::Motion::Idle, 0);
    if (s_rescan != nullptr) {
        s_rescan();
    }
    return 0;
}

int on_svc(std::uint16_t conn, const ble_gatt_error *error, const ble_gatt_svc *svc, void *)
{
    static std::uint16_t start = 0;
    static std::uint16_t end   = 0;

    if (error->status == 0 && svc != nullptr) {
        start = svc->start_handle;
        end   = svc->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE || start == 0) {
        ESP_LOGE(TAG, "service not found (%d)", error->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    ble_gattc_disc_chrs_by_uuid(conn, start, end, &ECHO_UUID.u, on_chr, nullptr);
    return 0;
}

std::atomic<int> s_hold{static_cast<int>(deskproto::Motion::Idle)};
desk::StatusHandler s_on_status = nullptr;

std::atomic<int>  s_last_height{-1};
std::atomic<bool> s_box_linked{false};
std::atomic<int>  s_last_motion{static_cast<int>(deskproto::Motion::Idle)};
std::atomic<bool> s_ever_heard{false};

constexpr TickType_t HOLD_PERIOD = pdMS_TO_TICKS(100);

TaskHandle_t s_hold_task = nullptr;

void expire_stale_timing();

[[noreturn]] void hold_task(void *)
{
    auto last = deskproto::Motion::Idle;

    for (;;) {
        const auto wanted = static_cast<deskproto::Motion>(s_hold.load(std::memory_order_relaxed));

        if (wanted != deskproto::Motion::Idle) {
            send(deskproto::Op::Hold, wanted, 0);
        } else if (last != deskproto::Motion::Idle) {
            send(deskproto::Op::Stop, deskproto::Motion::Idle, 0);
            send(deskproto::Op::Stop, deskproto::Motion::Idle, 0);
        }
        last = wanted;
        expire_stale_timing();
        ulTaskNotifyTake(pdTRUE, HOLD_PERIOD);
    }
}

void expire_stale_timing()
{
    if (!s_waiting.load(std::memory_order_relaxed)) {
        return;
    }
    if (esp_timer_get_time() - s_sent_us.load(std::memory_order_relaxed) > PROBE_TIMEOUT_US) {
        s_waiting.store(false, std::memory_order_relaxed);
        portENTER_CRITICAL(&s_stats_lock);
        ++s_lost;
        portEXIT_CRITICAL(&s_stats_lock);
    }
}

int on_conn_event(ble_gap_event *event, void *)
{
    handle(event);
    return 0;
}

}  // namespace

void set_rescan(void (*rescan)())
{
    s_rescan = rescan;
}

void recover()
{
    if (s_connecting && !ble_gap_conn_active()) {
        ESP_LOGW(TAG, "connection attempt went quiet; looking again");
        s_connecting = false;
    }
}

bool consider(const ble_gap_disc_desc &advert)
{
    if (s_up.load(std::memory_order_relaxed) || s_connecting || !advert_is_proxy(advert)) {
        return false;
    }

    s_connecting = true;
    ble_gap_disc_cancel();

    ble_gap_conn_params params{};
    params.scan_itvl           = 0x0010;
    params.scan_window         = 0x0010;
    params.itvl_min            = ITVL_MIN_UNITS;
    params.itvl_max            = ITVL_MAX_UNITS;
    params.latency             = 0;
    params.supervision_timeout = SUPERVISION_UNITS;
    params.min_ce_len          = 0;
    params.max_ce_len          = 0;

    ESP_LOGI(TAG, "found the proxy, connecting");
    if (ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &advert.addr, 5000, &params, on_conn_event,
                        nullptr) != 0) {
        s_connecting = false;
        if (s_rescan != nullptr) {
            s_rescan();
        }
    }
    return true;
}

bool handle(ble_gap_event *event)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            s_connecting = false;
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "connect failed (%d)", event->connect.status);
                if (s_rescan != nullptr) {
                    s_rescan();
                }
                return true;
            }
            s_conn = event->connect.conn_handle;
            s_echo = 0;
            ble_gattc_disc_svc_by_uuid(s_conn, &SERVICE_UUID.u, on_svc, nullptr);
            return true;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "link lost (reason %d); looking again", event->disconnect.reason);
            s_connecting = false;
            s_up.store(false, std::memory_order_relaxed);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_echo = 0;
            s_waiting.store(false, std::memory_order_relaxed);
            if (s_rescan != nullptr) {
                s_rescan();
            }
            return true;

        case BLE_GAP_EVENT_CONN_UPDATE: {
            ble_gap_conn_desc desc{};
            if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
                ESP_LOGI(TAG, "connection interval %u.%02u ms",
                         static_cast<unsigned>(desc.conn_itvl * 125 / 100),
                         static_cast<unsigned>((desc.conn_itvl * 125) % 100));
            }
            return true;
        }

        case BLE_GAP_EVENT_NOTIFY_RX: {
            std::uint8_t  payload[deskproto::STATUS_LEN];
            std::uint16_t length = 0;
            deskproto::Status status{};
            if (ble_hs_mbuf_to_flat(event->notify_rx.om, payload, sizeof(payload), &length) == 0 &&
                deskproto::decode(payload, length, status)) {
                s_last_height.store(status.height_mm, std::memory_order_relaxed);
                s_box_linked.store(status.linked, std::memory_order_relaxed);
                s_last_motion.store(static_cast<int>(status.motion), std::memory_order_relaxed);
                s_ever_heard.store(true, std::memory_order_relaxed);
                if (s_on_status != nullptr) {
                    s_on_status(status.height_mm, status.linked, status.motion);
                }
            }

            if (s_waiting.load(std::memory_order_relaxed) &&
                status.seq == s_probe_seq.load(std::memory_order_relaxed)) {
                const std::int64_t round_trip =
                    esp_timer_get_time() - s_sent_us.load(std::memory_order_relaxed);
                s_waiting.store(false, std::memory_order_relaxed);
                record(static_cast<int>(round_trip));
            }
            return true;
        }

        default:
            return false;
    }
}

bool connected()
{
    return s_up.load(std::memory_order_relaxed);
}

void collect(LinkStats &out)
{
    int copy[SAMPLE_MAX];
    int count = 0;

    portENTER_CRITICAL(&s_stats_lock);
    count = s_sample_count;
    std::memcpy(copy, s_samples, sizeof(int) * static_cast<std::size_t>(count));
    out.lost = s_lost;
    portEXIT_CRITICAL(&s_stats_lock);

    out.connected = s_up.load(std::memory_order_relaxed);
    out.samples   = count;
    if (count == 0) {
        return;
    }

    std::sort(copy, copy + count);
    out.min_us    = copy[0];
    out.median_us = copy[count / 2];
    out.p99_us    = copy[(count * 99) / 100];
    out.max_us    = copy[count - 1];
}

bool last_status(int &height_mm, bool &box_linked, deskproto::Motion &motion)
{
    height_mm  = s_last_height.load(std::memory_order_relaxed);
    box_linked = s_box_linked.load(std::memory_order_relaxed);
    motion     = static_cast<deskproto::Motion>(s_last_motion.load(std::memory_order_relaxed));
    return s_ever_heard.load(std::memory_order_relaxed);
}

void set_status_handler(desk::StatusHandler handler)
{
    s_on_status = handler;
}

void set_hold(deskproto::Motion direction)
{
    const int previous = s_hold.exchange(static_cast<int>(direction), std::memory_order_relaxed);
    if (previous == static_cast<int>(direction)) {
        return;
    }
    ESP_LOGI(TAG, "hold %s", direction == deskproto::Motion::Up     ? "up"
                             : direction == deskproto::Motion::Down ? "down"
                                                                    : "released");
    if (s_hold_task != nullptr) {
        xTaskNotifyGive(s_hold_task);
    }
}

void send_command(deskproto::Op op, std::uint8_t preset)
{
    send(op, deskproto::Motion::Idle, preset);
}

esp_err_t start()
{
    if (s_hold_task != nullptr) {
        return ESP_OK;
    }
    s_send_lock = xSemaphoreCreateMutexStatic(&s_send_lock_ctrl);
    if (s_send_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(hold_task, "blehold", 2048, nullptr, 5, &s_hold_task) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

}  // namespace ble::proxy

namespace ble::desk {
void on_status(StatusHandler handler)
{
    proxy::set_status_handler(handler);
}

bool connected()
{
    return proxy::connected();
}

void hold(deskproto::Motion direction)
{
    proxy::set_hold(direction);
}

void preset(int index)
{
    proxy::send_command(deskproto::Op::Preset, static_cast<std::uint8_t>(index));
}

void store(int index)
{
    proxy::send_command(deskproto::Op::Store, static_cast<std::uint8_t>(index));
}

void wake()
{
    proxy::send_command(deskproto::Op::Wake, 0);
}

bool last(int &height_mm, bool &box_linked, deskproto::Motion &motion)
{
    return proxy::last_status(height_mm, box_linked, motion);
}

}  // namespace ble::desk

namespace ble {
LinkStats link_stats()
{
    LinkStats out{};
    proxy::collect(out);
    return out;
}

}  // namespace ble
