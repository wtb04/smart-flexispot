#include "ota.h"

#include "deskproto.h"
#include "units.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#if __has_include("ota_secrets.h")
#include "ota_secrets.h"
#endif
#ifndef OTA_KEY
#define OTA_KEY ""
#endif

#include <cstdio>
#include <cstring>

namespace ota {
namespace {
constexpr char TAG[] = "ota";

constexpr char KEY_HEADER[]         = "X-Update-Key";
constexpr char COMPANION_PROJECT[]  = "desk_companion";
constexpr std::size_t KEY_MAX       = 64;
constexpr std::size_t CHUNK         = 4 * units::kBytesPerKiB;
constexpr std::size_t COMPANION_MAX = 960 * units::kBytesPerKiB;  // one of its app slots

// The image's own description sits after its header and first segment's.
constexpr std::size_t APP_DESC_AT = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);

constexpr std::int64_t PROVE_WITHIN_US  = 3 * units::kUsPerMinute;
constexpr std::int64_t RESTART_AFTER_US = units::kUsPerSecond;  // for the answer to get out
constexpr int          RECV_TIMEOUT_S   = 30;
constexpr std::uint32_t SERVER_STACK    = 8 * units::kBytesPerKiB;
constexpr int          PROGRESS_STEP    = 25;  // percent between notices

Hooks              s_hooks{};
httpd_handle_t     s_server   = nullptr;
esp_timer_handle_t s_deadline = nullptr;
std::uint8_t      *s_chunk    = nullptr;

void restart_now(void *)
{
    if (s_hooks.restart != nullptr) {
        s_hooks.restart();
    }
    esp_restart();
}

void restart_in(std::int64_t after_us, esp_timer_handle_t *timer, const char *name)
{
    const esp_timer_create_args_t args{.callback = restart_now, .arg = nullptr,
                                       .dispatch_method = ESP_TIMER_TASK, .name = name,
                                       .skip_unhandled_events = false};
    if (esp_timer_create(&args, timer) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_once(*timer, after_us));
    }
}

void notice(const char *message)
{
    ESP_LOGI(TAG, "%s", message);
    if (s_hooks.notice != nullptr) {
        s_hooks.notice(message);
    }
}

esp_err_t answer(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, text);
}

// Refuses anything without the key, and everything while there is no key.
bool allowed(httpd_req_t *req)
{
    char key[KEY_MAX] = {};
    return OTA_KEY[0] != '\0' &&
           httpd_req_get_hdr_value_str(req, KEY_HEADER, key, sizeof(key)) == ESP_OK &&
           std::strcmp(key, OTA_KEY) == 0;
}

/** The checks both updates start with; false when it has already answered. */
bool may_update(httpd_req_t *req, std::size_t limit)
{
    if (!allowed(req)) {
        answer(req, "403 Forbidden", "wrong or missing update key\n");
        return false;
    }
    if (s_hooks.busy != nullptr && s_hooks.busy()) {
        answer(req, "409 Conflict", "the desk is moving\n");
        return false;
    }
    if (req->content_len == 0 || req->content_len > limit) {
        answer(req, "400 Bad Request", "no image, or too big for its slot\n");
        return false;
    }
    return true;
}

/** Up to `want` bytes of the body; negative when the sender went quiet or away. */
int receive(httpd_req_t *req, std::uint8_t *into, std::size_t want)
{
    for (;;) {
        const int got = httpd_req_recv(req, reinterpret_cast<char *>(into), want);
        if (got != HTTPD_SOCK_ERR_TIMEOUT) {
            return got;
        }
    }
}

bool same_project(const esp_app_desc_t &arrived, const char *expected)
{
    return std::strncmp(arrived.project_name, expected, sizeof(arrived.project_name)) == 0;
}

esp_err_t update_panel(httpd_req_t *req)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    if (slot == nullptr || !may_update(req, slot->size)) {
        return slot == nullptr ? answer(req, "500 Internal Server Error", "no update slot\n") : ESP_OK;
    }
    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        return answer(req, "500 Internal Server Error", "could not open the update slot\n");
    }
    notice("Updating the panel");

    std::size_t left = req->content_len;
    while (left > 0) {
        const int got = receive(req, s_chunk, std::min(left, CHUNK));
        if (got <= 0) {
            esp_ota_abort(handle);
            notice("The panel update did not arrive whole");
            return answer(req, "500 Internal Server Error", "the image did not arrive whole\n");
        }
        // The first write checks that what arrives starts as a firmware does.
        if (esp_ota_write(handle, s_chunk, static_cast<std::size_t>(got)) != ESP_OK) {
            esp_ota_abort(handle);
            notice("That was not a panel firmware");
            return answer(req, "400 Bad Request", "not a valid panel firmware\n");
        }
        left -= static_cast<std::size_t>(got);
    }

    esp_app_desc_t arrived{};
    if (esp_ota_end(handle) != ESP_OK || esp_ota_get_partition_description(slot, &arrived) != ESP_OK ||
        !same_project(arrived, esp_app_get_description()->project_name)) {
        notice("That was not a panel firmware");
        return answer(req, "400 Bad Request", "not a valid panel firmware\n");
    }
    if (esp_ota_set_boot_partition(slot) != ESP_OK) {
        return answer(req, "500 Internal Server Error", "could not switch to it\n");
    }
    char text[96];
    std::snprintf(text, sizeof(text), "panel firmware %s written, restarting\n", arrived.version);
    answer(req, "200 OK", text);
    notice("Restarting into the new firmware");
    static esp_timer_handle_t reboot = nullptr;
    restart_in(RESTART_AFTER_US, &reboot, "ota-restart");
    return ESP_OK;
}

