// net for the simulator: the panel's own core deciding what goes when, as
// components/net/net.cpp does, with curl on threads in place of the ESP client.
// And only to the read-only feeds the simulator may reach: anything else, and
// anything but a read, is refused, so a stray address cannot reach Home
// Assistant, the desk or Jellyfin from the desktop.
#include "net.h"

#include "net_core.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

#if __has_include("ical_secrets.h")
#include "ical_secrets.h"
#endif
#ifndef ICAL_WORK_URL
#define ICAL_WORK_URL ""
#endif
#if __has_include("travel_secrets.h")
#include "travel_secrets.h"
#endif
#ifndef TRAVEL_HOST
#define TRAVEL_HOST ""
#endif

namespace net {
namespace {
constexpr int WORKERS = 3;

// Where the simulator may go, and nowhere else: the calendar, the way there
// and the sky. A leading dot is a domain and what is under it.
const std::vector<std::string> &allowed()
{
    static const std::vector<std::string> list = [] {
        std::vector<std::string> out = {
            "https://calendar.example.org",       // the timetable's feeds
            "https://api.adsb.lol",                  // the sky
            "https://opendata.adsb.fi",
            "https://adsb.lol",                      // where a plane has been
            "https://api.adsbdb.com",                // who it is
            "https://api.planespotters.net",         // its photo's address
            ".plnspttrs.net",                        // and the photo
        };
        for (const char *url : {ICAL_WORK_URL, TRAVEL_HOST}) {
            if (url[0] != '\0') {
                const std::string text  = url;
                const std::size_t start = text.find("://");
                out.push_back(start == std::string::npos ? text : text.substr(0, text.find('/', start + 3)));
            }
        }
        return out;
    }();
    return list;
}

std::string origin_of(const std::string &url)
{
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    return url.substr(0, url.find('/', scheme + 3));
}

bool may_reach(const std::string &url)
{
    const std::string origin = origin_of(url);
    const std::size_t scheme = origin.find("://");
    const std::string domain = scheme == std::string::npos ? origin : origin.substr(scheme + 3);
    for (const std::string &entry : allowed()) {
        if (entry[0] == '.') {
            if (domain.size() > entry.size() && domain.compare(domain.size() - entry.size(), entry.size(), entry) == 0) {
                return true;
            }
        } else if (origin == entry) {
            return true;
        }
    }
    return false;
}

std::int64_t now_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// Used by the workers, which are detached and outlive main: never destroyed,
// as glibc waits for ever to destroy a condition variable a thread waits on.
Core                    &s_core = *new Core;
std::mutex              &s_lock = *new std::mutex;
std::condition_variable &s_wake = *new std::condition_variable;
bool                    s_started = false;

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

struct Received {
    std::string body;
    std::size_t limit          = 0;
    int         retry_after_ms = 0;
};

std::size_t take_body(char *data, std::size_t size, std::size_t count, void *into)
{
    auto             &got  = *static_cast<Received *>(into);
    const std::size_t more = size * count;
    got.body.append(data, std::min(more, got.limit > got.body.size() ? got.limit - got.body.size() : 0));
    return more;
}

std::size_t take_header(char *data, std::size_t size, std::size_t count, void *into)
{
    const std::size_t len = size * count;
    constexpr char    KEY[] = "Retry-After:";
    if (len > sizeof(KEY) - 1 && strncasecmp(data, KEY, sizeof(KEY) - 1) == 0) {
        static_cast<Received *>(into)->retry_after_ms = std::atoi(data + sizeof(KEY) - 1) * 1000;
    }
    return len;
}

Exchange send(const HostConfig &config, const Running &running, Received &got)
{
    const Request    &request = running.request;
    const std::string url =
        request.path.rfind("http", 0) == 0 ? request.path : std::string(config.base) + request.path;
    Exchange out;
    if (request.method != Method::Get || !may_reach(url)) {
        std::printf("W (net) refused %s %s: not a feed the simulator may read\n",
                    request.method == Method::Get ? "GET" : "POST", origin_of(url).c_str());
        out.error = -1;
        return out;
    }
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        out.error = -1;
        return out;
    }
    curl_slist *headers = nullptr;
    for (const char *line = config.headers; line != nullptr && *line != '\0';) {
        const char *end = std::strchr(line, '\n');
        headers         = curl_slist_append(headers, std::string(line, end != nullptr ? end : line + std::strlen(line)).c_str());
        line            = end != nullptr ? end + 1 : nullptr;
    }
    got.limit = request.max_body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // whatever comes compressed is inflated
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config.timeout_ms));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, config.agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, take_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &got);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, take_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &got);
    const std::int64_t began  = now_us();
    const CURLcode     result = curl_easy_perform(curl);
    out.ms                    = static_cast<int>((now_us() - began) / 1000);
    if (result == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out.status = static_cast<int>(status);
    } else {
        out.error     = static_cast<int>(result);
        out.timed_out = result == CURLE_OPERATION_TIMEDOUT;
    }
    out.retry_after_ms = got.retry_after_ms;
    out.body           = got.body.c_str();
    out.length         = got.body.size();
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return out;
}

