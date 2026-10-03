#include "radar_parse.h"
#include "radar_trail.h"

#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace radar;

bool close_to(float got, float want, float slack)
{
    return std::fabs(got - want) <= slack;
}

std::string read_file(const char *name)
{
    const std::string path = std::string(FIXTURES) + "/" + name;
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ADD_FAILURE() << "cannot open " << path;
        return {};
    }
    std::string text;
    char        chunk[4096];
    std::size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        text.append(chunk, got);
    }
    std::fclose(file);
    return text;
}

int run(const std::string &json, Aircraft *out, int capacity)
{
    return parse(json.c_str(), json.size(), out, capacity);
}

void check_capture(const std::string &json)
{
    std::vector<Aircraft> found(64);
    const int             count = run(json, found.data(), static_cast<int>(found.size()));
    EXPECT_EQ(count, 25) << "keeps every airborne aircraft in the capture";
    if (count == 0) {
        return;
    }

    const Aircraft &first = found[0];
    EXPECT_EQ(std::strcmp(first.hex, "484bd1"), 0) << "hex";
    EXPECT_EQ(std::strcmp(first.flight, "KLM90G"), 0) << "callsign, padding trimmed";
    EXPECT_EQ(std::strcmp(first.type, "E190"), 0) << "type";
    EXPECT_TRUE(close_to(first.lat, 52.505768f, 0.00001f)) << "latitude";
    EXPECT_TRUE(close_to(first.lon, 4.048691f, 0.00001f)) << "longitude";
    EXPECT_EQ(first.altitude_ft, 9600) << "altitude";
    EXPECT_TRUE(close_to(first.speed_kt, 307.9f, 0.05f)) << "ground speed";
    EXPECT_TRUE(close_to(first.track_deg, 111.73f, 0.05f)) << "track";
    EXPECT_TRUE(close_to(first.distance_nm, 28.798f, 0.005f)) << "distance";
    EXPECT_TRUE(close_to(first.bearing_deg, 294.3f, 0.05f)) << "bearing";
    EXPECT_EQ(std::strcmp(first.reg, "PH-EZF"), 0) << "registration";
    EXPECT_EQ(std::strcmp(first.desc, "EMBRAER ERJ-190-100"), 0) << "description";
    EXPECT_EQ(first.squawk, 6335) << "squawk, sent as a string";

    int on_ground = 0;
    for (int i = 0; i < count; ++i) {
        on_ground += found[i].on_ground ? 1 : 0;
    }
    EXPECT_EQ(on_ground, 0) << "aircraft on stands are left out";
}

void check_capacity(const std::string &json)
{
    std::vector<Aircraft> found(5);
    EXPECT_EQ(run(json, found.data(), 5), 5) << "stops at capacity";
    EXPECT_EQ(run(json, found.data(), 0), 0) << "no capacity reads nothing";

    float farthest = 0.0f;
    for (const Aircraft &aircraft : found) {
        farthest = std::max(farthest, aircraft.distance_nm);
    }
    EXPECT_TRUE(close_to(farthest, 4.425f, 0.005f)) << "keeps the nearest, not the first";
}

