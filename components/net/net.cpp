#include "net.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "miniz.h"
#include "net_core.h"
#include "units.h"
#include "wifi.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <strings.h>

// The panel's side of net: workers that drive the core, and the connections
// they send on. What goes when is the core's, in net_core.cpp.
namespace net {
namespace {
constexpr char TAG[] = "net";

constexpr int           WORKERS         = 3;
constexpr std::uint32_t WORKER_STACK    = 10240;  // a TLS handshake, and what a callback parses
constexpr UBaseType_t   WORKER_PRIORITY = 3;
constexpr BaseType_t    WORKER_CORE     = 0;      // LVGL has the other
constexpr TickType_t    IDLE_WAIT       = pdMS_TO_TICKS(units::kMsPerSecond);
constexpr TickType_t    BUSY_WAIT       = pdMS_TO_TICKS(100);  // something waits on a delay or a rest
constexpr int           MAX_HOSTS       = 24;
constexpr int           MAX_CONNECTIONS = 4;
constexpr int           HTTP_BUFFER     = 2 * units::kBytesPerKiB;
constexpr UBaseType_t   WAKE_MAX        = 32;
constexpr std::size_t   FIRST_RESERVE   = 4 * units::kBytesPerKiB;
constexpr int           NOT_FOUND       = 404;

// Grown to what a request needs, and kept, in PSRAM. Never by realloc: the
// heap copies the old contents with its lock held and interrupts off, and a
// buffer of a hundred kilobytes and more cost the panel a frame, a flicker of
// blue. A fresh block, and the copy made here, with interrupts on.
struct Buffer {
    char       *data  = nullptr;
    std::size_t size  = 0;
    std::size_t len   = 0;
    std::size_t limit = 0;  // what this request allows

