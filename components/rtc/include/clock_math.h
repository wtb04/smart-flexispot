#pragma once

#include <cstdint>
#include <ctime>

namespace rtc {
/** A clock that says before 2024 has not been set: the backup chip starts at
 *  2000 and the system clock at 1970, and neither is a time to act on. */
inline constexpr std::time_t kSetAfter = 1704067200;  // 2024-01-01 00:00 UTC

/** The year std::tm counts from. */
inline constexpr int kTmYearBase = 1900;

inline bool plausible(std::time_t when)
{
    return when >= kSetAfter;
}

inline constexpr int          kBcdDigitBits = 4;
inline constexpr std::uint8_t kBcdDigitMask = 0x0f;
inline constexpr int          kDecimalBase  = 10;

inline std::uint8_t from_bcd(std::uint8_t value)
{
    return static_cast<std::uint8_t>((value >> kBcdDigitBits) * kDecimalBase +
                                     (value & kBcdDigitMask));
}

inline std::uint8_t to_bcd(int value)
{
    return static_cast<std::uint8_t>(((value / kDecimalBase) << kBcdDigitBits) |
                                     (value % kDecimalBase));
}

// The calendar as days_from_civil counts it: years start in March, so the leap
// day comes last, and every 400 years the calendar repeats exactly. Kept here
// rather than taken from units.h so the header stands alone for host tests.
namespace civil {
inline constexpr int  kFebruary            = 2;
inline constexpr int  kMarch               = 3;
inline constexpr int  kMonthsPerYear       = 12;
inline constexpr int  kYearsPerEra         = 400;
inline constexpr int  kYearsPerCentury     = 100;
inline constexpr int  kYearsPerLeap        = 4;
inline constexpr int  kDaysPerYear         = 365;
inline constexpr long kDaysPerEra          = 146097;
inline constexpr long kEraStartToEpochDays = 719468;  // 0000-03-01 to 1970-01-01

// From March the months run 31, 30, 31, 30, 31 and repeat, 153 days to each
// run of five; the 2 lands each month's first day on the right one.
inline constexpr int kMonthsPerRun = 5;
inline constexpr int kDaysPerRun   = 153;
inline constexpr int kRunRounding  = 2;

inline constexpr int kSecondsPerMinute = 60;
inline constexpr int kMinutesPerHour   = 60;
inline constexpr int kHoursPerDay      = 24;
inline constexpr int kSecondsPerHour   = kMinutesPerHour * kSecondsPerMinute;
inline constexpr int kSecondsPerDay    = kHoursPerDay * kSecondsPerHour;
}  // namespace civil

/** newlib offers no timegm, and mktime would apply the local timezone to a
 *  reading that is already UTC. Days since the epoch, counted directly. */
inline std::time_t utc_seconds(const std::tm &utc)
{
    using namespace civil;
    const int year       = utc.tm_year + kTmYearBase;
    const int month      = utc.tm_mon + 1;
    const int from_march = month > kFebruary ? month - kMarch : month + kMonthsPerYear - kMarch;
    const int shift      = year - (month <= kFebruary ? 1 : 0);
    const int era        = (shift >= 0 ? shift : shift - (kYearsPerEra - 1)) / kYearsPerEra;

    const auto of_era  = static_cast<unsigned>(shift - era * kYearsPerEra);
    const auto of_year = static_cast<unsigned>(
        (kDaysPerRun * from_march + kRunRounding) / kMonthsPerRun + utc.tm_mday - 1);
    const unsigned day_of_era =
        of_era * kDaysPerYear + of_era / kYearsPerLeap - of_era / kYearsPerCentury + of_year;
    const long days =
        static_cast<long>(era) * kDaysPerEra + static_cast<long>(day_of_era) - kEraStartToEpochDays;
    return static_cast<std::time_t>(days) * kSecondsPerDay + utc.tm_hour * kSecondsPerHour +
           utc.tm_min * kSecondsPerMinute + utc.tm_sec;
}

}  // namespace rtc