TEST(RadarParse, shapes)
{
    Aircraft one[4];

    const std::string missing =
        R"({"now":1,"aircraft":[{"hex":"a","lat":1.0},{"hex":"b","lat":2.0,"lon":3.0}]})";
    const int kept = parse(missing.c_str(), missing.size(), one, 4);
    EXPECT_TRUE(kept == 1 && std::strcmp(one[0].hex, "b") == 0) << "an entry without a position is skipped, not fatal";

    const std::string nested =
        R"({"aircraft":[{"nav_modes":["autopilot","lnav"],"x":{"y":[1,2]},)"
        R"("hex":"c","lat":1.0,"lon":2.0}]})";
    EXPECT_TRUE(parse(nested.c_str(), nested.size(), one, 4) == 1 && std::strcmp(one[0].hex, "c") == 0) << "nested arrays and objects are stepped over";

    const std::string nulls = R"({"aircraft":[{"flight":null,"gs":null,"hex":"d",)"
                              R"("lat":1.0,"lon":2.0}]})";
    EXPECT_TRUE(parse(nulls.c_str(), nulls.size(), one, 4) == 1 && one[0].flight[0] == '\0') << "a null where a value belongs is skipped";

    const std::string braces =
        R"({"aircraft":[{"desc":"A }] { trap","hex":"e","lat":1.0,"lon":2.0}]})";
    EXPECT_TRUE(parse(braces.c_str(), braces.size(), one, 4) == 1 && std::strcmp(one[0].hex, "e") == 0) << "brackets inside strings do not close anything";

    const std::string escaped =
        R"({"aircraft":[{"desc":"quote \" and \\ ","hex":"f","lat":1.0,"lon":2.0}]})";
    EXPECT_TRUE(parse(escaped.c_str(), escaped.size(), one, 4) == 1 && std::strcmp(one[0].hex, "f") == 0) << "escapes inside strings";

    const std::string blank =
        R"({"aircraft":[{"flight":"@@@@@@@@","hex":"h","lat":1.0,"lon":2.0},)"
        R"({"flight":"PHABC@@ ","hex":"i","lat":1.0,"lon":2.0}]})";
    EXPECT_TRUE(parse(blank.c_str(), blank.size(), one, 4) == 2 && one[0].flight[0] == '\0' &&
              std::strcmp(one[1].flight, "PHABC") == 0) << "ADS-B's blank is padding, and all of it no callsign";

    const std::string long_name =
        R"({"aircraft":[{"flight":"ABCDEFGHIJKLMNOP","hex":"g","lat":1.0,"lon":2.0}]})";
    EXPECT_TRUE(parse(long_name.c_str(), long_name.size(), one, 4) == 1 &&
              std::strlen(one[0].flight) == kFlightLen - 1) << "an over-long callsign is truncated rather than overrunning";

    const std::string no_squawk = R"({"aircraft":[{"hex":"i","lat":1.0,"lon":2.0}]})";
    EXPECT_TRUE(parse(no_squawk.c_str(), no_squawk.size(), one, 4) == 1 && one[0].squawk == -1) << "a missing squawk stays unset rather than reading as zero";

    const std::string long_desc =
        R"({"aircraft":[{"desc":"AIRBUS A350-1041 EXTRA LONG NAME HERE","hex":"j",)"
        R"("lat":1.0,"lon":2.0}]})";
    EXPECT_TRUE(parse(long_desc.c_str(), long_desc.size(), one, 4) == 1 &&
              std::strlen(one[0].desc) == kDescLen - 1) << "an over-long description is truncated";

    const std::string other_key = R"({"ac":[{"hex":"h","lat":1.0,"lon":2.0}]})";
    EXPECT_EQ(parse(other_key.c_str(), other_key.size(), one, 4), 1) << "accepts the \"ac\" spelling";
}

TEST(RadarParse, rejects)
{
    Aircraft one[4];

    const char *empty = "";
    EXPECT_EQ(parse(empty, 0, one, 4), 0) << "empty input";

    const std::string no_list = R"({"now":1,"resultCount":0})";
    EXPECT_EQ(parse(no_list.c_str(), no_list.size(), one, 4), 0) << "no aircraft list";

    const std::string empty_list = R"({"aircraft":[]})";
    EXPECT_EQ(parse(empty_list.c_str(), empty_list.size(), one, 4), 0) << "empty aircraft list";

    const std::string cut = R"({"aircraft":[{"hex":"a","lat":1.0,"lon":2.0},{"hex":"b",)";
    EXPECT_EQ(parse(cut.c_str(), cut.size(), one, 4), 1) << "a truncated response keeps what was whole";

    EXPECT_EQ(parse(nullptr, 10, one, 4), 0) << "null input";
    EXPECT_EQ(parse("{}", 2, nullptr, 4), 0) << "null output";
}

void check_route(const std::string &json)
{
    Details details{};
    EXPECT_TRUE(parse_route(json.c_str(), json.size(), details)) << "reads a flight route";
    EXPECT_EQ(std::strcmp(details.airline, "KLM Royal Dutch Airlines"), 0) << "airline";
    EXPECT_EQ(std::strcmp(details.origin_code, "NCL"), 0) << "origin code";
    EXPECT_EQ(std::strcmp(details.origin_city, "Newcastle"), 0) << "origin city";
    EXPECT_EQ(std::strcmp(details.dest_code, "AMS"), 0) << "destination code";
    EXPECT_EQ(std::strcmp(details.dest_city, "Amsterdam"), 0) << "destination city";
    EXPECT_TRUE(details.has_origin_at && std::fabs(details.origin_lat - 55.0375f) < 0.001f &&
              std::fabs(details.origin_lon + 1.69167f) < 0.001f) << "where the origin is";
    EXPECT_TRUE(details.has_dest_at && std::fabs(details.dest_lat - 52.3086f) < 0.001f &&
              std::fabs(details.dest_lon - 4.76389f) < 0.001f) << "where the destination is";
    EXPECT_NE(std::strcmp(details.airline, "Newcastle Airport"), 0) << "a nested name does not overwrite the airline";
}