    bool reserve(std::size_t want, bool keep = true)
    {
        if (want <= size) {
            return true;
        }
        auto *grown = static_cast<char *>(heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (grown == nullptr) {
            return false;
        }
        if (keep && data != nullptr && len > 0) {
            std::memcpy(grown, data, len + 1);
        }
        heap_caps_free(data);
        data = grown;
        size = want;
        return true;
    }
};

struct Connection {
    esp_http_client_handle_t client         = nullptr;
    bool                     busy           = false;
    bool                     open           = false;  // a socket may be held
    std::int64_t             used_us        = 0;
    Buffer                  *into           = nullptr;
    int                      retry_after_ms = 0;
};

Core              s_core;
Connection        s_connections[MAX_HOSTS][MAX_CONNECTIONS];
SemaphoreHandle_t s_lock      = nullptr;
SemaphoreHandle_t s_wake      = nullptr;
bool              s_screen_on = true;

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

void tell(Tells &tells)
{
    for (Tell &told : tells) {
        for (Done &done : told.done) {
            if (done) {
                done(told.response);
            }
        }
    }
    tells.clear();
}

esp_err_t on_event(esp_http_client_event_t *event)
{
    auto *connection = static_cast<Connection *>(event->user_data);
    if (connection == nullptr) {
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(event->header_key, "Retry-After") == 0) {
        connection->retry_after_ms = std::atoi(event->header_value) * units::kMsPerSecond;  // seconds; a date is ignored
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 || connection->into == nullptr) {
        return ESP_OK;
    }
    Buffer           &buffer = *connection->into;
    const std::size_t room   = buffer.limit > buffer.len ? buffer.limit - buffer.len : 0;
    const std::size_t take   = std::min(static_cast<std::size_t>(event->data_len), room);
    const std::size_t grown  = std::max(buffer.len + take + 1, std::min(buffer.limit + 1, buffer.size * 2));
    if (take > 0 && buffer.reserve(grown)) {
        std::memcpy(buffer.data + buffer.len, event->data, take);
        buffer.len += take;
        buffer.data[buffer.len] = '\0';
    }
    return ESP_OK;
}

std::string url_of(const HostConfig &config, const std::string &path)
{
    return path.rfind("http", 0) == 0 ? path : std::string(config.base) + path;
}

void add_headers(esp_http_client_handle_t client, const char *headers)
{
    const char *line = headers;
    while (line != nullptr && *line != '\0') {
        const char       *end = std::strchr(line, '\n');
        const std::string text(line, end != nullptr ? end : line + std::strlen(line));
        const std::size_t colon = text.find(':');
        if (colon != std::string::npos) {
            std::string value = text.substr(colon + 1);
            value.erase(0, value.find_first_not_of(' '));
            esp_http_client_set_header(client, text.substr(0, colon).c_str(), value.c_str());
        }
        line = end != nullptr ? end + 1 : nullptr;
    }
}

esp_http_client_handle_t open_client(const HostConfig &config, Connection &connection, const std::string &url)
{
    esp_http_client_config_t cfg{};
    cfg.url               = url.c_str();
    cfg.event_handler     = on_event;
    cfg.user_data         = &connection;
    cfg.timeout_ms        = config.timeout_ms;
    cfg.user_agent        = config.agent;
    cfg.buffer_size       = HTTP_BUFFER;
    cfg.keep_alive_enable = config.keep_open;
    if (url.rfind("https://", 0) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client != nullptr) {
        add_headers(client, config.headers);
        if (config.gzip) {
            esp_http_client_set_header(client, "Accept-Encoding", "gzip");
        }
    }
    return client;
}

// Inflated into `out` when `in` is a whole gzip stream; false otherwise.
bool gunzip(const Buffer &in, Buffer &out, tinfl_decompressor *state)
{
    constexpr std::size_t  HEADER = 10, TRAILER = 8;
    constexpr std::uint8_t FHCRC = 2, FEXTRA = 4, FNAME = 8, FCOMMENT = 16;
    const auto            *bytes = reinterpret_cast<const std::uint8_t *>(in.data);
    if (in.len < HEADER + TRAILER || bytes[0] != 0x1f || bytes[1] != 0x8b || bytes[2] != 8) {
        return false;
    }
    std::size_t at = HEADER;
    if ((bytes[3] & FEXTRA) != 0) {
        at += 2 + (bytes[at] | (bytes[at + 1] << 8));
    }
    for (const std::uint8_t text : {FNAME, FCOMMENT}) {
        if ((bytes[3] & text) != 0) {
            while (at < in.len && bytes[at] != 0) {
                ++at;
            }
            ++at;
        }
    }
    if ((bytes[3] & FHCRC) != 0) {
        at += 2;
    }
    if (at + TRAILER >= in.len) {
        return false;
    }
    // The trailer says how long it comes to, so the buffer is made that size.
    const std::uint8_t *tail  = bytes + in.len - 4;
    const std::size_t   whole = tail[0] | (tail[1] << 8) | (tail[2] << 16) | (static_cast<std::size_t>(tail[3]) << 24);
    if (!out.reserve(whole + 1, false)) {
        return false;
    }
    std::size_t in_bytes  = in.len - at - TRAILER;
    std::size_t out_bytes = whole;
    tinfl_init(state);
    const tinfl_status status =
        tinfl_decompress(state, bytes + at, &in_bytes, reinterpret_cast<mz_uint8 *>(out.data),
                         reinterpret_cast<mz_uint8 *>(out.data), &out_bytes, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE) {
        return false;
    }
    out.len           = out_bytes;
    out.data[out.len] = '\0';
    return true;
}

// What each worker keeps: where answers come in, and where they are inflated.
struct Worker {
    Buffer              body;
    Buffer              inflated;
    tinfl_decompressor *inflater = nullptr;
};

bool timed_out(esp_err_t err)
{
    return err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_TIMEOUT;
}

void send_once(const HostConfig &config, Connection &connection, const Request &request, Worker &worker,
               Exchange &out)
{
    const std::string url = url_of(config, request.path);
    if (connection.client == nullptr) {
        connection.client = open_client(config, connection, url);
    }
    if (connection.client == nullptr) {
        out.error = ESP_ERR_NO_MEM;
        return;
    }
    esp_http_client_set_url(connection.client, url.c_str());
    if (request.method == Method::Post) {
        esp_http_client_set_method(connection.client, HTTP_METHOD_POST);
        if (request.body.empty()) {
            // Null rather than empty: an empty body is given a form's type.
            esp_http_client_delete_header(connection.client, "Content-Type");
            esp_http_client_set_post_field(connection.client, nullptr, 0);
        } else {
            esp_http_client_set_header(connection.client, "Content-Type", "application/json");
            esp_http_client_set_post_field(connection.client, request.body.data(),
                                           static_cast<int>(request.body.size()));
        }
    } else {
        esp_http_client_set_method(connection.client, HTTP_METHOD_GET);
        esp_http_client_set_post_field(connection.client, nullptr, 0);
    }
    worker.body.len   = 0;
    worker.body.limit = request.max_body;
    worker.body.reserve(std::min(request.max_body + 1, FIRST_RESERVE));
    if (worker.body.data != nullptr) {
        worker.body.data[0] = '\0';
    }
    connection.into           = &worker.body;
    connection.retry_after_ms = 0;

    const std::int64_t began = esp_timer_get_time();
    const esp_err_t    err   = esp_http_client_perform(connection.client);
    out.ms                   = static_cast<int>((esp_timer_get_time() - began) / units::kUsPerMs);
    out.error                = err == ESP_OK ? 0 : err;
    out.timed_out            = timed_out(err);
    out.status               = err == ESP_OK ? esp_http_client_get_status_code(connection.client) : 0;
    out.retry_after_ms       = connection.retry_after_ms;
    connection.into          = nullptr;
    connection.used_us       = esp_timer_get_time();
    connection.open          = err == ESP_OK && config.keep_open;
    if (!connection.open) {
        esp_http_client_close(connection.client);
    }
}

Exchange send(const HostConfig &config, Connection &connection, const Running &running, Worker &worker)
{
    Exchange   exchange;
    const bool was_open = connection.open;
    send_once(config, connection, running.request, worker, exchange);
    // A kept connection the server has since closed fails at once: once more,
    // on a fresh one. Not after a timeout, which the server may have acted on.
    if (exchange.error != 0 && was_open && running.request.repeatable && !exchange.timed_out) {
        exchange = Exchange{};
        send_once(config, connection, running.request, worker, exchange);
    }
    const Buffer *answer = &worker.body;
    if (exchange.error == 0 && gunzip(worker.body, worker.inflated, worker.inflater)) {
        answer = &worker.inflated;
    }
    exchange.body   = answer->data != nullptr ? answer->data : "";
    exchange.length = answer->len;
    return exchange;
}

// Under the lock: connections left unused past their host's idle time closed.
void close_idle(std::int64_t now)
{
    for (int h = 0; h < s_core.host_count(); ++h) {
        const std::int64_t idle_us = s_core.config(h).idle_ms * units::kUsPerMs;
        for (Connection &connection : s_connections[h]) {
            if (!connection.busy && connection.open && now - connection.used_us > idle_us) {
                esp_http_client_close(connection.client);
                connection.open = false;
            }
        }
    }
}

void log_outcome(const HostConfig &config, const Running &running, const Exchange &exchange)
{
    if (exchange.error != 0) {
        ESP_LOGW(TAG, "%s %s: %s after %d ms, attempt %d", config.name, running.request.what,
                 esp_err_to_name(exchange.error), exchange.ms, running.attempt);
    } else if (exchange.status >= 400 && exchange.status != NOT_FOUND) {
        ESP_LOGW(TAG, "%s %s: http %d", config.name, running.request.what, exchange.status);
    }
}

[[noreturn]] void worker_task(void *arg)
{
    auto &worker = *static_cast<Worker *>(arg);
    bool  busy   = false;
    for (;;) {
        xSemaphoreTake(s_wake, busy ? BUSY_WAIT : IDLE_WAIT);

        Tells       tells;
        Running     running;
        HostConfig  config;
        Connection *connection = nullptr;
        bool        took       = false;
        {
            Lock               hold;
            const std::int64_t now = esp_timer_get_time();
            took = s_core.next(now, wifi::connected(), s_screen_on, running, tells);
            busy = s_core.waiting() > 0;
            close_idle(now);
            if (took) {
                config           = s_core.config(running.host);
                connection       = &s_connections[running.host][running.slot];
                connection->busy = true;
            }
        }
        tell(tells);
        if (!took) {
            continue;
        }

        const Exchange exchange = send(config, *connection, running, worker);
        log_outcome(config, running, exchange);
        {
            Lock hold;
            connection->busy = false;
            s_core.finish(running, exchange, esp_timer_get_time(), tells);
        }
        tell(tells);             // while the answer is still in this worker's buffers
        xSemaphoreGive(s_wake);  // a connection is free: another may go
    }
}

}  // namespace

esp_err_t start()
{
    if (s_lock != nullptr) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateCounting(WAKE_MAX, 0);
    ESP_RETURN_ON_FALSE(s_lock != nullptr && s_wake != nullptr, ESP_ERR_NO_MEM, TAG, "locks");
    for (int i = 0; i < WORKERS; ++i) {
        auto *worker     = new Worker();
        worker->inflater = static_cast<tinfl_decompressor *>(
            heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        auto *stack = static_cast<StackType_t *>(
            heap_caps_malloc(WORKER_STACK * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        auto *ctrl = static_cast<StaticTask_t *>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        ESP_RETURN_ON_FALSE(worker->inflater != nullptr && stack != nullptr && ctrl != nullptr, ESP_ERR_NO_MEM, TAG,
                            "worker");
        char name[8];
        std::snprintf(name, sizeof(name), "net%d", i);
        ESP_RETURN_ON_FALSE(xTaskCreateStaticPinnedToCore(worker_task, name, WORKER_STACK, worker, WORKER_PRIORITY,
                                                          stack, ctrl, WORKER_CORE) != nullptr,
                            ESP_ERR_NO_MEM, TAG, "worker task");
    }
    return ESP_OK;
}

Host add_host(const HostConfig &config)
{
    if (s_lock == nullptr) {
        ESP_LOGE(TAG, "%s added before net started", config.name);
        return kNoHost;
    }
    Lock hold;
    if (s_core.host_count() >= MAX_HOSTS) {
        ESP_LOGE(TAG, "no room for %s", config.name);
        return kNoHost;
    }
    HostConfig capped  = config;
    capped.connections = std::clamp(config.connections, 1, MAX_CONNECTIONS);
    return s_core.add_host(capped);
}

namespace {
// What add_host() was given for hosts found by host_for(): the core keeps
// only pointers to it.
std::vector<std::unique_ptr<std::string>> s_origins;
std::vector<Host>                         s_origin_hosts;

std::string origin_of(const std::string &url)
{
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    const std::size_t path = url.find('/', scheme + 3);
    return url.substr(0, path);
}
}  // namespace

Host host_for(const std::string &url, const HostConfig &like)
{
    const std::string origin = origin_of(url);
    if (origin.empty() || s_lock == nullptr) {
        return kNoHost;
    }
    {
        Lock hold;
        for (std::size_t i = 0; i < s_origins.size(); ++i) {
            if (*s_origins[i] == origin) {
                return s_origin_hosts[i];
            }
        }
        s_origins.push_back(std::make_unique<std::string>(origin));
    }
    HostConfig config = like;
    config.base       = s_origins.back()->c_str();
    config.name       = config.base;
    const Host host   = add_host(config);
    Lock hold;
    s_origin_hosts.push_back(host);
    return host;
}

namespace {
struct Waiting {
    SemaphoreHandle_t done;
    Fetched           got;
    char             *into;
    std::size_t       size;
    std::string      *text;
    std::size_t       text_max;
};

Fetched wait_for(Request request, Waiting &waiting)
{
    if (request.deadline_ms <= 0) {
        request.deadline_ms = kFetchDeadlineMs;
    }
    request.done = [&waiting](const Response &answer) {
        Fetched &got = waiting.got;
        got.outcome  = answer.outcome;
        got.status   = answer.status;
        got.error    = answer.error;
        got.ms       = answer.ms;
        if (waiting.into != nullptr && waiting.size > 0) {
            got.length    = std::min(answer.length, waiting.size - 1);
            got.truncated = got.length < answer.length;
            std::memcpy(waiting.into, answer.body, got.length);
            waiting.into[got.length] = '\0';
        } else if (waiting.text != nullptr) {
            got.length    = std::min(answer.length, waiting.text_max);
            got.truncated = got.length < answer.length;
            waiting.text->assign(answer.body, got.length);
        }
        xSemaphoreGive(waiting.done);
    };
    submit(std::move(request));
    xSemaphoreTake(waiting.done, portMAX_DELAY);  // done always comes, the deadline at the latest
    vSemaphoreDelete(waiting.done);
    return waiting.got;
}
}  // namespace

Fetched fetch(Request request, char *into, std::size_t size)
{
    Waiting waiting{xSemaphoreCreateBinary(), {}, into, size, nullptr, 0};
    if (waiting.done == nullptr) {
        return {};
    }
    return wait_for(std::move(request), waiting);
}

Fetched fetch(Request request, std::string &into)
{
    const std::size_t max = request.max_body;
    Waiting           waiting{xSemaphoreCreateBinary(), {}, nullptr, 0, &into, max};
    if (waiting.done == nullptr) {
        return {};
    }
    into.clear();
    return wait_for(std::move(request), waiting);
}

Ticket submit(Request request)
{
    Tells  tells;
    Ticket ticket = kNoTicket;
    if (s_lock == nullptr) {
        Tell told;
        told.done.push_back(std::move(request.done));
        tells.push_back(std::move(told));
    } else {
        Lock hold;
        ticket = s_core.submit(std::move(request), esp_timer_get_time(), tells);
    }
    // Those it replaced are told now, on the caller's task: they were only waiting.
    tell(tells);
    if (s_wake != nullptr) {
        xSemaphoreGive(s_wake);
    }
    return ticket;
}

void cancel(Ticket ticket)
{
    if (s_lock == nullptr) {
        return;
    }
    Tells tells;
    {
        Lock hold;
        s_core.cancel(ticket, tells);
    }
    tell(tells);
}

void cancel(Host host, const std::string &key)
{
    if (s_lock == nullptr) {
        return;
    }
    Tells tells;
    {
        Lock hold;
        s_core.cancel(host, key, tells);
    }
    tell(tells);
}

HostStatus status(Host host)
{
    if (s_lock == nullptr) {
        return {};
    }
    Lock hold;
    return s_core.status(host, esp_timer_get_time());
}

bool resting(Host host)
{
    return status(host).resting;
}

void set_screen(bool on)
{
    if (s_lock == nullptr) {
        return;
    }
    {
        Lock hold;
        s_screen_on = on;
    }
    xSemaphoreGive(s_wake);
}

}  // namespace net
