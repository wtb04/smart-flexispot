#include "laptop.h"

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "link_envelope.h"
#include "lwip/sockets.h"
#include "mbedtls/gcm.h"
#include "net.h"
#include "ota.h"
#include "units.h"

#if __has_include("desk_link_secrets.h")
#include "desk_link_secrets.h"
#endif
#ifndef DESK_LINK_KEY
#define DESK_LINK_KEY ""
#endif

#include <sys/time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace laptop {
namespace {
constexpr char        TAG[]     = "laptop";
constexpr char        PANEL[]   = "smart-flexispot";
constexpr char        LINK[]    = "/link";
constexpr char        COVER[]   = "/cover";
constexpr char        REPLY[]   = " reply";  // a path's answer is sealed to it and this
constexpr std::size_t BODY_MAX  = 8 * units::kBytesPerKiB;  // a long step list from the hook
constexpr std::size_t COVER_MAX = 512 * units::kBytesPerKiB;

// Desk Link says how it is every ten seconds; a laptop that sleeps or leaves
// says nothing, and is forgotten after this long.
constexpr std::int64_t QUIET_US       = 30 * units::kUsPerSecond;
constexpr std::int64_t CHECK_EVERY_US = 5 * units::kUsPerSecond;
// The panel tries the laptop's port back this often, and whenever it moves.
constexpr std::int64_t PING_EVERY_US = 60 * units::kUsPerSecond;
// A clock before this has not been set, and nothing it times can be fresh.
constexpr std::int64_t CLOCK_SET_MS = 1'750'000'000'000;

constexpr int COMMAND_DEADLINE_MS = 5 * units::kMsPerSecond;
constexpr int COMMAND_TIMEOUT_MS  = 2 * units::kMsPerSecond;
constexpr int COVER_TIMEOUT_MS    = 5 * units::kMsPerSecond;

// AES-256-GCM over the shared key; the server's task and net's workers seal
// and open, one at a time.
class Gcm : public link::Cipher {
public:
    bool start(const char *hex)
    {
        std::array<std::uint8_t, link::kKeyLen> key{};
        if (!link::parse_key(hex, key)) {
            return false;
        }
        mbedtls_gcm_init(&context_);
        ready_ = mbedtls_gcm_setkey(&context_, MBEDTLS_CIPHER_ID_AES, key.data(), key.size() * 8) == 0;
        return ready_;
    }

    bool seal(const link::Nonce &nonce, const std::string &aad, const std::string &plain, std::string &sealed) override
    {
        if (!ready_) {
            return false;
        }
        std::lock_guard<std::mutex> hold(lock_);
        sealed.assign(plain.size() + link::kTagLen, '\0');
        auto *out = reinterpret_cast<unsigned char *>(sealed.data());
        return mbedtls_gcm_crypt_and_tag(&context_, MBEDTLS_GCM_ENCRYPT, plain.size(), nonce.data(), nonce.size(),
                                         reinterpret_cast<const unsigned char *>(aad.data()), aad.size(),
                                         reinterpret_cast<const unsigned char *>(plain.data()), out, link::kTagLen,
                                         out + plain.size()) == 0;
    }

