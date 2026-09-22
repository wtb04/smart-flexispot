#include "ical_parse.cpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
int  failures = 0;
int  checks   = 0;

void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

int run(std::string text, ical::Event *out, int capacity, std::uint8_t feed = 0)
{
    std::string buffer = std::move(text);
    return ical::parse(buffer.data(), buffer.size(), feed, out, capacity);
}

// 2026-09-11 06:45:00Z
constexpr std::int64_t kStart = 1789109100;

}  // namespace

int main()
{
    ical::Event events[8]{};

    {
        const int n = run("BEGIN:VCALENDAR\r\n"
                          "BEGIN:VEVENT\r\n"
                          "DTSTART:20260911T064500Z\r\n"
                          "DTEND:20260911T083000Z\r\n"
                          "SUMMARY:Advanced Networking\r\n"
                          "LOCATION:HB 2F\r\n"
                          "END:VEVENT\r\n"
                          "END:VCALENDAR\r\n",
                          events, 8);
        check(n == 1, "one event parsed");
        check(events[0].start == kStart, "start is the UTC instant");
        check(events[0].end == kStart + 6300, "end is the UTC instant");
        check(std::strcmp(events[0].summary, "Advanced Networking") == 0, "summary");
        check(std::strcmp(events[0].location, "HB 2F") == 0, "location");
    }

    {
        // Folded exactly as the feed writes it: CRLF then one space.
        const int n = run("BEGIN:VEVENT\r\n"
                          "DTSTART:20260911T064500Z\r\n"
                          "SUMMARY:Advanced Networking - T1 BGP Sec\r\n"
                          " urity and routing\r\n"
                          "END:VEVENT\r\n",
                          events, 8);
        check(n == 1, "folded event parsed");
        check(std::strcmp(events[0].summary,
                          "Advanced Networking - T1 BGP Security and routing") == 0,
              "folded summary is rejoined");
    }

    {
        const int n = run("BEGIN:VEVENT\r\nDTSTART:20260911T064500Z\r\n"
                          "SUMMARY:Maths\\, Physics\\; and \\\\ a break\\nsecond line\r\n"
                          "END:VEVENT\r\n",
                          events, 8);
        check(n == 1, "escaped event parsed");
        check(std::strcmp(events[0].summary, "Maths, Physics; and \\ a break second line") == 0,
              "escapes are undone");
    }

    {
        const int n = run("BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20260911\r\n"
                          "SUMMARY:All day\r\nEND:VEVENT\r\n",
                          events, 8);
        check(n == 1, "all-day event parsed");
        check(events[0].start == kStart - 6 * 3600 - 2700, "all-day starts at midnight UTC");
        check(events[0].end == events[0].start, "missing end becomes the start");
    }

    {
        const int n = run("BEGIN:VEVENT\r\nSUMMARY:No start\r\nEND:VEVENT\r\n"
                          "BEGIN:VEVENT\r\nDTSTART:20260911T064500Z\r\nSUMMARY:Real\r\n"
                          "END:VEVENT\r\n",
                          events, 8);
        check(n == 1, "an event with no start is skipped");
        check(std::strcmp(events[0].summary, "Real") == 0, "the real one survives");
    }

    {
        std::string many;
        for (int i = 0; i < 6; ++i) {
            many += "BEGIN:VEVENT\r\nDTSTART:20260911T064500Z\r\nSUMMARY:E\r\nEND:VEVENT\r\n";
        }
        const int n = run(many, events, 3);
        check(n == 3, "capacity is respected");
    }

    {
        // Bare LF, and a property whose name merely starts the same way.
        const int n = run("BEGIN:VEVENT\nDTSTART:20260911T064500Z\n"
                          "SUMMARY:Kept\nDTSTAMP:20260101T000000Z\n"
                          "LAST-MODIFIED:20260101T000000Z\nEND:VEVENT\n",
                          events, 8);
        check(n == 1, "bare LF parsed");
        check(events[0].start == kStart, "DTSTAMP did not overwrite DTSTART");
        check(std::strcmp(events[0].summary, "Kept") == 0, "summary kept");
    }

    {
        const int n = run("BEGIN:VEVENT\r\nDTSTART:20260911T064500Z\r\n"
                          "SUMMARY:" + std::string(200, 'x') + "\r\nEND:VEVENT\r\n",
                          events, 8);
        check(n == 1, "over-long summary parsed");
        check(std::strlen(events[0].summary) == ical::kSummaryMax - 1, "summary is truncated");
    }

    {
        const int n = run("BEGIN:VEVENT\r\nDTSTART:20260911T064500Z\r\nSUMMARY:A\r\n"
                          "END:VEVENT\r\n",
                          events, 8, 3);
        check(n == 1 && events[0].feed == 3, "the feed is recorded");
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        std::printf("ALL PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
