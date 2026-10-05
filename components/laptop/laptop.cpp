#include "laptop.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "net.h"
#include "ota.h"
#include "units.h"

#if __has_include("laptop_secrets.h")
#include "laptop_secrets.h"
#endif
#ifndef LAPTOP_KEY
#define LAPTOP_KEY ""
#endif

#include <cstdio>
#include <cstring>
#include <mutex>

namespace laptop {
namespace {
constexpr char        TAG[]        = "laptop";
constexpr char        KEY_HEADER[] = "X-Laptop-Key";
constexpr std::size_t KEY_MAX      = 129;  // 128 characters and the end
static_assert(sizeof(LAPTOP_KEY) <= KEY_MAX, "the key is longer than a request may carry");
constexpr std::size_t BODY_MAX     = 4 * units::kBytesPerKiB;

// Desk Link reports every few seconds while anything plays; a laptop that
// sleeps or leaves says nothing, and is forgotten after this long.
constexpr std::int64_t QUIET_US       = 30 * units::kUsPerSecond;
constexpr std::int64_t CHECK_EVERY_US = 5 * units::kUsPerSecond;

constexpr int COMMAND_DEADLINE_MS = 5 * units::kMsPerSecond;
constexpr int COMMAND_TIMEOUT_MS  = 2 * units::kMsPerSecond;

Handler            s_on_change = nullptr;
std::mutex         s_lock;
NowPlaying         s_now;
std::string        s_origin;  // http://address:port, where it answers
std::int64_t       s_heard_us = 0;
esp_timer_handle_t s_quiet    = nullptr;

bool same_key(const char *given, const char *key)
{
    const std::size_t length = std::strlen(key);
    std::size_t       diff   = std::strlen(given) ^ length;
    for (std::size_t i = 0; i < length; ++i) {
        diff |= static_cast<unsigned char>(given[i]) ^ static_cast<unsigned char>(key[i]);
    }
    return diff == 0;
}

bool allowed(httpd_req_t *req)
{
    char key[KEY_MAX] = {};
    return LAPTOP_KEY[0] != '\0' && httpd_req_get_hdr_value_str(req, KEY_HEADER, key, sizeof(key)) == ESP_OK &&
           same_key(key, LAPTOP_KEY);
}

esp_err_t answer(httpd_req_t *req, const char *status)
{
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, nullptr, 0);
}

// Who sent it: the report says only its port.
std::string sender(httpd_req_t *req)
{
    sockaddr_in6 peer{};
    socklen_t    size = sizeof(peer);
    if (getpeername(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr *>(&peer), &size) != 0) {
        return "";
    }
    char text[INET6_ADDRSTRLEN] = "";
    if (peer.sin6_family == AF_INET) {
        const auto *v4 = reinterpret_cast<const sockaddr_in *>(&peer);
        inet_ntoa_r(v4->sin_addr, text, sizeof(text));
    } else if (IN6_IS_ADDR_V4MAPPED(&peer.sin6_addr)) {
        in_addr v4{};
        std::memcpy(&v4, &peer.sin6_addr.s6_addr[12], sizeof(v4));
        inet_ntoa_r(v4, text, sizeof(text));
    }
    return text;
}

// net keeps one setting per origin, taken from whoever asks for it first; the
// cover is fetched from the same origin, so the key has to be in it before a
// report can name a cover.
net::Host host_with_key(const std::string &origin)
{
    static const std::string headers = std::string(KEY_HEADER) + ": " + LAPTOP_KEY;
    net::HostConfig          like;
    like.name        = "laptop";
    like.headers     = headers.c_str();
    like.timeout_ms  = COMMAND_TIMEOUT_MS;
    like.connections = 1;
    like.retry       = net::Retry{1, 300, 200, false};
    return net::host_for(origin, like);
}

void tell(const NowPlaying &now)
{
    if (s_on_change != nullptr) {
        s_on_change(now);
    }
}

esp_err_t report(httpd_req_t *req)
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
    NowPlaying  now;
    std::string from = sender(req);
    if (!laptop::read(body, got, now) || from.empty()) {
        ESP_LOGW(TAG, "not a report: %.*s", static_cast<int>(got < 120 ? got : 120), body);
        return answer(req, "400 Bad Request");
    }
    const std::string origin = "http://" + from + ':' + std::to_string(now.port);
    host_with_key(origin);
    {
        std::lock_guard<std::mutex> hold(s_lock);
        s_now      = now;
        s_origin   = origin;
        s_heard_us = esp_timer_get_time();
    }
    tell(now);
    return answer(req, "204 No Content");
}

