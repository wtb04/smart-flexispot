#include "link.h"

#include "deskproto.h"
#include "loctek.h"

#include "esp_check.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace desklink {
namespace {
constexpr char TAG[]  = "desklink";
constexpr char NAME[] = "desk-companion";

int          s_height_mm  = -1;
TaskHandle_t s_status_task = nullptr;

constexpr std::int64_t BOX_QUIET_US = 4000000;

std::atomic<bool> s_box_up{false};

constexpr ble_uuid128_t SERVICE_UUID = BLE_UUID128_INIT(0x2d, 0x71, 0x9a, 0x4c, 0x8e, 0x3b, 0x4f,
                                                        0x6a, 0x9c, 0x1d, 0x5e, 0x77, 0x01, 0x00,
                                                        0xa5, 0xde);
constexpr ble_uuid128_t ECHO_UUID    = BLE_UUID128_INIT(0x2d, 0x71, 0x9a, 0x4c, 0x8e, 0x3b, 0x4f,
                                                        0x6a, 0x9c, 0x1d, 0x5e, 0x77, 0x02, 0x00,
                                                        0xa5, 0xde);

std::uint16_t s_echo_handle = 0;
std::uint16_t s_conn        = BLE_HS_CONN_HANDLE_NONE;
std::uint8_t  s_address_type = 0;

constexpr std::int64_t HOLD_GOOD_FOR_US = 300000;

// Written on the host task, read by the deadman and the status task.
std::atomic<std::int64_t>      s_hold_until{0};
std::atomic<deskproto::Motion> s_holding{deskproto::Motion::Idle};
std::uint32_t       s_last_seq    = 0;
bool                s_seq_started = false;

void advertise();

bool movement_allowed()
{
#if CONFIG_LOCTEK_PROXY_DRY_RUN
    return false;
#else
    return true;
#endif
}

void drive(deskproto::Motion motion)
{
    if (!movement_allowed()) {
        ESP_LOGW(TAG, "dry run: would drive %s",
                 motion == deskproto::Motion::Up     ? "up"
                 : motion == deskproto::Motion::Down ? "down"
                                                     : "stop");
        return;
    }

    const loctek::Move move = motion == deskproto::Motion::Up     ? loctek::Move::Up
                              : motion == deskproto::Motion::Down ? loctek::Move::Down
                                                                  : loctek::Move::Stop;
    ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::request_move(move));
}

deskproto::Motion motion_now()
{
    switch (loctek::motion()) {
        case loctek::Move::Up:   return deskproto::Motion::Up;
        case loctek::Move::Down: return deskproto::Motion::Down;
        case loctek::Move::Stop: break;
    }
    return deskproto::Motion::Idle;
}

void send_status()
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_echo_handle == 0) {
        return;
    }

    deskproto::Status status{};
    status.height_mm = loctek::stats().heights_decoded > 0 ? s_height_mm : -1;
    status.linked    = s_box_up.load(std::memory_order_relaxed);
    status.holding   = s_holding.load(std::memory_order_relaxed) != deskproto::Motion::Idle;
    status.driving   = loctek::driving_to() >= 0;
    status.motion    = motion_now();
    status.seq       = s_last_seq;

    std::uint8_t packet[deskproto::STATUS_LEN];
    deskproto::encode(status, packet);
    os_mbuf *out = ble_hs_mbuf_from_flat(packet, sizeof(packet));
    if (out == nullptr) {
        ESP_LOGW(TAG, "no buffer for status");
        return;
    }
    const int rc = ble_gatts_notify_custom(s_conn, s_echo_handle, out);
    static std::int64_t complained = 0;
    if (rc != 0 && esp_timer_get_time() - complained > 5000000) {
        complained = esp_timer_get_time();
        ESP_LOGW(TAG, "status refused (%d), conn %u handle %u", rc,
                 static_cast<unsigned>(s_conn), static_cast<unsigned>(s_echo_handle));
    }
}

QueueHandle_t s_slow = nullptr;

[[noreturn]] void slow_task(void *)
{
    deskproto::Command command{};
    for (;;) {
        if (xQueueReceive(s_slow, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (command.op) {
            case deskproto::Op::Preset:
                if (movement_allowed()) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(
                        loctek::goto_preset(static_cast<loctek::Preset>(command.preset)));
                } else {
                    ESP_LOGW(TAG, "dry run: would go to preset %u", command.preset);
                }
                break;
            case deskproto::Op::Store:
                if (movement_allowed()) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(
                        loctek::store_preset(static_cast<loctek::Preset>(command.preset)));
                } else {
                    ESP_LOGW(TAG, "dry run: would store preset %u", command.preset);
                }
                break;
            case deskproto::Op::Wake:
                ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::wake());
                break;
            default:
                break;
        }
    }
}