    bool open(const link::Nonce &nonce, const std::string &aad, const std::string &sealed, std::string &plain) override
    {
        if (!ready_ || sealed.size() < link::kTagLen) {
            return false;
        }
        std::lock_guard<std::mutex> hold(lock_);
        const std::size_t length = sealed.size() - link::kTagLen;
        plain.assign(length, '\0');
        const auto *in = reinterpret_cast<const unsigned char *>(sealed.data());
        return mbedtls_gcm_auth_decrypt(&context_, length, nonce.data(), nonce.size(),
                                        reinterpret_cast<const unsigned char *>(aad.data()), aad.size(),
                                        in + length, link::kTagLen, in,
                                        reinterpret_cast<unsigned char *>(plain.data())) == 0;
    }

private:
    mbedtls_gcm_context context_{};
    std::mutex          lock_;
    bool                ready_ = false;
};

Gcm                s_gcm;
link::Fresh        s_fresh;  // the server's task alone takes from it
Handlers           s_on;
std::mutex         s_lock;
NowPlaying         s_now;
std::string        s_origin;  // http://address:port, where it answers
std::int64_t       s_heard_us  = 0;
std::int64_t       s_pinged_us = 0;
int                s_reaches   = -1;  // whether the panel reaches the laptop's port back
int                s_missed    = 0;   // pings in a row it did not
esp_timer_handle_t s_quiet     = nullptr;

std::int64_t now_ms()
{
    timeval now{};
    gettimeofday(&now, nullptr);
    return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

std::string sealed(const std::string &path, const std::string &plain)
{
    link::Nonce nonce{};
    esp_fill_random(nonce.data(), nonce.size());
    return link::seal(s_gcm, nonce, path, plain);
}

/** An answer to what was sent to `path`, opened and checked to be the `type`
 *  asked for and fresh; with no type, a cover's picture. Empty when it was not one. */
std::string opened(const std::string &path, const char *body, std::size_t length, const char *type)
{
    std::string plain;
    link::Nonce nonce{};
    Head        head;
    if (body == nullptr || link::open(s_gcm, path + REPLY, body, length, plain, nonce) != link::Opened::Ok) {
        return "";
    }
    if (type == nullptr) {
        return plain;
    }
    const bool fresh = read_head(plain.data(), plain.size(), head) && head.type == type &&
                       std::llabs(head.at_ms - now_ms()) <= link::Fresh::kWindowMs;
    return fresh ? plain : "";
}

esp_err_t answer(httpd_req_t *req, const char *status, const std::string &body = "")
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

// Who sent it: a state says only its port.
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

net::Host host_of(const std::string &origin)
{
    net::HostConfig like;
    like.name        = "laptop";
    like.timeout_ms  = COVER_TIMEOUT_MS;
    // Desk Link closes each connection once it has answered; one kept open
    // here would be written to after that, and wait out the timeout.
    like.keep_open   = false;
    like.connections = 2;  // so a command never waits behind a cover
    like.retry       = net::Retry{1, 300, 200, false};
    // On the same network, a laptop that missed a few is back in seconds, as
    // one restarting Desk Link is; not left resting as a server would be.
    like.rest        = net::Rest{5, 2 * 1000, 2 * 1000};
    return net::host_for(origin, like);
}

// Anything sealed that came back from the laptop says the panel reaches it;
// it is said not to after two pings in a row that truly went unanswered.
void reached(const std::string &origin, bool yes)
{
    std::lock_guard<std::mutex> hold(s_lock);
    if (origin != s_origin) {
        return;
    }
    s_missed      = yes ? 0 : s_missed + 1;
    const int now = yes ? 1 : s_missed >= 2 ? 0 : s_reaches;
    if (now != s_reaches && now >= 0) {
        ESP_LOGI(TAG, "%s %s", s_now.machine.c_str(), now == 1 ? "answers back" : "does not answer back");
    }
    s_reaches = now;
}

void tell(const NowPlaying &now)
{
    if (s_on.playing != nullptr) {
        s_on.playing(now);
    }
}

// The laptop's port tried from here: a firewall there lets the laptop talk
// to the panel and keeps the panel's commands out.
void ping_back(const std::string &origin)
{
    net::Request request;
    request.host        = host_of(origin);
    request.path        = LINK;
    request.method      = net::Method::Post;
    request.body        = sealed(LINK, ping_body(now_ms()));
    request.priority    = net::Priority::Background;
    request.key         = "laptop ping";
    request.dedupe      = net::Dedupe::Replace;
    request.deadline_ms = COMMAND_DEADLINE_MS;
    request.what        = "laptop ping";
    request.done        = [origin](const net::Response &answer) {
        if (answer.outcome == net::Outcome::Answered || answer.outcome == net::Outcome::Failed) {
            reached(origin, answer.ok() && !opened(LINK, answer.body, answer.length, "pong").empty());
        }
    };
    net::submit(std::move(request));
}

esp_err_t take_state(httpd_req_t *req, const std::string &plain)
{
    NowPlaying        now;
    const std::string from = sender(req);
    if (!laptop::read(plain.data(), plain.size(), now) || from.empty()) {
        return answer(req, "400 Bad Request");
    }
    const std::string  origin  = "http://" + from + ':' + std::to_string(now.port);
    const std::int64_t heard   = esp_timer_get_time();
    bool               ping    = false;
    int                reaches = -1;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        if (origin != s_origin) {
            ESP_LOGI(TAG, "%s at %s", now.machine.c_str(), origin.c_str());
            s_reaches = -1;
            s_missed  = 0;
        }
        ping        = origin != s_origin || heard - s_pinged_us >= PING_EVERY_US;
        s_pinged_us = ping ? heard : s_pinged_us;
        s_now       = now;
        s_origin    = origin;
        s_heard_us  = heard;
        reaches     = s_reaches;
    }
    if (ping) {
        ping_back(origin);
    }
    tell(now);
    return answer(req, "200 OK",
                  sealed(std::string(LINK) + REPLY,
                         pong_body(PANEL, esp_app_get_description()->version, reaches, now_ms())));
}

esp_err_t take(httpd_req_t *req)
{
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
    std::string plain;
    link::Nonce nonce{};
    Head        head;
    if (link::open(s_gcm, LINK, body, got, plain, nonce) != link::Opened::Ok ||
        !read_head(plain.data(), plain.size(), head)) {
        return answer(req, "403 Forbidden");  // nothing to say to whoever it was
    }
    if (now_ms() < CLOCK_SET_MS || !s_fresh.take(nonce, head.at_ms, now_ms())) {
        ESP_LOGW(TAG, "a stale %s, or one seen before", head.type.c_str());
        return answer(req, "403 Forbidden");
    }
    if (head.type == "state") {
        return take_state(req, plain);
    }
    if (head.type == "claude") {
        const std::string event = claude_event(plain.data(), plain.size());
        if (!event.empty() && s_on.claude != nullptr) {
            s_on.claude(event.data(), event.size());
        }
        return answer(req, "200 OK", sealed(std::string(LINK) + REPLY, ok_body(now_ms())));
    }
    return answer(req, "400 Bad Request");
}

void check_quiet(void *)
{
    {
        std::lock_guard<std::mutex> hold(s_lock);
        if (s_origin.empty() || esp_timer_get_time() - s_heard_us < QUIET_US) {
            return;
        }
        ESP_LOGI(TAG, "%s went quiet", s_now.machine.c_str());
        s_now    = NowPlaying{};
        s_origin = "";
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
    const std::string body = command_body(command, value, now_ms());
    net::Request      request;
    request.host        = host_of(origin);
    request.path        = LINK;
    request.method      = net::Method::Post;
    request.body        = sealed(LINK, body);
    request.priority    = net::Priority::Tap;
    request.deadline_ms = COMMAND_DEADLINE_MS;
    request.what        = "laptop command";
    // A slider dragged along sends where it stopped, not every level it passed.
    if (command == Command::Seek || command == Command::Volume) {
        request.key    = command == Command::Seek ? "seek" : "volume";
        request.dedupe = net::Dedupe::Replace;
    }
    request.done = [body, origin](const net::Response &answer) {
        if (answer.ok() && !opened(LINK, answer.body, answer.length, "ok").empty()) {
            ESP_LOGI(TAG, "sent %s", body.c_str());
            reached(origin, true);
        } else if (answer.outcome != net::Outcome::Replaced) {
            ESP_LOGW(TAG, "%s not taken: %s, http %d", body.c_str(), net::outcome_name(answer.outcome),
                     answer.status);
        }
    };
    net::submit(std::move(request));
}
}  // namespace

esp_err_t start(Handlers handlers)
{
    httpd_handle_t server = ota::server();
    ESP_RETURN_ON_FALSE(server != nullptr, ESP_ERR_INVALID_STATE, TAG, "no server");
    if (!s_gcm.start(DESK_LINK_KEY)) {
        ESP_LOGW(TAG, "no DESK_LINK_KEY of 64 hex digits: the laptops are refused");
    }
    s_on = handlers;
    const esp_timer_create_args_t quiet = {
        .callback              = check_quiet,
        .arg                   = nullptr,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "laptop",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&quiet, &s_quiet), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_quiet, CHECK_EVERY_US), TAG, "timer");
    const httpd_uri_t route = {.uri = LINK, .method = HTTP_POST, .handler = take, .user_ctx = nullptr};
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

