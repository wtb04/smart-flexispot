#pragma once

#include <cstdint>

// When the screen goes dark by itself and when it lights again. The big light
// or the light scene on says someone is in the room, so the screen stays; with
// them off it stays only for the phone by day. Unplugged away from the desk it
// is somewhere else, and goes dark soon. Only something happening lights
// it again, never the night ending: whoever sleeps through it is not woken.
namespace ui::screen_rules {

constexpr std::int64_t kPhoneGoneMs   = 5 * 60 * 1000;  // unheard this long: away, not a fluke
constexpr std::int64_t kAwayDarkMs    = 30 * 1000;
constexpr std::int64_t kNightDarkMs   = 60 * 1000;
constexpr std::int64_t kNever         = -1;
constexpr int          kNightFromMin  = 22 * 60 + 30;
constexpr int          kNightUntilMin = 7 * 60;

inline bool is_night(int minute_of_day)
{
    return minute_of_day >= kNightFromMin || minute_of_day < kNightUntilMin;
}

struct Inputs {
    bool         screen_on  = true;
    bool         on_battery = false;
    bool         desk_linked = false;  // unplugged and not linked: away, its room's lights say nothing
    bool         lit_known  = false;  // Home Assistant has told the lights
    bool         lit        = false;  // the big light or the light scene
    bool         phone      = false;  // heard near just now
    bool         night      = false;  // false while the clock is not set
    bool         notice     = false;  // one on show keeps the screen for its time
    std::int64_t video_ms   = 0;      // the full-screen video's own: 0 none, kNever kept lit
    std::int64_t untouched  = 0;      // since the last touch
};

enum class Action : std::uint8_t { Keep, Wake, Dark };

class Schedule {
public:
    /** How long left alone before dark, as things are; kNever to stay lit. */
    std::int64_t dark_after(const Inputs &in) const
    {
        if (in.video_ms != 0) {
            return in.video_ms;
        }
        if (in.on_battery && !in.desk_linked) {
            return kAwayDarkMs;
        }
        if (!in.lit_known || in.lit) {
            return kNever;
        }
        if (!phone_here_) {
            return kAwayDarkMs;
        }
        return in.night ? kNightDarkMs : kNever;
    }

    /** Once a second or so, `now` in milliseconds from any start. */
    Action step(const Inputs &in, std::int64_t now)
    {
        if (!started_) {
            started_  = true;
            heard_at_ = now;  // a phone not heard yet since the start is given its time
            count_from_ = now;
        }
        if (in.phone) {
            heard_at_ = now;
        }
        const bool was_here = phone_here_;
        phone_here_         = now - heard_at_ < kPhoneGoneMs;
        const bool arrived  = phone_here_ && !was_here;

        const bool lit_now = in.lit_known && in.lit;
        const bool lit_up  = lit_now && was_dark_known_;
        was_dark_known_    = in.lit_known && !in.lit;

        const bool woke = in.screen_on && !was_on_;
        was_on_         = in.screen_on;

        const std::int64_t after = dark_after(in);
        if (arrived || lit_up || woke || (after != kNever && after != last_after_)) {
            count_from_ = now;
        }
        last_after_ = after;

        if (!in.screen_on) {
            if (arrived || lit_up) {
                was_on_ = true;
                return Action::Wake;
            }
            return Action::Keep;
        }
        const std::int64_t quiet = in.untouched < now - count_from_ ? in.untouched : now - count_from_;
        if (after == kNever || in.notice || quiet < after) {
            return Action::Keep;
        }
        was_on_ = false;
        return Action::Dark;
    }

    bool phone_here() const { return phone_here_; }

private:
    bool         started_        = false;
    bool         phone_here_     = true;
    bool         was_dark_known_ = false;
    bool         was_on_         = true;
    std::int64_t heard_at_       = 0;
    std::int64_t count_from_     = 0;
    std::int64_t last_after_     = kNever;
};

}  // namespace ui::screen_rules
