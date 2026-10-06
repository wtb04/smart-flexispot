#include "proxy_link.h"

#include "ble.h"
#include "ble_desk.h"
#include "deskproto.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "units.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>

namespace ble::proxy {
namespace {
constexpr char TAG[]  = "desklink";
constexpr const char *NAME = deskproto::kDeviceName;

constexpr ble_uuid128_t uuid128(const deskproto::Uuid128 &bytes)
{
    ble_uuid128_t uuid{};
    uuid.u.type = BLE_UUID_TYPE_128;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        uuid.value[i] = bytes[i];
    }
    return uuid;
}

constexpr ble_uuid128_t SERVICE_UUID = uuid128(deskproto::kServiceUuid);
constexpr ble_uuid128_t ECHO_UUID    = uuid128(deskproto::kEchoUuid);
constexpr ble_uuid128_t UPDATE_UUID  = uuid128(deskproto::kUpdateUuid);

// Connection intervals count 1.25 ms, supervision timeouts 10 ms.
constexpr int           CONN_ITVL_UNIT_US = 1250;
constexpr std::uint16_t ITVL_MIN_UNITS    = 6;    // 7.5 ms, the floor the spec allows
constexpr std::uint16_t ITVL_MAX_UNITS    = 8;    // 10 ms
constexpr std::uint16_t SUPERVISION_UNITS = 400;  // 4 s
// Dark, a desk moved from Home Assistant does not need its link quick: half a
// second between events is some 7 mA less from the battery, measured.
constexpr std::uint16_t DARK_ITVL_UNITS   = 400;  // 500 ms

// Scanning flat out while connecting: a 10 ms window every 10 ms, in 0.625 ms.
constexpr std::uint16_t CONNECT_SCAN_UNITS = 0x0010;
constexpr std::int32_t  CONNECT_TIMEOUT_MS = 5000;

constexpr std::int64_t PROBE_TIMEOUT_US = units::kUsPerSecond;

std::uint16_t s_conn  = BLE_HS_CONN_HANDLE_NONE;
std::uint16_t s_echo  = 0;
std::uint16_t s_update = 0;  // absent on a companion too old to take updates
bool          s_connecting = false;

void (*s_rescan)() = nullptr;

std::atomic<bool> s_up{false};

std::atomic<bool> s_dark{false};

void ask_interval(std::uint16_t conn)
{
    const bool          dark = s_dark.load(std::memory_order_relaxed);
    ble_gap_upd_params params{};
    params.itvl_min            = dark ? DARK_ITVL_UNITS : ITVL_MIN_UNITS;
    params.itvl_max            = dark ? DARK_ITVL_UNITS : ITVL_MAX_UNITS;
    params.latency             = 0;
    params.supervision_timeout = SUPERVISION_UNITS;
    ble_gap_update_params(conn, &params);
}

// The proxy reports at least once a second; a link that says nothing for this
// long is up in name only, and is dropped so the scanner finds it again.
constexpr std::int64_t    SILENT_US = 5 * units::kUsPerSecond;
std::atomic<std::int64_t> s_heard_us{0};

constexpr std::int64_t COMPLAINT_GAP_US = 5 * units::kUsPerSecond;

// Written without a response, so a lost stop would go unnoticed.
constexpr int STOP_SENDS = 2;

constexpr std::uint32_t HOLD_TASK_STACK    = 2048;
constexpr UBaseType_t   HOLD_TASK_PRIORITY = 5;

constexpr int PERCENTILE = 99;
constexpr int PERCENT    = 100;

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

bool send(deskproto::Op op, deskproto::Motion direction, std::uint8_t preset,
          std::uint16_t height_mm = 0)
{
    if (!s_up.load(std::memory_order_relaxed) || s_echo == 0 || s_send_lock == nullptr) {
        return false;
    }
    deskproto::Command command{};
    command.op        = op;
    command.direction = direction;
    command.preset    = preset;
    command.height_mm = height_mm;

    // The proxy drops anything numbered below what it last saw, so the number
    // and the write have to happen in the same breath.
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    command.seq = s_seq.fetch_add(1, std::memory_order_relaxed) + 1;

    std::uint8_t packet[deskproto::kCommandLen];
    deskproto::encode(command, packet);

    const bool timing = !s_waiting.exchange(true, std::memory_order_relaxed);
    if (timing) {
        s_sent_us.store(esp_timer_get_time(), std::memory_order_relaxed);
        s_probe_seq.store(command.seq, std::memory_order_relaxed);
    }
    const int rc = ble_gattc_write_no_rsp_flat(s_conn, s_echo, packet, sizeof(packet));
    xSemaphoreGive(s_send_lock);

    if (timing && rc != 0) {
        s_waiting.store(false, std::memory_order_relaxed);
    }

    if (rc != 0) {
        static std::int64_t complained = 0;
        if (esp_timer_get_time() - complained > COMPLAINT_GAP_US) {
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
    return fields.name != nullptr && fields.name_len == std::strlen(NAME) &&
           std::memcmp(fields.name, NAME, fields.name_len) == 0;
}

int on_chr(std::uint16_t conn, const ble_gatt_error *error, const ble_gatt_chr *chr, void *)
{
    if (error->status == 0 && chr != nullptr) {
        if (ble_uuid_cmp(&chr->uuid.u, &ECHO_UUID.u) == 0) {
            s_echo = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UPDATE_UUID.u) == 0) {
            s_update = chr->val_handle;
        }
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

    ask_interval(conn);

    s_heard_us.store(esp_timer_get_time(), std::memory_order_relaxed);
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
    ble_gattc_disc_all_chrs(conn, start, end, on_chr, nullptr);
    return 0;
}

std::atomic<int> s_hold{static_cast<int>(deskproto::Motion::Idle)};
desk::StatusHandler s_on_status = nullptr;

std::atomic<int>  s_last_height{-1};
std::atomic<bool> s_box_linked{false};
std::atomic<int>  s_last_motion{static_cast<int>(deskproto::Motion::Idle)};
std::atomic<bool> s_last_driving{false};
std::atomic<bool> s_ever_heard{false};

void drop_if_silent()
{
    if (!s_up.load(std::memory_order_relaxed)) {
        return;
    }
    const std::int64_t now = esp_timer_get_time();
    if (now - s_heard_us.load(std::memory_order_relaxed) < SILENT_US) {
        return;
    }
    s_heard_us.store(now, std::memory_order_relaxed);  // once per silence
    ESP_LOGW(TAG, "proxy silent for 5 s, dropping the link");
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

constexpr TickType_t HOLD_PERIOD = pdMS_TO_TICKS(deskproto::kHoldPeriodMs);

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
            for (int i = 0; i < STOP_SENDS; ++i) {
                send(deskproto::Op::Stop, deskproto::Motion::Idle, 0);
            }
        }
        last = wanted;
        expire_stale_timing();
        drop_if_silent();
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

void log_interval(std::uint16_t conn)
{
    constexpr unsigned HUNDREDTHS_PER_MS = 100;
    constexpr auto     US_PER_HUNDREDTH  = units::kUsPerMs / HUNDREDTHS_PER_MS;
    ble_gap_conn_desc  desc{};
    if (ble_gap_conn_find(conn, &desc) == 0) {
        const auto hundredths =
            static_cast<unsigned>(desc.conn_itvl * CONN_ITVL_UNIT_US / US_PER_HUNDREDTH);
        ESP_LOGI(TAG, "connection interval %u.%02u ms", hundredths / HUNDREDTHS_PER_MS,
                 hundredths % HUNDREDTHS_PER_MS);
    }
}

void take_status(const deskproto::Status &status)
{
    s_last_height.store(status.height_mm, std::memory_order_relaxed);
    s_box_linked.store(status.linked, std::memory_order_relaxed);
    s_last_motion.store(static_cast<int>(status.motion), std::memory_order_relaxed);
    s_last_driving.store(status.driving, std::memory_order_relaxed);
    s_ever_heard.store(true, std::memory_order_relaxed);
    s_heard_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    if (s_on_status != nullptr) {
        s_on_status(status);
    }
}

void time_round_trip(std::uint32_t echoed_seq)
{
    if (s_waiting.load(std::memory_order_relaxed) &&
        echoed_seq == s_probe_seq.load(std::memory_order_relaxed)) {
        const std::int64_t round_trip =
            esp_timer_get_time() - s_sent_us.load(std::memory_order_relaxed);
        s_waiting.store(false, std::memory_order_relaxed);
        record(static_cast<int>(round_trip));
    }
}

void take_notification(os_mbuf *om)
{
    std::uint8_t      payload[deskproto::kStatusLen];
    std::uint16_t     length = 0;
    deskproto::Status status{};
    if (ble_hs_mbuf_to_flat(om, payload, sizeof(payload), &length) == 0 &&
        deskproto::decode(payload, length, status)) {
        take_status(status);
    }
    time_round_trip(status.seq);  // zero if unreadable, which no probe is numbered
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
    params.scan_itvl           = CONNECT_SCAN_UNITS;
    params.scan_window         = CONNECT_SCAN_UNITS;
    params.itvl_min            = ITVL_MIN_UNITS;
    params.itvl_max            = ITVL_MAX_UNITS;
    params.latency             = 0;
    params.supervision_timeout = SUPERVISION_UNITS;
    params.min_ce_len          = 0;
    params.max_ce_len          = 0;

    ESP_LOGI(TAG, "found the proxy, connecting");
    if (ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &advert.addr, CONNECT_TIMEOUT_MS, &params,
                        on_conn_event, nullptr) != 0) {
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
            s_conn   = event->connect.conn_handle;
            s_echo   = 0;
            s_update = 0;
            // Updates go in pieces as big as a write may be; the default leaves
            // room for twenty bytes.
            ble_gattc_exchange_mtu(s_conn, nullptr, nullptr);
            ble_gattc_disc_svc_by_uuid(s_conn, &SERVICE_UUID.u, on_svc, nullptr);
            return true;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "link lost (reason %d); looking again", event->disconnect.reason);
            s_connecting = false;
            s_up.store(false, std::memory_order_relaxed);
            s_conn   = BLE_HS_CONN_HANDLE_NONE;
            s_echo   = 0;
            s_update = 0;
            s_waiting.store(false, std::memory_order_relaxed);
            if (s_rescan != nullptr) {
                s_rescan();
            }
            return true;

        case BLE_GAP_EVENT_CONN_UPDATE:
            log_interval(event->conn_update.conn_handle);
            return true;

        case BLE_GAP_EVENT_NOTIFY_RX:
            take_notification(event->notify_rx.om);
            return true;

        default:
            return false;
    }
}

void set_dark(bool dark)
{
    if (s_dark.exchange(dark) != dark && s_up.load(std::memory_order_relaxed)) {
        ask_interval(s_conn);
    }
}

bool connected()
{
    return s_up.load(std::memory_order_relaxed);
}

int quiet_ms()
{
    if (!s_up.load(std::memory_order_relaxed)) {
        return -1;
    }
    return static_cast<int>((esp_timer_get_time() - s_heard_us.load(std::memory_order_relaxed)) /
                            units::kUsPerMs);
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
    out.p99_us    = copy[(count * PERCENTILE) / PERCENT];
    out.max_us    = copy[count - 1];
}

bool last_status(deskproto::Status &out)
{
    out.height_mm = s_last_height.load(std::memory_order_relaxed);
    out.linked    = s_box_linked.load(std::memory_order_relaxed);
    out.motion    = static_cast<deskproto::Motion>(s_last_motion.load(std::memory_order_relaxed));
    out.driving   = s_last_driving.load(std::memory_order_relaxed);
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

void send_command(deskproto::Op op, std::uint8_t preset, std::uint16_t height_mm)
{
    send(op, deskproto::Motion::Idle, preset, height_mm);
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
    return xTaskCreate(hold_task, "blehold", HOLD_TASK_STACK, nullptr, HOLD_TASK_PRIORITY,
                       &s_hold_task) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

// An update's steps, each written with a response and waited for, so the next
// is only sent once the companion has taken this one.
namespace {
constexpr int  STEP_TIMEOUT_MS = 5000;  // a flash erase and more, never a lost link
constexpr int  PERCENT_ALL     = 100;

SemaphoreHandle_t s_step_done = nullptr;
StaticSemaphore_t s_step_done_ctrl;
std::atomic<int>  s_step_status{0};

int on_step_written(std::uint16_t, const ble_gatt_error *error, ble_gatt_attr *, void *)
{
    s_step_status.store(error->status, std::memory_order_relaxed);
    xSemaphoreGive(s_step_done);
    return 0;
}

esp_err_t write_step(const deskproto::UpdateMessage &message, std::uint8_t *packet,
                     std::size_t capacity)
{
    const std::size_t length = deskproto::encode(message, packet, capacity);
    if (length == 0 || !s_up.load(std::memory_order_relaxed) || s_update == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_step_done, 0);
    if (ble_gattc_write_flat(s_conn, s_update, packet, static_cast<std::uint16_t>(length),
                             on_step_written, nullptr) != 0) {
        return ESP_FAIL;
    }
    if (xSemaphoreTake(s_step_done, pdMS_TO_TICKS(STEP_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const int status = s_step_status.load(std::memory_order_relaxed);
    if (status == 0) {
        return ESP_OK;
    }
    const int refusal = status - BLE_HS_ERR_ATT_BASE;
    ESP_LOGW(TAG, "update step refused (%d)", refusal);
    return refusal == deskproto::kUpdateBusy ? ESP_ERR_INVALID_STATE
           : refusal == deskproto::kUpdateBadImage ? ESP_ERR_INVALID_CRC
                                                   : ESP_FAIL;
}
}  // namespace

}  // namespace ble::proxy

namespace ble::desk {
esp_err_t send_update(const std::uint8_t *image, std::size_t size, std::uint32_t crc,
                      void (*progress)(int percent))
{
    using namespace ble::proxy;
    if (!s_up.load(std::memory_order_relaxed)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_update == 0) {
        ESP_LOGW(TAG, "the companion's firmware takes no updates over the link");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_step_done == nullptr) {
        s_step_done = xSemaphoreCreateBinaryStatic(&s_step_done_ctrl);
    }

    // As much as a write may carry: the link's MTU less the ATT header.
    constexpr std::size_t ATT_WRITE_HEADER = 3;
    std::uint8_t          packet[CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU];
    const std::size_t     capacity =
        std::min<std::size_t>(sizeof(packet), ble_att_mtu(s_conn) - ATT_WRITE_HEADER);
    const std::size_t piece_max = capacity - deskproto::update_at::kData;
    ESP_LOGI(TAG, "sending %u bytes in pieces of %u", static_cast<unsigned>(size),
             static_cast<unsigned>(piece_max));

    deskproto::UpdateMessage message;
    message.step = deskproto::UpdateStep::Begin;
    message.size = static_cast<std::uint32_t>(size);
    message.crc  = crc;
    esp_err_t err = write_step(message, packet, capacity);

    int told = -1;
    for (std::size_t offset = 0; err == ESP_OK && offset < size; offset += piece_max) {
        message        = deskproto::UpdateMessage{};
        message.step   = deskproto::UpdateStep::Piece;
        message.offset = static_cast<std::uint32_t>(offset);
        message.data   = image + offset;
        message.length = std::min(piece_max, size - offset);
        err            = write_step(message, packet, capacity);
        const int percent = static_cast<int>(offset * PERCENT_ALL / size);
        if (progress != nullptr && percent != told) {
            told = percent;
            progress(percent);
        }
    }

    message      = deskproto::UpdateMessage{};
    message.step = err == ESP_OK ? deskproto::UpdateStep::Finish : deskproto::UpdateStep::Abandon;
    const esp_err_t finished = write_step(message, packet, capacity);
    if (err == ESP_OK) {
        err = finished;
    }
    if (err == ESP_OK && progress != nullptr) {
        progress(PERCENT_ALL);
    }
    ESP_LOGI(TAG, "update %s", err == ESP_OK ? "taken, the companion restarts" : esp_err_to_name(err));
    return err;
}

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

void goto_height(int height_mm)
{
    if (height_mm < 0 || height_mm > std::numeric_limits<std::uint16_t>::max()) {
        return;
    }
    proxy::send_command(deskproto::Op::GoTo, 0, static_cast<std::uint16_t>(height_mm));
}

void stop()
{
    proxy::set_hold(deskproto::Motion::Idle);
    proxy::send_command(deskproto::Op::Stop, 0);
}

bool last(deskproto::Status &out)
{
    return proxy::last_status(out);
}

int quiet_ms()
{
    return proxy::quiet_ms();
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
