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
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "settings.h"

#if __has_include("ota_secrets.h")
#include "ota_secrets.h"
#endif
#ifndef OTA_KEY
#define OTA_KEY ""
#endif

#include <algorithm>
#include <array>
#include <iterator>
#include <cstdio>
#include <cstring>

namespace ota {
namespace {
constexpr char TAG[] = "ota";

constexpr char KEY_HEADER[]        = "X-Update-Key";
constexpr char INSTALL_KEY[]       = "install";
constexpr char INSTALL_NOW[]       = "now";
constexpr char COMPANION_PROJECT[] = "desk_companion";
constexpr std::size_t KEY_MAX      = 64;
constexpr std::size_t QUERY_MAX    = 32;
constexpr std::size_t CHUNK        = 4 * units::kBytesPerKiB;
constexpr std::size_t COMPANION_MAX = 960 * units::kBytesPerKiB;  // one of its app slots

// The image's own description sits after its header and first segment's.
constexpr std::size_t APP_DESC_AT = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
constexpr std::size_t DIGEST      = 32;  // the SHA-256 an image ends with

// A panel image waits in the app slot it will boot from, marked in NVS; a
// companion's waits in the storage partition, behind a header written last so
// half an image is never taken for a whole one.
constexpr char        NVS_NAMESPACE[]  = "ota";
constexpr char        NVS_PANEL_KEY[]  = "panel_ready";
constexpr char        STORE_LABEL[]    = "storage";
constexpr std::uint32_t STORE_MAGIC    = 0x57464643;  // "CFFW"
constexpr std::size_t STORE_IMAGE_AT   = 4 * units::kBytesPerKiB;  // a sector past the header

struct Stored {
    std::uint32_t magic;
    std::uint32_t size;
    std::uint32_t crc;
};

constexpr std::int64_t PROVE_WITHIN_US  = 3 * units::kUsPerMinute;
constexpr std::int64_t RESTART_AFTER_US = units::kUsPerSecond;  // for the answer to get out
constexpr int          RECV_TIMEOUT_S   = 30;
// These and a development build's pages, with room for more; the default is 8.
constexpr int MAX_ROUTES = 16;
constexpr std::uint32_t SERVER_STACK    = 8 * units::kBytesPerKiB;
constexpr std::uint32_t INSTALL_STACK   = 6 * units::kBytesPerKiB;
constexpr UBaseType_t   INSTALL_PRIORITY = 3;
constexpr TickType_t    STILL_CHECK     = pdMS_TO_TICKS(500);
constexpr int           PERCENT_ALL     = 100;
constexpr std::int64_t  ESTIMATE_AFTER_US = 2 * units::kUsPerSecond;  // before it, the rate is noise

Hooks              s_hooks{};
Status             s_status{};
httpd_handle_t     s_server   = nullptr;
esp_timer_handle_t s_deadline = nullptr;
std::uint8_t      *s_chunk    = nullptr;
TaskHandle_t       s_installer = nullptr;
std::int64_t       s_busy_since = 0;

void publish()
{
    if (s_hooks.status != nullptr) {
        s_hooks.status(s_status);
    }
}

void set_busy(Target target, Phase phase = Phase::Receiving, bool immediate = false)
{
    s_status.busy         = target;
    s_status.phase        = phase;
    s_status.immediate    = immediate || phase == Phase::Installing;
    s_status.percent      = 0;
    s_status.seconds_left = -1;
    s_busy_since          = esp_timer_get_time();
    publish();
}

/** How far it is, and how long the rest takes at the rate so far. */
void advance(std::size_t done, std::size_t total)
{
    const std::int64_t elapsed = esp_timer_get_time() - s_busy_since;
    const int          percent = static_cast<int>(done * PERCENT_ALL / total);
    int                left    = -1;
    if (done > 0 && elapsed >= ESTIMATE_AFTER_US) {
        const std::int64_t rest_us = elapsed * static_cast<std::int64_t>(total - done) /
                                     static_cast<std::int64_t>(done);
        left = static_cast<int>((rest_us + units::kUsPerSecond - 1) / units::kUsPerSecond);
    }
    if (percent != s_status.percent || left != s_status.seconds_left) {
        s_status.percent      = percent;
        s_status.seconds_left = left;
        publish();
    }
}

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

// Written at once: the restart that installs it may come straight after.
settings::Record<std::uint8_t> s_panel_ready{NVS_NAMESPACE, NVS_PANEL_KEY, 0};

void remember_panel_ready(bool ready)
{
    s_panel_ready.set(ready ? 1 : 0, settings::Write::Now);
}

bool remembered_panel_ready()
{
    return s_panel_ready.get() != 0;
}

const esp_partition_t *store()
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, STORE_LABEL);
}

