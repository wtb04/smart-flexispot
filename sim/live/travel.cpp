// The way to the next appointment, asked of the travel service as travel.cpp asks
// it, for what network.cpp's ask_journey() would ask, and read by the same parser.
#include "feeds.h"
#include "http.h"
#include "ical.h"
#include "travel.h"
#include "ui.h"

#if __has_include("travel_secrets.h")
#include "travel_secrets.h"
#endif
#ifndef TRAVEL_HOST
#define TRAVEL_HOST ""
#endif
#ifndef TRAVEL_API_KEY
#define TRAVEL_API_KEY ""
#endif

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace travel {
namespace {
std::mutex s_lock;
Option     s_options[kOptionsMax];
int        s_count = 0;
bool       s_ok    = false;
}  // namespace

int options(Option *out, int capacity)
{
    std::lock_guard<std::mutex> hold(s_lock);
    const int                   count = s_count < capacity ? s_count : capacity;
    for (int i = 0; i < count; ++i) {
        out[i] = s_options[i];
    }
    return count;
}

bool ok()
{
    std::lock_guard<std::mutex> hold(s_lock);
    return s_ok;
}
}  // namespace travel

namespace live {
bool fetch_journey()
{
    ical::Event next[1];
    const int   count = ical::upcoming(next, 1);
    const auto  now   = static_cast<std::int64_t>(std::time(nullptr));
    const bool  soon  = count > 0 && next[0].start > now && next[0].start - now < ui::kJourneyAhead;
    if (!soon || TRAVEL_HOST[0] == '\0') {
        std::lock_guard<std::mutex> hold(travel::s_lock);
        travel::s_count = 0;
        return false;
    }
    char url[176];
    std::snprintf(url, sizeof(url), "%s/v1/leave?arriveBy=%lld%s", TRAVEL_HOST,
                  static_cast<long long>(next[0].start), next[0].feed == ical::kWorkFeed ? "&to=work" : "");
    std::vector<std::string> headers;
    if (TRAVEL_API_KEY[0] != '\0') {
        headers.push_back(std::string("X-Api-Key: ") + TRAVEL_API_KEY);
    }
    const Answer   got = get(url, headers);
    travel::Option found[travel::kOptionsMax];
    const int      n = got.status == 200 ? travel::parse(got.body.data(), got.body.size(), found, travel::kOptionsMax) : 0;
    std::printf("%s (travel) %d, %d ways\n", got.status == 200 ? "I" : "W", got.status, n);

    std::lock_guard<std::mutex> hold(travel::s_lock);
    travel::s_ok = got.status == 200;
    if (got.status == 200) {
        for (int i = 0; i < n; ++i) {
            travel::s_options[i] = found[i];
        }
        travel::s_count = n;
    }
    return travel::s_ok;
}
}  // namespace live
