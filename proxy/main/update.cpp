#include "update.h"

#include "loctek.h"
#include "units.h"

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <atomic>
#include <cstring>

namespace update {
namespace {
constexpr char TAG[] = "update";

// Long enough for the panel to find the companion again after its restart.
constexpr std::int64_t PROVE_WITHIN_US = 3 * units::kUsPerMinute;
// Time for the answer to the last piece to reach the panel before the restart.
constexpr std::int64_t RESTART_AFTER_US = 500 * units::kUsPerMs;

esp_ota_handle_t       s_handle   = 0;
const esp_partition_t *s_slot     = nullptr;
std::uint32_t          s_size     = 0;
std::uint32_t          s_expected = 0;
std::uint32_t          s_crc      = 0;
std::uint32_t          s_received = 0;
std::atomic<bool>      s_running{false};
esp_timer_handle_t     s_deadline = nullptr;

void restart(void *)
{
    esp_restart();
}

void restart_in(std::int64_t after_us, esp_timer_handle_t *timer, const char *name)
{
    const esp_timer_create_args_t args{.callback = restart, .arg = nullptr,
                                       .dispatch_method = ESP_TIMER_TASK, .name = name,
                                       .skip_unhandled_events = false};
    if (esp_timer_create(&args, timer) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_once(*timer, after_us));
    }
}

bool desk_still()
{
    return loctek::motion() == loctek::Move::Stop && loctek::driving_to() < 0;
}

int begin(const deskproto::UpdateMessage &message)
{
    if (!desk_still()) {
        ESP_LOGW(TAG, "not while the desk moves");
        return deskproto::kUpdateBusy;
    }
    abandon();
    s_slot = esp_ota_get_next_update_partition(nullptr);
    if (s_slot == nullptr || message.size == 0 || message.size > s_slot->size) {
        ESP_LOGE(TAG, "no room for %u bytes", static_cast<unsigned>(message.size));
        return deskproto::kUpdateBadImage;
    }
    // Sequential writes erase a sector at a time as the image arrives, so no
    // single step stalls the radio for the whole slot's erase.
    if (esp_ota_begin(s_slot, OTA_WITH_SEQUENTIAL_WRITES, &s_handle) != ESP_OK) {
        s_handle = 0;
        return deskproto::kUpdateFlashFault;
    }
    s_size     = message.size;
    s_expected = message.crc;
    s_crc      = 0;
    s_received = 0;
    s_running.store(true, std::memory_order_relaxed);
    ESP_LOGI(TAG, "receiving %u bytes into %s", static_cast<unsigned>(s_size), s_slot->label);
    return 0;
}

int piece(const deskproto::UpdateMessage &message)
{
    if (!running() || message.offset != s_received || s_received + message.length > s_size) {
        return deskproto::kUpdateOutOfOrder;
    }
    if (esp_ota_write(s_handle, message.data, message.length) != ESP_OK) {
        abandon();
        return deskproto::kUpdateFlashFault;
    }
    s_crc = deskproto::crc32(s_crc, message.data, message.length);
    s_received += static_cast<std::uint32_t>(message.length);
    return 0;
}

// The image must be whole, match what the panel sent, pass the bootloader's own
// check, and be a companion's: the panel's firmware would not fit, but a stray
// image of anything else might.
int finish()
{
    if (!running() || s_received != s_size || s_crc != s_expected) {
        ESP_LOGE(TAG, "image incomplete or damaged");
        abandon();
        return deskproto::kUpdateBadImage;
    }
    const esp_err_t ended = esp_ota_end(s_handle);
    s_handle              = 0;
    s_running.store(false, std::memory_order_relaxed);
    if (ended != ESP_OK) {
        ESP_LOGE(TAG, "image refused: %s", esp_err_to_name(ended));
        return deskproto::kUpdateBadImage;
    }
    esp_app_desc_t arrived{};
    if (esp_ota_get_partition_description(s_slot, &arrived) != ESP_OK ||
        std::strcmp(arrived.project_name, esp_app_get_description()->project_name) != 0) {
        ESP_LOGE(TAG, "not a companion firmware");
        return deskproto::kUpdateBadImage;
    }
    if (esp_ota_set_boot_partition(s_slot) != ESP_OK) {
        return deskproto::kUpdateFlashFault;
    }
    ESP_LOGW(TAG, "firmware %s written, restarting into it", arrived.version);
    static esp_timer_handle_t reboot = nullptr;
    restart_in(RESTART_AFTER_US, &reboot, "update-restart");
    return 0;
}

}  // namespace

int take(const deskproto::UpdateMessage &message)
{
    switch (message.step) {
        case deskproto::UpdateStep::Begin:   return begin(message);
        case deskproto::UpdateStep::Piece:   return piece(message);
        case deskproto::UpdateStep::Finish:  return finish();
        case deskproto::UpdateStep::Abandon: abandon(); return 0;
    }
    return deskproto::kUpdateOutOfOrder;
}

void abandon()
{
    if (s_handle != 0) {
        esp_ota_abort(s_handle);
        s_handle = 0;
        ESP_LOGW(TAG, "update abandoned");
    }
    s_running.store(false, std::memory_order_relaxed);
}

bool running()
{
    return s_running.load(std::memory_order_relaxed);
}

void watch()
{
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGW(TAG, "new firmware, on trial until the panel links");
    restart_in(PROVE_WITHIN_US, &s_deadline, "update-trial");
}

void confirm()
{
    if (s_deadline == nullptr) {
        return;
    }
    esp_timer_stop(s_deadline);
    esp_timer_delete(s_deadline);
    s_deadline = nullptr;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "new firmware confirmed");
    }
}

}  // namespace update
