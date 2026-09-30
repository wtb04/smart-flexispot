#include "clock_math.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
std::tm made(int year, int mon, int day, int hour, int min, int sec)
{
    std::tm out{};
    out.tm_year = year - 1900;
    out.tm_mon  = mon - 1;
    out.tm_mday = day;
    out.tm_hour = hour;
    out.tm_min  = min;
    out.tm_sec  = sec;
    return out;
}

TEST(ClockMath, bcd)
{
    EXPECT_EQ(rtc::from_bcd(0x00), 0) << "bcd 0x00 is 0";
    EXPECT_EQ(rtc::from_bcd(0x09), 9) << "bcd 0x09 is 9";
    EXPECT_EQ(rtc::from_bcd(0x10), 10) << "bcd 0x10 is 10";
    EXPECT_EQ(rtc::from_bcd(0x59), 59) << "bcd 0x59 is 59";
    EXPECT_EQ(rtc::to_bcd(0), 0x00) << "0 is bcd 0x00";
    EXPECT_EQ(rtc::to_bcd(9), 0x09) << "9 is bcd 0x09";
    EXPECT_EQ(rtc::to_bcd(23), 0x23) << "23 is bcd 0x23";
    EXPECT_EQ(rtc::to_bcd(59), 0x59) << "59 is bcd 0x59";

    bool round_trips = true;
    for (int i = 0; i <= 99; ++i) {
        round_trips = round_trips && rtc::from_bcd(rtc::to_bcd(i)) == i;
    }
    EXPECT_TRUE(round_trips) << "every value 0 to 99 survives the round trip";
}

TEST(ClockMath, epoch)
{
    EXPECT_EQ(rtc::utc_seconds(made(1970, 1, 1, 0, 0, 0)), 0) << "the epoch itself";
    EXPECT_EQ(rtc::utc_seconds(made(1970, 1, 1, 0, 0, 1)), 1) << "one second past it";
    EXPECT_EQ(rtc::utc_seconds(made(2000, 1, 1, 0, 0, 0)), 946684800) << "the millennium";
    EXPECT_EQ(rtc::utc_seconds(made(2024, 2, 29, 12, 0, 0)), 1709208000) << "a leap day";
    EXPECT_EQ(rtc::utc_seconds(made(2038, 1, 19, 3, 14, 7)), 2147483647) << "the far end of 32 bits";
}

TEST(ClockMath, century)
{
    EXPECT_EQ(rtc::utc_seconds(made(2000 + rtc::from_bcd(0x00), 1, 1, 0, 0, 0)), 946684800) << "chip year 00 reads as 2000";
    EXPECT_EQ(rtc::utc_seconds(made(2000 + rtc::from_bcd(0x25), 6, 15, 8, 30, 0)), rtc::utc_seconds(made(2025, 6, 15, 8, 30, 0))) << "chip year 25 reads as 2025";
}

TEST(ClockMath, against_the_system)
{
    static const std::tm cases[] = {
        made(1999, 12, 31, 23, 59, 59), made(2001, 3, 1, 0, 0, 0),
        made(2016, 2, 29, 6, 30, 15),   made(2020, 12, 31, 23, 59, 59),
        made(2023, 7, 4, 12, 0, 0),     made(2025, 9, 22, 18, 45, 30),
        made(2031, 11, 30, 4, 20, 10),  made(2064, 5, 5, 5, 5, 5),
    };
    bool all_match = true;
    for (std::tm one : cases) {
        std::tm copy     = one;
        const auto mine  = rtc::utc_seconds(one);
        const auto theirs = timegm(&copy);
        if (mine != theirs) {
            std::printf("      %04d-%02d-%02d: mine %lld, timegm %lld\n", one.tm_year + 1900,
                        one.tm_mon + 1, one.tm_mday, static_cast<long long>(mine),
                        static_cast<long long>(theirs));
            all_match = false;
        }
    }
    EXPECT_TRUE(all_match) << "agrees with the host's timegm across the years";
}

}  // namespace