void apply(const deskproto::Command &command)
{
    switch (command.op) {
        case deskproto::Op::Hold:
            s_hold_until.store(esp_timer_get_time() + HOLD_GOOD_FOR_US, std::memory_order_relaxed);
            if (command.direction != s_holding.load(std::memory_order_relaxed)) {
                s_holding.store(command.direction, std::memory_order_relaxed);
                drive(command.direction);
            }
            break;

        case deskproto::Op::Stop:
            s_hold_until.store(0, std::memory_order_relaxed);
            if (s_holding.exchange(deskproto::Motion::Idle, std::memory_order_relaxed) !=
                    deskproto::Motion::Idle ||
                loctek::driving_to() >= 0) {
                drive(deskproto::Motion::Idle);
            }
            break;

        case deskproto::Op::GoTo:
            if (s_holding.load(std::memory_order_relaxed) != deskproto::Motion::Idle) {
                ESP_LOGW(TAG, "not travelling to %u mm: a hold is live", command.height_mm);
                break;
            }
            if (movement_allowed()) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::goto_height(command.height_mm));
            } else {
                ESP_LOGW(TAG, "dry run: would travel to %u mm", command.height_mm);
            }
            break;

        case deskproto::Op::Preset:
        case deskproto::Op::Store:
        case deskproto::Op::Wake:
            if (s_slow != nullptr) {
                xQueueSend(s_slow, &command, 0);
            }
            break;

        case deskproto::Op::Ping:
            break;
    }
}

int on_echo(std::uint16_t conn, std::uint16_t attr, ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    std::uint8_t  payload[32];
    std::uint16_t length = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &length) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    deskproto::Command command{};
    if (!deskproto::decode(payload, length, command)) {
        ESP_LOGW(TAG, "rejected a malformed command");
        return 0;
    }

    if (s_seq_started && command.seq <= s_last_seq) {
        return 0;
    }
    s_last_seq    = command.seq;
    s_seq_started = true;

    s_conn        = conn;
    s_echo_handle = attr;

    apply(command);
    send_status();
    return 0;
}

constexpr std::int64_t STATUS_MIN_GAP_US = 50000;
constexpr TickType_t   STATUS_KEEPALIVE  = pdMS_TO_TICKS(1000);

[[noreturn]] void status_task(void *)
{
    int           shown_height  = -2;
    int           shown_motion  = -1;
    bool          shown_up      = false;
    bool          shown_driving = false;
    std::int64_t  last_sent    = 0;
    std::uint32_t seen_frames  = 0;
    std::int64_t  seen_at      = 0;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, STATUS_KEEPALIVE);

        const std::uint32_t frames = loctek::stats().frames_decoded;
        const std::int64_t  now_us = esp_timer_get_time();
        if (frames != seen_frames) {
            seen_frames = frames;
            seen_at     = now_us;
        }
        const bool box_up = seen_at != 0 && now_us - seen_at < BOX_QUIET_US;
        if (box_up != s_box_up.exchange(box_up, std::memory_order_relaxed)) {
            ESP_LOGW(TAG, "control box %s", box_up ? "answering" : "gone quiet - check the cable");
        }

        static std::int64_t woke_at = 0;
        if (!box_up && now_us - woke_at > 30000000) {
            woke_at = now_us;
            ESP_LOGI(TAG, "nudging the box awake");
            if (s_slow != nullptr) {
                deskproto::Command wake{};
                wake.op = deskproto::Op::Wake;
                xQueueSend(s_slow, &wake, 0);
            }
        }

        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            continue;
        }

        const int  motion  = static_cast<int>(motion_now());
        const bool driving = loctek::driving_to() >= 0;
        const bool changed = s_height_mm != shown_height || motion != shown_motion ||
                             box_up != shown_up || driving != shown_driving;
        const std::int64_t now = esp_timer_get_time();
        if (changed && now - last_sent < STATUS_MIN_GAP_US) {
            continue;  // the next report, or the keepalive, will carry it
        }
        if (!changed && now - last_sent < 1000000) {
            continue;
        }
        shown_height  = s_height_mm;
        shown_motion  = motion;
        shown_up      = box_up;
        shown_driving = driving;
        last_sent     = now;
        send_status();
    }
}

[[noreturn]] void deadman_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (s_holding.load(std::memory_order_relaxed) != deskproto::Motion::Idle &&
            esp_timer_get_time() > s_hold_until.load(std::memory_order_relaxed)) {
            ESP_LOGW(TAG, "hold went quiet, stopping");
            s_holding.store(deskproto::Motion::Idle, std::memory_order_relaxed);
            drive(deskproto::Motion::Idle);
            send_status();
        }
    }
}