bool stored_header(Stored &out)
{
    const esp_partition_t *part = store();
    return part != nullptr && esp_partition_read(part, 0, &out, sizeof(out)) == ESP_OK &&
           out.magic == STORE_MAGIC && out.size > 0 && out.size <= COMPANION_MAX;
}

esp_err_t keep_companion(const std::uint8_t *image, std::size_t size, std::uint32_t crc)
{
    const esp_partition_t *part = store();
    ESP_RETURN_ON_FALSE(part != nullptr && STORE_IMAGE_AT + size <= part->size, ESP_ERR_NO_MEM,
                        TAG, "no room to keep the companion's image");
    const std::size_t span = (STORE_IMAGE_AT + size + part->erase_size - 1) / part->erase_size *
                             part->erase_size;
    ESP_RETURN_ON_ERROR(esp_partition_erase_range(part, 0, span), TAG, "erase");
    ESP_RETURN_ON_ERROR(esp_partition_write(part, STORE_IMAGE_AT, image, size), TAG, "image");
    const Stored header{STORE_MAGIC, static_cast<std::uint32_t>(size), crc};
    return esp_partition_write(part, 0, &header, sizeof(header));
}

void forget_companion()
{
    if (const esp_partition_t *part = store(); part != nullptr) {
        esp_partition_erase_range(part, 0, part->erase_size);
    }
}

/** An image is whole when its last bytes are the SHA-256 of all before them. */
bool whole(const std::uint8_t *image, std::size_t size)
{
    const auto *header = reinterpret_cast<const esp_image_header_t *>(image);
    if (size <= sizeof(*header) + DIGEST || header->hash_appended != 1) {
        return false;
    }
    std::uint8_t digest[DIGEST];
    return mbedtls_sha256(image, size - DIGEST, digest, 0) == 0 &&
           std::memcmp(digest, image + size - DIGEST, DIGEST) == 0;
}

/** The kept companion image, read back from flash and whole; nullptr when there
 *  is none. The caller frees it. */
