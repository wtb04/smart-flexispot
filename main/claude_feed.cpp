#include "claude_feed.h"

#include "claude.h"
#include "ota.h"
#include "sound.h"
#include "ui.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

#if __has_include("claude_secrets.h")
#include "claude_secrets.h"
#endif
#ifndef CLAUDE_KEY
#define CLAUDE_KEY ""
#endif

#include <cstdio>
#include <cstring>

namespace claude_feed {
namespace {
constexpr char        TAG[]        = "claude";
constexpr char        KEY_HEADER[] = "X-Claude-Key";
constexpr std::size_t KEY_MAX      = 64;
constexpr std::size_t BODY_MAX     = 8 * 1024;  // a long step list; an event alone is a few hundred bytes
constexpr int         ASKED_MS     = 60 * 1000;  // the pill stays amber after, until it is answered

bool allowed(httpd_req_t *req)
{
    char key[KEY_MAX] = {};
    return CLAUDE_KEY[0] != '\0' && httpd_req_get_hdr_value_str(req, KEY_HEADER, key, sizeof(key)) == ESP_OK &&
           std::strcmp(key, CLAUDE_KEY) == 0;
}

esp_err_t answer(httpd_req_t *req, const char *status)
{
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, nullptr, 0);
}

void tell_asked(const claude::Session &session)
{
    char wants[96];
    claude::describe(session.doing, claude::Tense::Wanted, wants, sizeof(wants));
    char title[64];
    std::snprintf(title, sizeof(title), "%s needs you", session.project);
    char message[128];
    std::snprintf(message, sizeof(message), "%s %s", session.machine, wants);
    sound::ding();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify("Claude Code", title, message, ui::Level::Warn, ASKED_MS));
}

esp_err_t event(httpd_req_t *req)
{
    if (!allowed(req)) {
        return answer(req, "403 Forbidden");
    }
    if (req->content_len == 0 || req->content_len > BODY_MAX) {
        return answer(req, "413 Content Too Large");
    }
    // One server task, so one buffer; in PSRAM, as internal RAM is short.
    static char *body = static_cast<char *>(heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM));
    if (body == nullptr) {
        return answer(req, "503 Service Unavailable");
    }
    std::size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += static_cast<std::size_t>(n);
    }
    claude::Session asked;
    bool            was_asked = false;
    if (!claude::take(body, got, asked, was_asked)) {
        ESP_LOGW(TAG, "not an event: %.*s", static_cast<int>(got < 120 ? got : 120), body);
        return answer(req, "400 Bad Request");
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_claude());
    if (was_asked) {
        tell_asked(asked);
    }
    return answer(req, "204 No Content");
}
}  // namespace

esp_err_t start()
{
    httpd_handle_t server = ota::server();
    ESP_RETURN_ON_FALSE(server != nullptr, ESP_ERR_INVALID_STATE, TAG, "no server");
    if (CLAUDE_KEY[0] == '\0') {
        ESP_LOGI(TAG, "no CLAUDE_KEY: the laptops' sessions are refused");
    }
    const httpd_uri_t route = {.uri = "/claude", .method = HTTP_POST, .handler = event, .user_ctx = nullptr};
    return httpd_register_uri_handler(server, &route);
}

}  // namespace claude_feed