const ble_gatt_chr_def ECHO_CHRS[] = {
    {
        .uuid       = &ECHO_UUID.u,
        .access_cb  = on_echo,
        .arg        = nullptr,
        .descriptors = nullptr,
        .flags      = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_echo_handle,
        .cpfd       = nullptr,
    },
    {nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr},
};

const ble_gatt_svc_def SERVICES[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &SERVICE_UUID.u,
        .includes        = nullptr,
        .characteristics = ECHO_CHRS,
    },
    {0, nullptr, nullptr, nullptr},
};

int on_gap(ble_gap_event *event, void *)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn        = event->connect.conn_handle;
                s_seq_started = false;
                s_last_seq    = 0;
                ESP_LOGI(TAG, "panel connected");
            } else {
                advertise();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "panel gone (reason %d)", event->disconnect.reason);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            // Nothing can stop the desk from here once the panel is gone.
            if (s_holding.exchange(deskproto::Motion::Idle, std::memory_order_relaxed) !=
                deskproto::Motion::Idle) {
                ESP_LOGW(TAG, "link lost mid-hold, stopping");
                drive(deskproto::Motion::Idle);
            } else if (loctek::driving_to() >= 0) {
                ESP_LOGW(TAG, "link lost mid-travel, stopping");
                drive(deskproto::Motion::Idle);
            }
            advertise();
            break;

        case BLE_GAP_EVENT_CONN_UPDATE:
            ESP_LOGI(TAG, "connection updated");
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            break;

        default:
            break;
    }
    return 0;
}

void advertise()
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    ble_hs_adv_fields fields{};
    fields.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name             = reinterpret_cast<const std::uint8_t *>(NAME);
    fields.name_len         = sizeof(NAME) - 1;
    fields.name_is_complete = 1;
    if (const int err = ble_gap_adv_set_fields(&fields); err != 0) {
        ESP_LOGE(TAG, "advertising fields: %d", err);
        return;
    }

    ble_hs_adv_fields scan_response{};
    scan_response.uuids128             = const_cast<ble_uuid128_t *>(&SERVICE_UUID);
    scan_response.num_uuids128         = 1;
    scan_response.uuids128_is_complete = 1;
    if (const int err = ble_gap_adv_rsp_set_fields(&scan_response); err != 0) {
        ESP_LOGE(TAG, "scan response fields: %d", err);
        return;
    }

    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min  = BLE_GAP_ADV_ITVL_MS(30);
    params.itvl_max  = BLE_GAP_ADV_ITVL_MS(60);

    if (const int err = ble_gap_adv_start(s_address_type, nullptr, BLE_HS_FOREVER, &params, on_gap,
                                          nullptr);
        err != 0) {
        ESP_LOGE(TAG, "advertising start: %d", err);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"", NAME);
}

void on_sync()
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_address_type) != 0) {
        ESP_LOGE(TAG, "no usable address");
        return;
    }
    advertise();
}

void host_task(void *)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

bool panel_connected()
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

bool box_up()
{
    return s_box_up.load(std::memory_order_relaxed);
}

void note_height(int height_mm)
{
    if (height_mm == s_height_mm) {
        return;
    }
    s_height_mm = height_mm;
    if (s_status_task != nullptr) {
        xTaskNotifyGive(s_status_task);
    }
}

esp_err_t start()
{
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init");

    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_RETURN_ON_FALSE(ble_gatts_count_cfg(SERVICES) == 0, ESP_FAIL, TAG, "gatt count");
    ESP_RETURN_ON_FALSE(ble_gatts_add_svcs(SERVICES) == 0, ESP_FAIL, TAG, "gatt add");
    ESP_RETURN_ON_FALSE(ble_svc_gap_device_name_set(NAME) == 0, ESP_FAIL, TAG, "name");

    nimble_port_freertos_init(host_task);
    ESP_RETURN_ON_FALSE(xTaskCreate(deadman_task, "deadman", 3072, nullptr, 6, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "deadman");

    if (movement_allowed()) {
        ESP_LOGW(TAG, "LIVE: commands will move the desk");
    } else {
        ESP_LOGI(TAG, "dry run: commands are decoded but the desk will not move");
    }

    ESP_RETURN_ON_FALSE(xTaskCreate(status_task, "deskstat", 3072, nullptr, 4, &s_status_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "status task");

    s_slow = xQueueCreate(4, sizeof(deskproto::Command));
    ESP_RETURN_ON_FALSE(s_slow != nullptr, ESP_ERR_NO_MEM, TAG, "slow queue");
    ESP_RETURN_ON_FALSE(xTaskCreate(slow_task, "deskslow", 4096, nullptr, 4, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "slow task");
    return ESP_OK;
}

}  // namespace desklink
