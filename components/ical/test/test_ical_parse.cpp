#include "ical_parse.cpp"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace {
int run(std::string text, ical::Event *out, int capacity, std::uint8_t feed = 0)
{
    std::string buffer = std::move(text);
    return ical::parse(buffer.data(), buffer.size(), feed, out, capacity);
}

// 2026-09-11 06:45:00Z
constexpr std::int64_t kStart = 1789109100;

}  // namespace

TEST(IcalParse, one_event_parsed)
{
    ical::Event events[8]{};
    const int n = run("BEGIN:VCALENDAR\r\n"
                      "BEGIN:VEVENT\r\n"
                      "DTSTART:20260911T064500Z\r\n"
                      "DTEND:20260911T083000Z\r\n"
                      "SUMMARY:Advanced Networking\r\n"
                      "LOCATION:HB 2F\r\n"
                      "END:VEVENT\r\n"
                      "END:VCALENDAR\r\n",
                      events, 8);
    EXPECT_EQ(n, 1) << "one event parsed";
    EXPECT_EQ(events[0].start, kStart) << "start is the UTC instant";
    EXPECT_EQ(events[0].end, kStart + 6300) << "end is the UTC instant";
    EXPECT_EQ(std::strcmp(events[0].summary, "Advanced Networking"), 0) << "summary";
    EXPECT_EQ(std::strcmp(events[0].location, "HB 2F"), 0) << "location";
}

TEST(IcalParse, folded_event_parsed)
{
    ical::Event events[8]{};
    // Folded exactly as the feed writes it: CRLF then one space.
    const int n = run("BEGIN:VEVENT\r\n"
                      "DTSTART:20260911T064500Z\r\n"
                      "SUMMARY:Advanced Networking - T1 BGP Sec\r\n"
                      " urity and routing\r\n"
                      "END:VEVENT\r\n",
                      events, 8);
    EXPECT_EQ(n, 1) << "folded event parsed";
    EXPECT_EQ(std::strcmp(events[0].summary,
                      "Advanced Networking - T1 BGP Security and routing"), 0) << "folded summary is rejoined";
}

TEST(IcalParse, escaped_event_parsed)
{
    ical::Event events[8]{};
    const int n = run("BEGIN:VEVENT\r\nDTSTART:20260911T064500Z\r\n"
                      "SUMMARY:Maths\\, Physics\\; and \\\\ a break\\nsecond line\r\n"
                      "END:VEVENT\r\n",
                      events, 8);
    EXPECT_EQ(n, 1) << "escaped event parsed";
    EXPECT_EQ(std::strcmp(events[0].summary, "Maths, Physics; and \\ a break second line"), 0) << "escapes are undone";
}

TEST(IcalParse, all_day_event_parsed)
{
    ical::Event events[8]{};
    const int n = run("BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20260911\r\n"
                      "SUMMARY:All day\r\nEND:VEVENT\r\n",
                      events, 8);
    EXPECT_EQ(n, 1) << "all-day event parsed";
    EXPECT_EQ(events[0].start, kStart - 6 * 3600 - 2700) << "all-day starts at midnight UTC";
    EXPECT_EQ(events[0].end, events[0].start) << "missing end becomes the start";
}

TEST(IcalParse, an_event_with_no_start_is_skipped)
{
    ical::Event events[8]{};
    const int n = run("BEGIN:VEVENT\r\nSUMMARY:No start\r\nEND:VEVENT\r\n"
                      "BEGIN:VEVENT\r\nDTSTART:20260911T064500Z\r\nSUMMARY:Real\r\n"
                      "END:VEVENT\r\n",
                      events, 8);
    EXPECT_EQ(n, 1) << "an event with no start is skipped";
    EXPECT_EQ(std::strcmp(events[0].summary, "Real"), 0) << "the real one survives";
}

TEST(IcalParse, capacity_is_respected)
{
    ical::Event events[8]{};
    std::string many;
    for (int i = 0; i < 6; ++i) {
        many += "BEGIN:VEVENT\r\nDTSTART:20260911T064500Z\r\nSUMMARY:E\r\nEND:VEVENT\r\n";
    }
    const int n = run(many, events, 3);
    EXPECT_EQ(n, 3) << "capacity is respected";
}

TEST(IcalParse, bare_lf_parsed)
{
    ical::Event events[8]{};
    // Bare LF, and a property whose name merely starts the same way.
    const int n = run("BEGIN:VEVENT\nDTSTART:20260911T064500Z\n"
                      "SUMMARY:Kept\nDTSTAMP:20260101T000000Z\n"
                      "LAST-MODIFIED:20260101T000000Z\nEND:VEVENT\n",
                      events, 8);
    EXPECT_EQ(n, 1) << "bare LF parsed";
    EXPECT_EQ(events[0].start, kStart) << "DTSTAMP did not overwrite DTSTART";
    EXPECT_EQ(std::strcmp(events[0].summary, "Kept"), 0) << "summary kept";
}

TEST(IcalParse, over_long_summary_parsed)
{
    ical::Event events[8]{};
    const int n = run("BEGIN:VEVENT\r\nDTSTART:20260911T064500Z\r\n"
                      "SUMMARY:" + std::string(200, 'x') + "\r\nEND:VEVENT\r\n",
                      events, 8);
    EXPECT_EQ(n, 1) << "over-long summary parsed";
    EXPECT_EQ(std::strlen(events[0].summary), ical::kSummaryMax - 1) << "summary is truncated";
}

TEST(IcalParse, the_feed_is_recorded)
{
    ical::Event events[8]{};
    const int n = run("BEGIN:VEVENT\r\nDTSTART:20260911T064500Z\r\nSUMMARY:A\r\n"
                      "END:VEVENT\r\n",
                      events, 8, 3);
    EXPECT_TRUE(n == 1 && events[0].feed == 3) << "the feed is recorded";
}

TEST(IcalParse, zoned_events_parsed)
{
    ical::Event events[8]{};
    // Outlook writes wall-clock times in a zone it names after itself; the
    // panel reads them in its own. 08:00 in September is 06:00Z, and in
    // December, after the clocks go back, 07:00Z.
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    const int n = run("BEGIN:VCALENDAR\r\n"
                      "BEGIN:VEVENT\r\n"
                      "DTSTART;TZID=Customized Time Zone:20260911T080000\r\n"
                      "DTEND;TZID=Customized Time Zone:20260911T120000\r\n"
                      "SUMMARY:Werken\r\n"
                      "END:VEVENT\r\n"
                      "BEGIN:VEVENT\r\n"
                      "DTSTART;TZID=Customized Time Zone:20261211T080000\r\n"
                      "DTEND;TZID=Customized Time Zone:20261211T120000\r\n"
                      "SUMMARY:Werken\r\n"
                      "END:VEVENT\r\n"
                      "END:VCALENDAR\r\n",
                      events, 8, 4);
    EXPECT_EQ(n, 2) << "zoned events parsed";
    EXPECT_EQ(events[0].start, 1789106400) << "summer: 08:00 local is 06:00Z";
    EXPECT_EQ(events[1].start, 1796972400) << "winter: 08:00 local is 07:00Z";
    EXPECT_EQ(events[1].end - events[1].start, 4 * 3600) << "zoned end";
    setenv("TZ", "UTC0", 1);
    tzset();
}