void worker()
{
    for (;;) {
        Tells      tells;
        Running    running;
        HostConfig config;
        bool       took = false;
        {
            std::unique_lock<std::mutex> hold(s_lock);
            took = s_core.next(now_us(), true, true, running, tells);
            if (took) {
                config = s_core.config(running.host);
            } else if (tells.empty()) {
                // Woken by a request, or soon for one waiting on a delay or a rest.
                s_wake.wait_for(hold, std::chrono::milliseconds(s_core.waiting() > 0 ? 100 : 1000));
                continue;
            }
        }
        tell(tells);
        if (!took) {
            continue;
        }
        Received       got;
        const Exchange exchange = send(config, running, got);
        {
            std::lock_guard<std::mutex> hold(s_lock);
            s_core.finish(running, exchange, now_us(), tells);
        }
        tell(tells);  // while the body is still in `got`
        s_wake.notify_all();
    }
}

std::vector<std::unique_ptr<std::string>> s_origins;  // what the core points at
std::vector<Host>                         s_origin_hosts;
}  // namespace

esp_err_t start()
{
    std::lock_guard<std::mutex> hold(s_lock);
    if (!s_started) {
        s_started = true;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        for (int i = 0; i < WORKERS; ++i) {
            std::thread(worker).detach();
        }
    }
    return ESP_OK;
}

Host add_host(const HostConfig &config)
{
    std::lock_guard<std::mutex> hold(s_lock);
    return s_core.add_host(config);
}

Host host_for(const std::string &url, const HostConfig &like)
{
    const std::string origin = origin_of(url);
    if (origin.empty()) {
        return kNoHost;
    }
    std::lock_guard<std::mutex> hold(s_lock);
    for (std::size_t i = 0; i < s_origins.size(); ++i) {
        if (*s_origins[i] == origin) {
            return s_origin_hosts[i];
        }
    }
    s_origins.push_back(std::make_unique<std::string>(origin));
    HostConfig config = like;
    config.base       = s_origins.back()->c_str();
    config.name       = config.base;
    const Host host   = s_core.add_host(config);
    s_origin_hosts.push_back(host);
    return host;
}

Ticket submit(Request request)
{
    Tells  tells;
    Ticket ticket = kNoTicket;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        ticket = s_core.submit(std::move(request), now_us(), tells);
    }
    tell(tells);
    s_wake.notify_all();
    return ticket;
}

namespace {
Fetched wait_for(Request request, char *into, std::size_t size, std::string *text)
{
    if (request.deadline_ms <= 0) {
        request.deadline_ms = kFetchDeadlineMs;
    }
    const std::size_t       text_max = request.max_body;
    std::mutex              lock;
    std::condition_variable answered;
    bool                    done = false;
    Fetched                 got;
    request.done = [&](const Response &answer) {
        std::lock_guard<std::mutex> hold(lock);
        got.outcome = answer.outcome;
        got.status  = answer.status;
        got.error   = answer.error;
        got.ms      = answer.ms;
        if (into != nullptr && size > 0) {
            got.length    = std::min(answer.length, size - 1);
            got.truncated = got.length < answer.length;
            std::memcpy(into, answer.body, got.length);
            into[got.length] = '\0';
        } else if (text != nullptr) {
            got.length    = std::min(answer.length, text_max);
            got.truncated = got.length < answer.length;
            text->assign(answer.body, got.length);
        }
        done = true;
        answered.notify_one();
    };
    submit(std::move(request));
    std::unique_lock<std::mutex> hold(lock);
    answered.wait(hold, [&] { return done; });  // done always comes, the deadline at the latest
    return got;
}
}  // namespace

Fetched fetch(Request request, char *into, std::size_t size)
{
    return wait_for(std::move(request), into, size, nullptr);
}

Fetched fetch(Request request, std::string &into)
{
    into.clear();
    return wait_for(std::move(request), nullptr, 0, &into);
}

void cancel(Ticket ticket)
{
    Tells tells;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        s_core.cancel(ticket, tells);
    }
    tell(tells);
}

void cancel(Host host, const std::string &key)
{
    Tells tells;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        s_core.cancel(host, key, tells);
    }
    tell(tells);
}

HostStatus status(Host host)
{
    std::lock_guard<std::mutex> hold(s_lock);
    return s_core.status(host, now_us());
}

bool resting(Host host)
{
    return status(host).resting;
}

}  // namespace net