void step_volume(bool up)
{
    send(up ? Command::VolumeUp : Command::VolumeDown);
}

std::string art_url(const NowPlaying &now)
{
    return now.art.empty() ? "" : std::string(kCoverScheme) + now.art;
}

std::size_t fetch_cover(const char *url, std::uint8_t *into, std::size_t size)
{
    const std::size_t scheme = std::strlen(kCoverScheme);
    if (std::strncmp(url, kCoverScheme, scheme) != 0) {
        return 0;
    }
    std::string origin;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        origin = s_origin;
    }
    if (origin.empty()) {
        return 0;
    }
    // Sealed, it is a little more than the picture: a buffer of its own.
    static char *answer = static_cast<char *>(heap_caps_malloc(COVER_MAX, MALLOC_CAP_SPIRAM));
    if (answer == nullptr) {
        return 0;
    }
    net::Request request;
    request.host     = host_of(origin);
    request.path     = COVER;
    request.method   = net::Method::Post;
    request.body     = sealed(COVER, cover_body(url + scheme, now_ms()));
    request.priority = net::Priority::Now;
    request.max_body = COVER_MAX - 1;
    request.what     = "laptop cover";
    const std::int64_t asked = esp_timer_get_time();
    const net::Fetched got   = net::fetch(std::move(request), answer, COVER_MAX);
    if (!got.ok() || got.truncated) {
        ESP_LOGW(TAG, "cover not had: http %d", got.status);
        return 0;
    }
    ESP_LOGI(TAG, "cover of %u KB in %d ms, %d of them on the wire", static_cast<unsigned>(got.length / 1024),
             static_cast<int>((esp_timer_get_time() - asked) / units::kUsPerMs), got.ms);
    const std::string picture = opened(COVER, answer, got.length, nullptr);
    if (picture.empty() || picture.size() > size) {
        ESP_LOGW(TAG, "cover %s", picture.empty() ? "would not open" : "too large");
        return 0;
    }
    std::memcpy(into, picture.data(), picture.size());
    reached(origin, true);
    return picture.size();
}
}  // namespace laptop
