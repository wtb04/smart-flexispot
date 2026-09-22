#pragma once

// The arithmetic the backup clock needs, kept free of hardware so it can be
// tested on a host. Getting a date conversion subtly wrong is the sort of thing
// that shows up months later on one particular day of the year.

#include <cstdint>
#include <ctime>

namespace rtc {

inline std::uint8_t from_bcd(std::uint8_t value)
{
    return static_cast<std::uint8_t>((value >> 4) * 10 + (value & 0x0f));
}

inline std::uint8_t to_bcd(int value)
{
    return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}

/** newlib offers no timegm, and mktime would apply the local timezone to a
 *  reading that is already UTC. Days since the epoch, counted directly. */
inline std::time_t utc_seconds(const std::tm &utc)
{
    const int      year    = utc.tm_year + 1900;
    const int      month   = utc.tm_mon + 1;
    const int      shift   = year - (month <= 2 ? 1 : 0);
    const int      era     = (shift >= 0 ? shift : shift - 399) / 400;
    const unsigned of_era  = static_cast<unsigned>(shift - era * 400);
    const unsigned of_year =
        static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + utc.tm_mday - 1);
    const unsigned day_of_era = of_era * 365 + of_era / 4 - of_era / 100 + of_year;
    const long     days =
        static_cast<long>(era) * 146097 + static_cast<long>(day_of_era) - 719468;
    return static_cast<std::time_t>(days) * 86400 + utc.tm_hour * 3600 + utc.tm_min * 60 +
           utc.tm_sec;
}

}  // namespace rtc
