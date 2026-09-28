// The panel's five feeds, fetched as ical.cpp fetches them and read by the same
// parser. A feed that fails keeps what it had, as on the panel.
#include "feeds.h"
#include "http.h"
#include "ical.h"

#if __has_include("ical_secrets.h")
#include "ical_secrets.h"
#endif
#ifndef ICAL_WORK_URL
#define ICAL_WORK_URL ""
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>

namespace ical {
namespace {
constexpr char HOST[] = "https://calendar.example.org";

struct Feed {
    const char *name;
    const char *url;   // null: HOST/name
    const char *keep;  // when set, only events whose summary starts with it
};
constexpr Feed FEEDS[kFeedCount] = {
    {"Lectures", nullptr, nullptr},
    {"Practicals", nullptr, nullptr},
    {"Exams", nullptr, nullptr},
    {"Other", nullptr, nullptr},
    {"Work", ICAL_WORK_URL, "werk"},
};

std::mutex         s_lock;
std::vector<Event> s_events;

bool starts_with(const char *text, const char *prefix)
{
    for (; *prefix != '\0'; ++text, ++prefix) {
        if (std::tolower(static_cast<unsigned char>(*text)) != *prefix) {
            return false;
        }
    }
    return true;
}

int fetch_feed(int index, std::vector<Event> &into)
{
    const Feed &feed = FEEDS[index];
    if (feed.url != nullptr && feed.url[0] == '\0') {
        return -1;
    }
    const std::string url = feed.url != nullptr ? std::string(feed.url) : std::string(HOST) + "/" + feed.name;
    live::Answer      got = live::get(url);
    if (got.status != 200) {
        std::printf("W (ical) %s: %d\n", feed.name, got.status);
        return -1;
    }
    static Event scratch[kMaxEvents];
    int          count = parse(got.body.data(), got.body.size(), static_cast<std::uint8_t>(index), scratch, kMaxEvents);
    int          kept  = 0;
    for (int i = 0; i < count; ++i) {
        if (feed.keep == nullptr || starts_with(scratch[i].summary, feed.keep)) {
            into.push_back(scratch[i]);
            ++kept;
        }
    }
    std::printf("I (ical) %s: %d events\n", feed.name, kept);
    return kept;
}

}  // namespace

int upcoming(Event *out, int capacity)
{
    const auto                  now = static_cast<std::int64_t>(std::time(nullptr));
    std::lock_guard<std::mutex> hold(s_lock);
    int                         count = 0;
    for (const Event &e : s_events) {
        if (e.end > now && count < capacity) {
            out[count++] = e;
        }
    }
    return count;
}

int between(std::int64_t from, std::int64_t to, Event *out, int capacity)
{
    std::lock_guard<std::mutex> hold(s_lock);
    int                         count = 0;
    for (const Event &e : s_events) {
        if (e.start < to && e.end > from && count < capacity) {
            out[count++] = e;
        }
    }
    return count;
}
}  // namespace ical

namespace live {
bool fetch_calendar()
{
    std::vector<ical::Event> all;
    bool                     every = true;
    for (int i = 0; i < ical::kFeedCount; ++i) {
        if (ical::fetch_feed(i, all) < 0) {
            every = false;
            std::lock_guard<std::mutex> hold(ical::s_lock);
            for (const ical::Event &e : ical::s_events) {
                if (e.feed == i) {
                    all.push_back(e);
                }
            }
        }
    }
    std::sort(all.begin(), all.end(), [](const ical::Event &a, const ical::Event &b) { return a.start < b.start; });
    std::lock_guard<std::mutex> hold(ical::s_lock);
    ical::s_events.swap(all);
    return every;
}
}  // namespace live