void check_aircraft(const std::string &json)
{
    Details details{};
    EXPECT_TRUE(parse_aircraft(json.c_str(), json.size(), details)) << "reads aircraft details";
    EXPECT_EQ(std::strcmp(details.manufacturer, "Embraer"), 0) << "manufacturer";
    EXPECT_EQ(std::strcmp(details.model, "EMB-190 STD"), 0) << "model";
    EXPECT_EQ(std::strcmp(details.owner, "KLM cityhopper"), 0) << "owner";
    EXPECT_EQ(std::strncmp(details.photo_url, "https://airport-data.com/images/", 32), 0) << "photo url";
    EXPECT_TRUE(details.has_aircraft) << "aircraft half marked present";
    EXPECT_TRUE(!details.has_route) << "route half left alone";
}

TEST(RadarParse, lookup_rejects)
{
    Details details{};

    const std::string unknown = R"({"response":"unknown callsign"})";
    EXPECT_TRUE(!parse_route(unknown.c_str(), unknown.size(), details)) << "an unknown callsign is not a route";

    const std::string no_route = R"({"response":{"flightroute":{"callsign":"X"}}})";
    EXPECT_TRUE(!parse_route(no_route.c_str(), no_route.size(), details)) << "a flight with no airports is not a route";

    const std::string cut = R"({"response":{"aircraft":{"type":"A320",)";
    EXPECT_TRUE(!parse_aircraft(cut.c_str(), cut.size(), details)) << "a truncated lookup";

    EXPECT_TRUE(!parse_route(nullptr, 0, details)) << "null route input";
    EXPECT_TRUE(!parse_aircraft(nullptr, 0, details)) << "null aircraft input";
}

void check_photo(const std::string &json, const std::string &none)
{
    char url[160] = {};
    EXPECT_TRUE(parse_photo(json.c_str(), json.size(), url, sizeof(url))) << "a photo is found";
    EXPECT_EQ(std::strncmp(url, "https://", 8), 0) << "the photo address is a url";
    EXPECT_EQ(std::strstr(url, "\\"), nullptr) << "the escapes are gone";
    EXPECT_NE(std::strstr(url, "_280.jpg"), nullptr) << "the larger thumbnail is taken";

    url[0] = 'x';
    EXPECT_TRUE(!parse_photo(none.c_str(), none.size(), url, sizeof(url))) << "an aircraft nobody has photographed";
    EXPECT_EQ(url[0], '\0') << "and it leaves nothing behind";

    EXPECT_TRUE(!parse_photo(nullptr, 0, url, sizeof(url))) << "null photo input";
}

}  // namespace

TEST(RadarParse, trail)
{
    Trail trail{};
    note(trail, 52.0f, 5.0f, 1.0f);
    note(trail, 52.001f, 5.0f, 1.0f);  // some 100 m on: not yet
    note(trail, 52.02f, 5.0f, 1.0f);   // some 2 km
    TrailPoint points[kTrailPoints];
    int        count = oldest_first(trail, points, kTrailPoints);
    EXPECT_TRUE(count == 2 && points[0].lat == 52.0f && points[1].lat == 52.02f) << "a point is kept once it has moved far enough";

    for (int i = 0; i < kTrailPoints + 10; ++i) {
        note(trail, 53.0f + 0.1f * static_cast<float>(i), 5.0f, 1.0f);
    }
    count = oldest_first(trail, points, kTrailPoints);
    EXPECT_TRUE(count == kTrailPoints && std::fabs(points[0].lat - 54.0f) < 0.001f &&
              std::fabs(points[kTrailPoints - 1].lat - (53.0f + 0.1f * (kTrailPoints + 9))) < 0.001f) << "the oldest go once it is full, and the order holds";
    count = oldest_first(trail, points, 3);
    EXPECT_TRUE(count == 3 && std::fabs(points[2].lat - (53.0f + 0.1f * (kTrailPoints + 9))) < 0.001f) << "asked for fewer, the newest are the ones given";
}

TEST(RadarParse, seen)
{
    Trail trail{};
    seen(trail, 100, 180);
    note(trail, 52.0f, 5.0f, 1.0f);
    seen(trail, 160, 180);
    note(trail, 52.02f, 5.0f, 1.0f);
    EXPECT_TRUE(trail.count == 2 && trail.seen_us == 160) << "seen again in time, it goes on";
    seen(trail, 400, 180);
    note(trail, 52.5f, 5.0f, 1.0f);
    TrailPoint points[kTrailPoints];
    const int  count = oldest_first(trail, points, kTrailPoints);
    EXPECT_TRUE(count == 1 && points[0].lat == 52.5f) << "unseen too long, it starts again rather than join up";
}

