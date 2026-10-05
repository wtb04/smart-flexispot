#pragma once

#include <cstdint>

// When the screen goes dark by itself and when it lights again, with nothing of
// LVGL in it. The big light or the light scene on says someone is in the room,
// so the screen stays; with them off it stays only for the phone by day.
// Unplugged away from the desk it is somewhere else and goes dark soon. Only
// something happening lights it again, never the night ending: whoever sleeps
// through it is not woken.
namespace ui::screen_rules {

constexpr std::int64_t kNever         = -1;
constexpr std::int64_t kAwayMs        = 30 * 1000;
constexpr std::int64_t kNightMs       = 60 * 1000;
constexpr std::int64_t kPhoneGoneMs   = 5 * 60 * 1000;  // unheard this long: away, not a fluke
constexpr int          kNightFromMin  = 22 * 60 + 30;
constexpr int          kNightUntilMin = 7 * 60;

inline bool is_night(int minute_of_day)
{
    return minute_of_day >= kNightFromMin || minute_of_day < kNightUntilMin;
}

/** Things as they are at one look. */
struct Inputs {
    bool         screen_on  = true;
    bool         unplugged  = false;
    bool         at_desk    = false;  // the desk answers over Bluetooth
    bool         room_known = false;  // Home Assistant has told the lights
    bool         room_lit   = false;  // the big light or the light scene
    bool         phone      = false;  // heard near just now
    bool         night      = false;  // false while the clock is not set
    bool         attention  = false;  // a notice or a skip button: lights the screen and holds it
    std::int64_t film_ms    = 0;      // the open film's own wait: 0 none open, kNever held lit
    std::int64_t untouched  = 0;      // since the last touch
};

enum class Action : std::uint8_t { Keep, Wake, Dark };

enum class Why : std::uint8_t {
    None,
    LightOn,    // woke: the big light or the scene came on
    PhoneBack,  // woke: the phone came back after counting as gone
    Attention,  // woke for a notice or a skip button, and dark once it went
    Film,       // the film's own wait
    Away,       // unplugged and away from the desk
    Empty,      // the lights off and the phone gone
    Night,      // the lights off at night
};

struct Decision {
    Action action = Action::Keep;
    Why    why    = Why::None;
    bool operator==(const Decision &) const = default;
};

inline const char *describe(Why why)
{
    switch (why) {
        case Why::LightOn:   return "the light came on";
        case Why::PhoneBack: return "the phone came back";
        case Why::Attention: return "a notice or a skip button";
        case Why::Film:      return "the film's own wait";
        case Why::Away:      return "unplugged, away from the desk";
        case Why::Empty:     return "the lights off, the phone gone";
        case Why::Night:     return "the lights off, at night";
        default:             return "nothing";
    }
}

/** Looked at whenever something changes and at least once a second, `now` in
 *  milliseconds from the first look. */
class Schedule {
public:
    Decision step(const Inputs &in, std::int64_t now)
    {
        const bool phone_back = follow_phone(in.phone, now);
        const bool light_on   = follow_room(in);
        const bool asked      = in.attention && !attention_;
        const bool let_go     = !in.attention && attention_;
        attention_            = in.attention;

        // Lit by anything, or a new rule: the wait starts again from here.
        const Rule rule = rule_for(in);
        if ((in.screen_on && !screen_on_) || rule.after != rule_.after) {
            since_ = now;
        }
        screen_on_ = in.screen_on;
        rule_      = rule;

        if (!in.screen_on) {
            return wake_for(phone_back, light_on, asked, now);
        }
        if (in.attention) {
            return {};
        }
        if (let_go && lit_for_attention_ && in.untouched >= now - woke_at_) {
            return dark(Why::Attention);
        }
        const std::int64_t quiet = in.untouched < now - since_ ? in.untouched : now - since_;
        if (rule.after == kNever || quiet < rule.after) {
            return {};
        }
        return dark(rule.why);
    }

private:
    struct Rule {
        std::int64_t after = kNever;  // left alone this long, dark
        Why          why   = Why::None;
    };

    Rule rule_for(const Inputs &in) const
    {
        if (in.film_ms != 0) {
            return {in.film_ms, Why::Film};
        }
        if (in.unplugged && !in.at_desk) {
            return {kAwayMs, Why::Away};
        }
        if (!in.room_known || in.room_lit) {
            return {};
        }
        if (!phone_near_) {
            return {kAwayMs, Why::Empty};
        }
        return in.night ? Rule{kNightMs, Why::Night} : Rule{};
    }

    /** True as the phone comes back after counting as gone. */
    bool follow_phone(bool heard, std::int64_t now)
    {
        if (heard) {
            heard_at_ = now;
        }
        const bool was_near = phone_near_;
        phone_near_         = now - heard_at_ < kPhoneGoneMs;
        return phone_near_ && !was_near;
    }

    /** True as the room's light comes on after being told off. */
    bool follow_room(const Inputs &in)
    {
        const bool came_on = in.room_known && in.room_lit && room_was_dark_;
        room_was_dark_     = in.room_known && !in.room_lit;
        return came_on;
    }

    Decision wake_for(bool phone_back, bool light_on, bool asked, std::int64_t now)
    {
        if (!phone_back && !light_on && !asked) {
            return {};
        }
        lit_for_attention_ = !phone_back && !light_on;
        woke_at_           = now;
        return {Action::Wake, light_on ? Why::LightOn : phone_back ? Why::PhoneBack : Why::Attention};
    }

    Decision dark(Why why)
    {
        lit_for_attention_ = false;
        return {Action::Dark, why};
    }

    std::int64_t heard_at_          = 0;  // a phone not heard since the first look gets its time from there
    bool         phone_near_        = true;
    bool         room_was_dark_     = false;
    bool         attention_         = false;
    bool         lit_for_attention_ = false;  // lit for it alone, so dark again after unless touched
    std::int64_t woke_at_           = 0;
    bool         screen_on_         = true;
    Rule         rule_;
    std::int64_t since_ = 0;  // when the current rule's wait began
};

}  // namespace ui::screen_rules
