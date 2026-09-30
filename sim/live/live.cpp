#include "live.h"

#include "feeds.h"
#include "ui.h"

#include "net.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace live {
namespace {
using Clock = std::chrono::steady_clock;
using std::chrono::minutes;
using std::chrono::seconds;

// As the panel paces them: the calendar every half hour, the way there every
// few minutes while it matters, the sky every five seconds on screen and every
// minute off it, and longer after the feed has asked to be left alone.
constexpr auto CALENDAR_EVERY  = minutes(30);
constexpr auto JOURNEY_EVERY   = minutes(3);
constexpr auto SKY_SHOWING     = seconds(5);
constexpr auto SKY_HIDDEN      = minutes(1);
constexpr auto SKY_BUSY        = seconds(30);

std::atomic<bool> s_calendar_changed{false};
std::atomic<bool> s_sky_changed{false};
std::atomic<bool> s_radar_showing{false};
std::atomic<bool> s_sky_now{false};  // the page came up: fetch without waiting

std::mutex              s_wake_lock;
std::condition_variable s_wake;

// One lookup at a time, the last tap winning, as on the panel.
std::mutex                 s_lookup_lock;
std::condition_variable    s_lookup_wake;
std::string                s_lookup_hex, s_lookup_callsign;
bool                       s_lookup_wanted = false;
bool                       s_details_ready = false;
bool                       s_photo_ready   = false;
std::string                s_found_hex;
radar::Details             s_details{};
std::vector<std::uint16_t> s_photo[2];  // shown while the next is fetched into the other
int                        s_photo_shown = 0;
int                        s_photo_w = 0, s_photo_h = 0;

void feeds()
{
    Clock::time_point calendar_due{}, journey_due{}, sky_due{};
    for (;;) {
        const auto now = Clock::now();
        if (now >= calendar_due) {
            fetch_calendar();
            calendar_due = now + CALENDAR_EVERY;
            journey_due  = now;  // the next appointment may have changed
            s_calendar_changed = true;
        }
        if (now >= journey_due) {
            fetch_journey();
            journey_due        = now + JOURNEY_EVERY;
            s_calendar_changed = true;
        }
        if (s_sky_now.exchange(false)) {
            sky_due = now;
        }
        if (now >= sky_due) {
            bool busy = false;
            if (fetch_sky(busy)) {
                s_sky_changed = true;
            }
            sky_due = now + (busy ? SKY_BUSY : s_radar_showing ? SKY_SHOWING : SKY_HIDDEN);
        }
        std::unique_lock<std::mutex> hold(s_wake_lock);
        s_wake.wait_until(hold, std::min({calendar_due, journey_due, sky_due}), [] { return s_sky_now.load(); });
    }
}

void lookups()
{
    for (;;) {
        std::string hex, callsign;
        {
            std::unique_lock<std::mutex> hold(s_lookup_lock);
            s_lookup_wake.wait(hold, [] { return s_lookup_wanted; });
            s_lookup_wanted = false;
            hex             = s_lookup_hex;
            callsign        = s_lookup_callsign;
        }
        radar::Details details{};
        fetch_details(hex.c_str(), callsign.c_str(), details);
        details.photo_checked = true;
        {
            std::lock_guard<std::mutex> hold(s_lookup_lock);
            s_found_hex     = hex;
            s_details       = details;
            s_details_ready = true;
        }
        if (fetch_trace(hex.c_str())) {
            s_sky_changed = true;
        }
        int  w = 0, h = 0;
        auto pixels = fetch_photo(hex.c_str(), w, h);
        std::lock_guard<std::mutex> hold(s_lookup_lock);
        if (s_found_hex == hex) {
            s_photo[1 - s_photo_shown] = std::move(pixels);
            s_photo_w                  = w;
            s_photo_h                  = h;
            s_photo_ready              = true;
        }
    }
}
}  // namespace

void init_http()
{
    net::start();
}

void start()
{
    std::thread(feeds).detach();
    std::thread(lookups).detach();
}

void pump()
{
    if (s_calendar_changed.exchange(false)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_calendar());
    }
    if (s_sky_changed.exchange(false)) {
        static radar::Snapshot snapshot;  // too large for the stack
        radar::snapshot(snapshot);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_radar(snapshot));
    }
    std::lock_guard<std::mutex> hold(s_lookup_lock);
    if (s_details_ready) {
        s_details_ready = false;
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_radar_details(s_found_hex.c_str(), s_details));
    }
    if (s_photo_ready) {
        s_photo_ready = false;
        s_photo_shown = 1 - s_photo_shown;
        const auto &pixels = s_photo[s_photo_shown];
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_radar_photo(s_found_hex.c_str(), pixels.empty() ? nullptr : pixels.data(),
                                                          s_photo_w, s_photo_h));
    }
}

void set_radar_showing(bool showing)
{
    if (s_radar_showing.exchange(showing) != showing && showing) {
        s_sky_now = true;  // on screen: fetched at once rather than on the slow round
        s_wake.notify_all();
    }
}

void look_up(const char *hex, const char *callsign)
{
    std::lock_guard<std::mutex> hold(s_lookup_lock);
    s_lookup_hex      = hex;
    s_lookup_callsign = callsign;
    s_lookup_wanted   = true;
    s_lookup_wake.notify_one();
}
}  // namespace live