void check_quiet(void *)
{
    {
        std::lock_guard<std::mutex> hold(s_lock);
        if (!s_now.active || esp_timer_get_time() - s_heard_us < QUIET_US) {
            return;
        }
        ESP_LOGI(TAG, "%s went quiet", s_now.machine.c_str());
        s_now = NowPlaying{};
    }
    tell(NowPlaying{});
}

void send(Command command, int value = 0)
{
    std::string origin;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        if (!s_now.active) {
            return;
        }
        origin = s_origin;
    }
    net::Request request;
    request.host        = host_with_key(origin);
    request.path        = "/command";
    request.method      = net::Method::Post;
    request.body        = command_body(command, value);
    request.priority    = net::Priority::Tap;
    request.deadline_ms = COMMAND_DEADLINE_MS;
    request.what        = "laptop command";
    // A slider dragged along sends where it stopped, not every level it passed.
    if (command == Command::Seek || command == Command::Volume) {
        request.key    = command == Command::Seek ? "seek" : "volume";
        request.dedupe = net::Dedupe::Replace;
    }
    request.done = [body = request.body](const net::Response &answer) {
        if (answer.ok()) {
            ESP_LOGI(TAG, "sent %s", body.c_str());
        } else if (answer.outcome != net::Outcome::Replaced) {
            ESP_LOGW(TAG, "%s not taken: %s, http %d", body.c_str(), net::outcome_name(answer.outcome),
                     answer.status);
        }
    };
    net::submit(std::move(request));
}
}  // namespace

esp_err_t start(Handler on_change)
{
    httpd_handle_t server = ota::server();
    ESP_RETURN_ON_FALSE(server != nullptr, ESP_ERR_INVALID_STATE, TAG, "no server");
    if (LAPTOP_KEY[0] == '\0') {
        ESP_LOGI(TAG, "no LAPTOP_KEY: the laptops' reports are refused");
    }
    s_on_change = on_change;
    const esp_timer_create_args_t quiet = {
        .callback              = check_quiet,
        .arg                   = nullptr,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "laptop",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&quiet, &s_quiet), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_quiet, CHECK_EVERY_US), TAG, "timer");
    const httpd_uri_t route = {.uri = "/laptop", .method = HTTP_POST, .handler = report, .user_ctx = nullptr};
    return httpd_register_uri_handler(server, &route);
}

// Said outright rather than toggled, so a second tap before the laptop
// reports does not undo the first; shown at once, as the next report would.
void play_pause()
{
    NowPlaying now;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        if (!s_now.active || !s_now.takes_pause) {
            ESP_LOGI(TAG, "play or pause, but %s", s_now.active ? "it does not take one" : "nothing plays");
            return;
        }
        s_now.playing = !s_now.playing;
        now           = s_now;
    }
    tell(now);
    send(now.playing ? Command::Play : Command::Pause);
}

void seek(int position_s)
{
    send(Command::Seek, position_s);
}

void next()
{
    send(Command::Next);
}

void previous()
{
    send(Command::Previous);
}

void set_volume(int percent)
{
    send(Command::Volume, percent);
}

void set_muted(bool muted)
{
    send(Command::Mute, muted ? 1 : 0);
}

std::string art_url(const NowPlaying &now)
{
    if (now.art.empty()) {
        return "";
    }
    std::lock_guard<std::mutex> hold(s_lock);
    return s_origin + art_path(now.art);
}
}  // namespace laptop
