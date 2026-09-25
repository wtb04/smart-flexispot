#pragma once

#include <cstdint>

// The conversions every part of the panel does, named once, so no file has to
// spell out what 3600 or 1024 means.
namespace units {
inline constexpr int          kMsPerSecond      = 1000;
inline constexpr std::int64_t kUsPerMs          = 1000;
inline constexpr std::int64_t kUsPerSecond      = 1000 * kUsPerMs;
inline constexpr int          kSecondsPerMinute = 60;
inline constexpr int          kMinutesPerHour   = 60;
inline constexpr int          kHoursPerDay      = 24;
inline constexpr int          kDaysPerWeek      = 7;
inline constexpr int          kSecondsPerHour   = kSecondsPerMinute * kMinutesPerHour;
inline constexpr int          kSecondsPerDay    = kSecondsPerHour * kHoursPerDay;
inline constexpr int          kMsPerMinute      = kMsPerSecond * kSecondsPerMinute;
inline constexpr std::int64_t kUsPerMinute      = kUsPerSecond * kSecondsPerMinute;
inline constexpr int          kBytesPerKiB      = 1024;
inline constexpr int          kMmPerCm          = 10;
}  // namespace units