void check_trace(const std::string &json)
{
    Trail trail{};
    std::snprintf(trail.hex, sizeof(trail.hex), "4ca27a");
    note(trail, 40.0f, 1.0f, 1.0f);
    int        positions = parse_trace(json.c_str(), json.size(), trail, 0.0f);
    TrailPoint points[kTrailPoints];
    int        count = oldest_first(trail, points, kTrailPoints);
    EXPECT_TRUE(positions == 5 && count == 5 && std::fabs(points[0].lat - 51.943452f) < 0.0001f &&
              std::fabs(points[4].lon - 1.391204f) < 0.0001f) << "a trace's positions, oldest first, past objects inside an entry and one without a position";
    EXPECT_EQ(std::strcmp(trail.hex, "4ca27a"), 0) << "and what was there before is gone but whose it is";

    parse_trace(json.c_str(), json.size(), trail, 20.0f);
    count = oldest_first(trail, points, kTrailPoints);
    EXPECT_TRUE(count == 2 && std::fabs(points[1].lat - 52.090312f) < 0.0001f) << "kept as far apart as the step asks";

    const char *other = "{\"ac\":[]}";
    EXPECT_EQ(parse_trace(other, std::strlen(other), trail, 1.0f), -1) << "not a trace";
}

// Recorded answers from the feed, the lookup service and the photo service.
TEST(RadarParse, capture) { check_capture(read_file("adsb_sample.json")); }
TEST(RadarParse, capacity) { check_capacity(read_file("adsb_sample.json")); }
TEST(RadarParse, route) { check_route(read_file("adsbdb_route.json")); }
TEST(RadarParse, aircraft) { check_aircraft(read_file("adsbdb_aircraft.json")); }
TEST(RadarParse, trace) { check_trace(read_file("trace_recent.json")); }
TEST(RadarParse, photo) { check_photo(read_file("planespotters_photo.json"), read_file("planespotters_none.json")); }

namespace {
radar::Details route(float from_lat, float from_lon, float to_lat, float to_lon)
{
    radar::Details details{};
    details.has_route     = true;
    details.has_origin_at = details.has_dest_at = true;
    details.origin_lat    = from_lat;
    details.origin_lon    = from_lon;
    details.dest_lat      = to_lat;
    details.dest_lon      = to_lon;
    return details;
}
}  // namespace

TEST(RadarParse, route_fits)
{
    // RYR2UC over the Ruhr, flying Weeze to Thessaloniki, which the database
    // has as La Rochelle to Porto, another of the callsign's days.
    EXPECT_FALSE(radar::route_fits(route(46.18f, -1.20f, 41.25f, -8.68f), 51.46f, 7.03f)) << "another day's route";
    EXPECT_TRUE(radar::route_fits(route(51.60f, 6.14f, 40.52f, 22.97f), 51.46f, 7.03f)) << "its own, just after leaving";
    // Amsterdam to New York over the North Atlantic, well off the great circle.
    EXPECT_TRUE(radar::route_fits(route(52.31f, 4.76f, 40.64f, -73.78f), 55.0f, -20.0f)) << "a long way round";
    EXPECT_FALSE(radar::route_fits(route(52.31f, 4.76f, 51.47f, -0.45f), 48.0f, 11.0f)) << "past its origin, far";
    EXPECT_FALSE(radar::route_fits(route(52.31f, 4.76f, 51.47f, -0.45f), 50.0f, -6.0f)) << "past its destination, far";
    radar::Details unplaced = route(0, 0, 0, 0);
    unplaced.has_origin_at  = false;
    EXPECT_TRUE(radar::route_fits(unplaced, 51.46f, 7.03f)) << "nothing to say it is wrong";
}

TEST(RadarParse, interesting)
{
    const auto aircraft = [](const char *flight, const char *category, bool on_ground = false) {
        Aircraft a{};
        std::snprintf(a.flight, sizeof(a.flight), "%s", flight);
        std::snprintf(a.category, sizeof(a.category), "%s", category);
        a.on_ground = on_ground;
        return a;
    };
    EXPECT_TRUE(interesting(aircraft("KLM90G", "A3"))) << "an airline's flight";
    EXPECT_TRUE(interesting(aircraft("RYR15P", ""))) << "by its callsign alone";
    EXPECT_TRUE(interesting(aircraft("", "A5"))) << "a heavy without a callsign";
    EXPECT_FALSE(interesting(aircraft("PHAHJ", "A1"))) << "a light aircraft by its registration";
    EXPECT_FALSE(interesting(aircraft("N217SR", "A1"))) << "nor an American one";
    EXPECT_FALSE(interesting(aircraft("", "A7"))) << "a helicopter";
    EXPECT_FALSE(interesting(aircraft("KLM90G", "A3", true))) << "nothing on the ground";
}
