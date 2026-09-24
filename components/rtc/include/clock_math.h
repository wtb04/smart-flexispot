#pragma once

#include <cstdint>
#include <ctime>

namespace rtc {
/** A clock that says before 2024 has not been set: the backup chip starts at
 *  2000 and the system clock at 1970, and neither is a time to act on. */
inline constexpr std::time_t kSetAfter = 1704067200;  // 2024-01-01 00:00 UTC

inline bool plausible(std::time_t when)
{
    return when >= kSetAfter;
}

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