std::uint8_t *load_companion(Stored &header)
{
    if (!stored_header(header)) {
        return nullptr;
    }
    auto *image = static_cast<std::uint8_t *>(
        heap_caps_malloc(header.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (image == nullptr) {
        return nullptr;
    }
    if (esp_partition_read(store(), STORE_IMAGE_AT, image, header.size) != ESP_OK ||
        deskproto::crc32(0, image, header.size) != header.crc || !whole(image, header.size)) {
        heap_caps_free(image);
        return nullptr;
    }
    return image;
}

/** Keeps it, then reads it back: only an image found whole in flash is ready. */
esp_err_t keep_and_check(const std::uint8_t *image, std::size_t size, std::uint32_t crc)
{
    ESP_RETURN_ON_ERROR(keep_companion(image, size, crc), TAG, "keep");
    Stored         header{};
    std::uint8_t  *kept = load_companion(header);
    if (kept == nullptr) {
        forget_companion();
        ESP_LOGE(TAG, "the kept companion image did not read back whole");
        return ESP_ERR_INVALID_CRC;
    }
    heap_caps_free(kept);
    return ESP_OK;
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

bool asked_now(httpd_req_t *req)
{
    char query[QUERY_MAX] = {};
    char value[QUERY_MAX] = {};
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, INSTALL_KEY, value, sizeof(value)) == ESP_OK &&
           std::strcmp(value, INSTALL_NOW) == 0;
}

/** The checks both updates start with; false when it has already answered. */
bool may_update(httpd_req_t *req, std::size_t limit, bool now)
{
    if (!allowed(req)) {
        answer(req, "403 Forbidden", "wrong or missing update key\n");
        return false;
    }
    if (now && s_hooks.busy != nullptr && s_hooks.busy()) {
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

/** The body into the spare app slot; answers and returns nullptr when it fails. */
const esp_partition_t *receive_panel(httpd_req_t *req, esp_app_desc_t &arrived)
{
    const esp_partition_t *slot   = esp_ota_get_next_update_partition(nullptr);
    esp_ota_handle_t       handle = 0;
    if (slot == nullptr || esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        answer(req, "500 Internal Server Error", "could not open the update slot\n");
        return nullptr;
    }
    for (std::size_t left = req->content_len; left > 0;) {
        const int got = receive(req, s_chunk, std::min(left, CHUNK));
        if (got <= 0) {
            esp_ota_abort(handle);
            answer(req, "500 Internal Server Error", "the image did not arrive whole\n");
            return nullptr;
        }
        // The first write checks that what arrives starts as a firmware does.
        if (esp_ota_write(handle, s_chunk, static_cast<std::size_t>(got)) != ESP_OK) {
            esp_ota_abort(handle);
            answer(req, "400 Bad Request", "not a valid panel firmware\n");
            return nullptr;
        }
        left -= static_cast<std::size_t>(got);
        advance(req->content_len - left, req->content_len);
    }
    if (esp_ota_end(handle) != ESP_OK || esp_ota_get_partition_description(slot, &arrived) != ESP_OK ||
        !same_project(arrived, esp_app_get_description()->project_name)) {
        answer(req, "400 Bad Request", "not a valid panel firmware\n");
        return nullptr;
    }
    return slot;
}

esp_err_t update_panel(httpd_req_t *req)
{
    const esp_partition_t *spare = esp_ota_get_next_update_partition(nullptr);
    const bool             now   = asked_now(req);
    if (spare == nullptr || !may_update(req, spare->size, now)) {
        return spare == nullptr ? answer(req, "500 Internal Server Error", "no update slot\n") : ESP_OK;
    }
    // What was ready in the slot is overwritten from the first byte.
    remember_panel_ready(false);
    s_status.panel_ready = false;
    set_busy(Target::Panel, Phase::Receiving, now);
    esp_app_desc_t         arrived{};
    const esp_partition_t *slot = receive_panel(req, arrived);
    if (slot == nullptr) {
        set_busy(Target::None);
        return ESP_OK;
    }

    char text[96];
    if (!now) {
        remember_panel_ready(true);
        s_status.panel_ready = true;
        set_busy(Target::None);
        std::snprintf(text, sizeof(text), "panel firmware %s ready: Update now in Setup\n",
                      arrived.version);
        return answer(req, "200 OK", text);
    }
    if (esp_ota_set_boot_partition(slot) != ESP_OK) {
        set_busy(Target::None);
        return answer(req, "500 Internal Server Error", "could not switch to it\n");
    }
    std::snprintf(text, sizeof(text), "panel firmware %s written, restarting\n", arrived.version);
    answer(req, "200 OK", text);
    static esp_timer_handle_t reboot = nullptr;
    restart_in(RESTART_AFTER_US, &reboot, "ota-restart");
    return ESP_OK;
}

void relay_progress(int percent)
{
    advance(static_cast<std::size_t>(percent), PERCENT_ALL);
}

esp_err_t relay(const std::uint8_t *image, std::size_t size, std::uint32_t crc)
{
    if (s_hooks.relay == nullptr) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    set_busy(Target::Companion, Phase::Installing);
    const esp_err_t sent = s_hooks.relay(image, size, crc, relay_progress);
    set_busy(Target::None);
    return sent;
}

/** The whole body in PSRAM, checked for being a companion's; nullptr, answered,
 *  when it is not. */
std::uint8_t *receive_companion(httpd_req_t *req, esp_app_desc_t &arrived)
{
    const std::size_t size  = req->content_len;
    auto             *image = static_cast<std::uint8_t *>(
        heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (image == nullptr) {
        answer(req, "500 Internal Server Error", "no room for the image\n");
        return nullptr;
    }
    for (std::size_t taken = 0; taken < size;) {
        const int got = receive(req, image + taken, std::min(size - taken, CHUNK));
        if (got <= 0) {
            heap_caps_free(image);
            answer(req, "500 Internal Server Error", "the image did not arrive whole\n");
            return nullptr;
        }
        taken += static_cast<std::size_t>(got);
        advance(taken, size);
    }
    const bool shaped = size > APP_DESC_AT + sizeof(arrived) && image[0] == ESP_IMAGE_HEADER_MAGIC;
    if (shaped) {
        std::memcpy(&arrived, image + APP_DESC_AT, sizeof(arrived));
    }
    if (!shaped || arrived.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !same_project(arrived, COMPANION_PROJECT) || !whole(image, size)) {
        heap_caps_free(image);
        answer(req, "400 Bad Request", "not a companion firmware\n");
        return nullptr;
    }
    return image;
}

// The companion's image is taken whole before any of it goes anywhere, so a
// broken upload never reaches it.
esp_err_t update_companion(httpd_req_t *req)
{
    const bool now = asked_now(req);
    if (!may_update(req, COMPANION_MAX, now)) {
        return ESP_OK;
    }
    set_busy(Target::Companion, Phase::Receiving, now);
    esp_app_desc_t arrived{};
    std::uint8_t  *image = receive_companion(req, arrived);
    if (image == nullptr) {
        set_busy(Target::None);
        return ESP_OK;
    }
    const std::size_t   size = req->content_len;
    const std::uint32_t crc  = deskproto::crc32(0, image, size);
    char                text[96];

    if (!now) {
        const esp_err_t kept = keep_and_check(image, size, crc);
        heap_caps_free(image);
        s_status.companion_ready = kept == ESP_OK;
        set_busy(Target::None);
        if (kept != ESP_OK) {
            return answer(req, "500 Internal Server Error", "could not keep the image\n");
        }
        std::snprintf(text, sizeof(text), "companion firmware %s ready: Update now in Setup\n",
                      arrived.version);
        return answer(req, "200 OK", text);
    }
    const esp_err_t sent = relay(image, size, crc);
    heap_caps_free(image);
    if (sent != ESP_OK) {
        return answer(req, "502 Bad Gateway", esp_err_to_name(sent));
    }
    std::snprintf(text, sizeof(text), "companion firmware %s sent, it is restarting\n",
                  arrived.version);
    return answer(req, "200 OK", text);
}

esp_err_t version(httpd_req_t *req)
{
    const esp_app_desc_t *running = esp_app_get_description();
    // With the start of the image's checksum: every development build of a
    // commit has the one version, and often the one build time too.
    char sha[9];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    char text[128];
    std::snprintf(text, sizeof(text), "%s %s, image %s\n", running->project_name, running->version, sha);
    return answer(req, "200 OK", text);
}

esp_err_t install_companion()
{
    Stored        header{};
    std::uint8_t *image = load_companion(header);
    esp_err_t     err   = ESP_ERR_INVALID_CRC;
    if (image != nullptr) {
        err = relay(image, header.size, header.crc);
        heap_caps_free(image);
    }
    if (err == ESP_OK || err == ESP_ERR_INVALID_CRC) {
        forget_companion();  // installed, or not worth trying again
        s_status.companion_ready = false;
        publish();
    }
    return err;
}

// Waits for the desk to stand still, sends the companion its image and then,
// when there is one, restarts the panel into its own.
[[noreturn]] void install_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (s_hooks.busy != nullptr && s_hooks.busy()) {
            vTaskDelay(STILL_CHECK);
        }
        if (s_status.companion_ready) {
            const esp_err_t err = install_companion();
            ESP_LOGI(TAG, "companion update: %s", esp_err_to_name(err));
        }
        const esp_partition_t *spare = esp_ota_get_next_update_partition(nullptr);
        if (s_status.panel_ready && spare != nullptr && esp_ota_set_boot_partition(spare) == ESP_OK) {
            remember_panel_ready(false);
            ESP_LOGI(TAG, "restarting into the panel update");
            restart_now(nullptr);
        }
    }
}

// What is waiting from before a restart: a panel image still in its slot, a
// companion's still whole in storage.
void find_ready()
{
    esp_app_desc_t         waiting{};
    const esp_partition_t *spare = esp_ota_get_next_update_partition(nullptr);
    s_status.panel_ready = remembered_panel_ready() && spare != nullptr &&
                           esp_ota_get_partition_description(spare, &waiting) == ESP_OK &&
                           same_project(waiting, esp_app_get_description()->project_name);
    Stored        header{};
    std::uint8_t *kept       = load_companion(header);
    s_status.companion_ready = kept != nullptr;
    heap_caps_free(kept);
}

}  // namespace

esp_err_t start(const Hooks &hooks)
{
    ESP_RETURN_ON_FALSE(s_server == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_hooks = hooks;
    s_chunk = static_cast<std::uint8_t *>(heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_chunk != nullptr, ESP_ERR_NO_MEM, TAG, "chunk");
    ESP_RETURN_ON_FALSE(xTaskCreate(install_task, "ota-install", INSTALL_STACK, nullptr,
                                    INSTALL_PRIORITY, &s_installer) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "install task");
    find_ready();
    publish();

    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.stack_size        = SERVER_STACK;
    config.recv_wait_timeout = RECV_TIMEOUT_S;
    config.send_wait_timeout = RECV_TIMEOUT_S;
    config.max_uri_handlers  = MAX_ROUTES;
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

httpd_handle_t server()
{
    return s_server;
}

bool authorised(httpd_req_t *req)
{
    return allowed(req);
}

void install()
{
    if (s_installer != nullptr) {
        xTaskNotifyGive(s_installer);
    }
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

namespace {
using ImageMark = std::array<char, 9>;
settings::Record<ImageMark> s_rolled_told{NVS_NAMESPACE, "rolled_told", ImageMark{}};
}  // namespace

RolledBack rolled_back()
{
    static const RolledBack found = [] {
        RolledBack             out;
        const esp_partition_t *turned = esp_ota_get_last_invalid_partition();
        esp_app_desc_t         desc{};
        if (turned == nullptr || esp_ota_get_partition_description(turned, &desc) != ESP_OK) {
            return out;
        }
        out.happened = true;
        std::snprintf(out.version, sizeof(out.version), "%s", desc.version);
        for (int i = 0; i < 4; ++i) {
            std::snprintf(out.image + 2 * i, 3, "%02x", desc.app_elf_sha256[i]);
        }
        ImageMark mark{};
        std::copy(std::begin(out.image), std::end(out.image), mark.begin());
        out.new_now = s_rolled_told.get() != mark;
        if (out.new_now) {
            s_rolled_told.set(mark);
            ESP_LOGW(TAG, "update %s (%s) did not take: back on %s", out.version, out.image,
                     esp_app_get_description()->version);
        }
        return out;
    }();
    return found;
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