void relay_progress(int percent)
{
    static int said = 0;
    if (percent == 0) {
        said = 0;
    }
    if (percent >= said + PROGRESS_STEP && percent < 100) {
        said = percent - percent % PROGRESS_STEP;
        char text[48];
        std::snprintf(text, sizeof(text), "Updating the companion, %d%%", said);
        notice(text);
    }
}

// The companion's image is taken whole before any of it goes over the link, so
// a broken upload never reaches it, and checked for being a companion's.
esp_err_t update_companion(httpd_req_t *req)
{
    if (!may_update(req, COMPANION_MAX)) {
        return ESP_OK;
    }
    if (s_hooks.relay == nullptr) {
        return answer(req, "501 Not Implemented", "no link to the companion\n");
    }
    const std::size_t size  = req->content_len;
    auto             *image = static_cast<std::uint8_t *>(
        heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (image == nullptr) {
        return answer(req, "500 Internal Server Error", "no room for the image\n");
    }
    std::size_t taken = 0;
    while (taken < size) {
        const int got = receive(req, image + taken, std::min(size - taken, CHUNK));
        if (got <= 0) {
            heap_caps_free(image);
            return answer(req, "500 Internal Server Error", "the image did not arrive whole\n");
        }
        taken += static_cast<std::size_t>(got);
    }

    esp_app_desc_t arrived{};
    const bool     shaped = size > APP_DESC_AT + sizeof(arrived) && image[0] == ESP_IMAGE_HEADER_MAGIC;
    if (shaped) {
        std::memcpy(&arrived, image + APP_DESC_AT, sizeof(arrived));
    }
    if (!shaped || arrived.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !same_project(arrived, COMPANION_PROJECT)) {
        heap_caps_free(image);
        return answer(req, "400 Bad Request", "not a companion firmware\n");
    }

    notice("Updating the companion");
    relay_progress(0);
    const esp_err_t sent =
        s_hooks.relay(image, size, deskproto::crc32(0, image, size), relay_progress);
    heap_caps_free(image);
    if (sent != ESP_OK) {
        notice("The companion update did not go through");
        return answer(req, "502 Bad Gateway", esp_err_to_name(sent));
    }
    notice("The companion is restarting into its new firmware");
    char text[96];
    std::snprintf(text, sizeof(text), "companion firmware %s sent, it is restarting\n", arrived.version);
    return answer(req, "200 OK", text);
}

esp_err_t version(httpd_req_t *req)
{
    const esp_app_desc_t *running = esp_app_get_description();
    char                  text[96];
    std::snprintf(text, sizeof(text), "%s %s\n", running->project_name, running->version);
    return answer(req, "200 OK", text);
}

}  // namespace

esp_err_t start(const Hooks &hooks)
{
    ESP_RETURN_ON_FALSE(s_server == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_hooks = hooks;
    s_chunk = static_cast<std::uint8_t *>(heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_chunk != nullptr, ESP_ERR_NO_MEM, TAG, "chunk");

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.stack_size       = SERVER_STACK;
    config.recv_wait_timeout = RECV_TIMEOUT_S;
    config.send_wait_timeout = RECV_TIMEOUT_S;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "server");

    const httpd_uri_t routes[] = {
        {.uri = "/update/panel", .method = HTTP_POST, .handler = update_panel, .user_ctx = nullptr},
        {.uri = "/update/companion", .method = HTTP_POST, .handler = update_companion, .user_ctx = nullptr},
        {.uri = "/version", .method = HTTP_GET, .handler = version, .user_ctx = nullptr},
    };
    for (const httpd_uri_t &route : routes) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &route), TAG, "route");
    }
    if (OTA_KEY[0] == '\0') {
        ESP_LOGW(TAG, "no OTA_KEY in ota_secrets.h: every update will be refused");
    }
    return ESP_OK;
}

void watch()
{
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGW(TAG, "new firmware, on trial until it reaches the network");
    restart_in(PROVE_WITHIN_US, &s_deadline, "ota-trial");
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

}  // namespace ota
