#pragma once

#include "laptop_protocol.h"

#include <array>
#include <cstdint>
#include <string>

// The laptops sharing with the panel, and the one it follows: whichever plays,
// the one that began last when several do, else the one it had. A heartbeat
// never moves it, so two laptops at the desk no longer take turns. Pure, so
// the host tests can run it; laptop.cpp holds one under its lock.
namespace laptop {

struct Seen {
    std::string  origin;  // http://address:port, where it answers
    NowPlaying   now;
    std::int64_t heard_us   = 0;
    std::int64_t started_us = -1;  // when it last began playing; -1 never
    std::int64_t pinged_us  = -1;
    int          reaches    = -1;  // whether the panel reaches its port back: -1 untried, 0 no, 1 yes
    int          missed     = 0;   // pings in a row it did not answer
};

struct Taken {
    bool ping    = false;  // try this laptop's port back now
    int  reaches = -1;     // what its pong says
    bool tell    = false;  // the panel's now playing has changed, to be told again
};

class Laptops {
public:
    static constexpr int kMax = 4;

    /** A state from `origin`, which joins the table if new. */
    Taken take(const std::string &origin, const NowPlaying &now, std::int64_t now_us, std::int64_t ping_every_us);
    /** Those not heard from for `quiet_us` gone; true when the one followed changed. */
    bool forget_quiet(std::int64_t now_us, std::int64_t quiet_us);
    /** A sealed answer from `origin`, or a ping it missed; true when its reaches changed.
     *  It is said not to reach after two missed in a row. */
    bool reached(const std::string &origin, bool yes);
    /** The followed laptop's playing set by the panel itself, ahead of its report; false with none. */
    bool set_playing(bool playing, std::int64_t now_us);

    NowPlaying  current_now() const;     // inactive when none is followed
    std::string current_origin() const;  // empty when none is followed
    int         reaches_of(const std::string &origin) const;  // -2 for one not in the table
    int         count() const { return count_; }

private:
    int  find(const std::string &origin) const;
    int  make_room();
    void choose();

    std::array<Seen, kMax> rows_{};
    int                    count_   = 0;
    int                    current_ = -1;
};

}  // namespace laptop
