#include "radar_parse.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace radar;

int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

bool close_to(float got, float want, float slack)
{
    return std::fabs(got - want) <= slack;
}

std::string read_file(const char *path)
{
    std::FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::printf("FAIL  cannot open %s\n", path);
        ++g_failures;
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

void test_capture(const std::string &json)
{
    std::vector<Aircraft> found(64);
    const int             count = run(json, found.data(), static_cast<int>(found.size()));
    check(count == 25, "keeps every airborne aircraft in the capture");
    if (count == 0) {
        return;
    }

    const Aircraft &first = found[0];
    check(std::strcmp(first.hex, "484bd1") == 0, "hex");
    check(std::strcmp(first.flight, "KLM90G") == 0, "callsign, padding trimmed");
    check(std::strcmp(first.type, "E190") == 0, "type");
    check(close_to(first.lat, 52.505768f, 0.00001f), "latitude");
    check(close_to(first.lon, 4.048691f, 0.00001f), "longitude");
    check(first.altitude_ft == 9600, "altitude");
    check(close_to(first.speed_kt, 307.9f, 0.05f), "ground speed");
    check(close_to(first.track_deg, 111.73f, 0.05f), "track");
    check(close_to(first.distance_nm, 28.798f, 0.005f), "distance");
    check(close_to(first.bearing_deg, 294.3f, 0.05f), "bearing");
    check(std::strcmp(first.reg, "PH-EZF") == 0, "registration");
    check(std::strcmp(first.desc, "EMBRAER ERJ-190-100") == 0, "description");
    check(first.squawk == 6335, "squawk, sent as a string");

    int on_ground = 0;
    for (int i = 0; i < count; ++i) {
        on_ground += found[i].on_ground ? 1 : 0;
    }
    check(on_ground == 0, "aircraft on stands are left out");
}

void test_capacity(const std::string &json)
{
    std::vector<Aircraft> found(5);
    check(run(json, found.data(), 5) == 5, "stops at capacity");
    check(run(json, found.data(), 0) == 0, "no capacity reads nothing");

    float farthest = 0.0f;
    for (const Aircraft &aircraft : found) {
        farthest = std::max(farthest, aircraft.distance_nm);
    }
    check(close_to(farthest, 4.425f, 0.005f), "keeps the nearest, not the first");
}

void test_shapes()
{
    Aircraft one[4];

    const std::string missing =
        R"({"now":1,"aircraft":[{"hex":"a","lat":1.0},{"hex":"b","lat":2.0,"lon":3.0}]})";
    const int kept = parse(missing.c_str(), missing.size(), one, 4);
    check(kept == 1 && std::strcmp(one[0].hex, "b") == 0,
          "an entry without a position is skipped, not fatal");

    const std::string nested =
        R"({"aircraft":[{"nav_modes":["autopilot","lnav"],"x":{"y":[1,2]},)"
        R"("hex":"c","lat":1.0,"lon":2.0}]})";
    check(parse(nested.c_str(), nested.size(), one, 4) == 1 && std::strcmp(one[0].hex, "c") == 0,
          "nested arrays and objects are stepped over");

    const std::string nulls = R"({"aircraft":[{"flight":null,"gs":null,"hex":"d",)"
                              R"("lat":1.0,"lon":2.0}]})";
    check(parse(nulls.c_str(), nulls.size(), one, 4) == 1 && one[0].flight[0] == '\0',
          "a null where a value belongs is skipped");

    const std::string braces =
        R"({"aircraft":[{"desc":"A }] { trap","hex":"e","lat":1.0,"lon":2.0}]})";
    check(parse(braces.c_str(), braces.size(), one, 4) == 1 && std::strcmp(one[0].hex, "e") == 0,
          "brackets inside strings do not close anything");

    const std::string escaped =
        R"({"aircraft":[{"desc":"quote \" and \\ ","hex":"f","lat":1.0,"lon":2.0}]})";
    check(parse(escaped.c_str(), escaped.size(), one, 4) == 1 && std::strcmp(one[0].hex, "f") == 0,
          "escapes inside strings");

    const std::string long_name =
        R"({"aircraft":[{"flight":"ABCDEFGHIJKLMNOP","hex":"g","lat":1.0,"lon":2.0}]})";
    check(parse(long_name.c_str(), long_name.size(), one, 4) == 1 &&
              std::strlen(one[0].flight) == kFlightLen - 1,
          "an over-long callsign is truncated rather than overrunning");

    const std::string no_squawk = R"({"aircraft":[{"hex":"i","lat":1.0,"lon":2.0}]})";
    check(parse(no_squawk.c_str(), no_squawk.size(), one, 4) == 1 && one[0].squawk == -1,
          "a missing squawk stays unset rather than reading as zero");

    const std::string long_desc =
        R"({"aircraft":[{"desc":"AIRBUS A350-1041 EXTRA LONG NAME HERE","hex":"j",)"
        R"("lat":1.0,"lon":2.0}]})";
    check(parse(long_desc.c_str(), long_desc.size(), one, 4) == 1 &&
              std::strlen(one[0].desc) == kDescLen - 1,
          "an over-long description is truncated");

    const std::string other_key = R"({"ac":[{"hex":"h","lat":1.0,"lon":2.0}]})";
    check(parse(other_key.c_str(), other_key.size(), one, 4) == 1, "accepts the \"ac\" spelling");
}

void test_rejects()
{
    Aircraft one[4];

    const char *empty = "";
    check(parse(empty, 0, one, 4) == 0, "empty input");

    const std::string no_list = R"({"now":1,"resultCount":0})";
    check(parse(no_list.c_str(), no_list.size(), one, 4) == 0, "no aircraft list");

    const std::string empty_list = R"({"aircraft":[]})";
    check(parse(empty_list.c_str(), empty_list.size(), one, 4) == 0, "empty aircraft list");

    const std::string cut = R"({"aircraft":[{"hex":"a","lat":1.0,"lon":2.0},{"hex":"b",)";
    check(parse(cut.c_str(), cut.size(), one, 4) == 1,
          "a truncated response keeps what was whole");

    check(parse(nullptr, 10, one, 4) == 0, "null input");
    check(parse("{}", 2, nullptr, 4) == 0, "null output");
}

void test_route(const std::string &json)
{
    Details details{};
    check(parse_route(json.c_str(), json.size(), details), "reads a flight route");
    check(std::strcmp(details.airline, "KLM Royal Dutch Airlines") == 0, "airline");
    check(std::strcmp(details.origin_code, "NCL") == 0, "origin code");
    check(std::strcmp(details.origin_city, "Newcastle") == 0, "origin city");
    check(std::strcmp(details.dest_code, "AMS") == 0, "destination code");
    check(std::strcmp(details.dest_city, "Amsterdam") == 0, "destination city");
    check(std::strcmp(details.airline, "Newcastle Airport") != 0,
          "a nested name does not overwrite the airline");
}

void test_aircraft(const std::string &json)
{
    Details details{};
    check(parse_aircraft(json.c_str(), json.size(), details), "reads aircraft details");
    check(std::strcmp(details.manufacturer, "Embraer") == 0, "manufacturer");
    check(std::strcmp(details.model, "EMB-190 STD") == 0, "model");
    check(std::strcmp(details.owner, "KLM cityhopper") == 0, "owner");
    check(std::strncmp(details.photo_url, "https://airport-data.com/images/", 32) == 0,
          "photo url");
    check(details.has_aircraft, "aircraft half marked present");
    check(!details.has_route, "route half left alone");
}

void test_lookup_rejects()
{
    Details details{};

    const std::string unknown = R"({"response":"unknown callsign"})";
    check(!parse_route(unknown.c_str(), unknown.size(), details),
          "an unknown callsign is not a route");

    const std::string no_route = R"({"response":{"flightroute":{"callsign":"X"}}})";
    check(!parse_route(no_route.c_str(), no_route.size(), details),
          "a flight with no airports is not a route");

    const std::string cut = R"({"response":{"aircraft":{"type":"A320",)";
    check(!parse_aircraft(cut.c_str(), cut.size(), details), "a truncated lookup");

    check(!parse_route(nullptr, 0, details), "null route input");
    check(!parse_aircraft(nullptr, 0, details), "null aircraft input");
}

void test_photo(const std::string &json, const std::string &none)
{
    char url[160] = {};
    check(parse_photo(json.c_str(), json.size(), url, sizeof(url)), "a photo is found");
    check(std::strncmp(url, "https://", 8) == 0, "the photo address is a url");
    check(std::strstr(url, "\\") == nullptr, "the escapes are gone");
    check(std::strstr(url, "_280.jpg") != nullptr, "the larger thumbnail is taken");

    url[0] = 'x';
    check(!parse_photo(none.c_str(), none.size(), url, sizeof(url)),
          "an aircraft nobody has photographed");
    check(url[0] == '\0', "and it leaves nothing behind");

    check(!parse_photo(nullptr, 0, url, sizeof(url)), "null photo input");
}

}  // namespace

int main(int argc, char **argv)
{
    const std::string json = read_file(argc > 1 ? argv[1] : "adsb_sample.json");
    if (!json.empty()) {
        test_capture(json);
        test_capacity(json);
    }
    test_shapes();
    test_rejects();

    const std::string route = read_file(argc > 2 ? argv[2] : "adsbdb_route.json");
    if (!route.empty()) {
        test_route(route);
    }
    const std::string aircraft = read_file(argc > 3 ? argv[3] : "adsbdb_aircraft.json");
    if (!aircraft.empty()) {
        test_aircraft(aircraft);
    }
    test_lookup_rejects();

    const std::string photo = read_file(argc > 4 ? argv[4] : "planespotters_photo.json");
    const std::string none  = read_file(argc > 5 ? argv[5] : "planespotters_none.json");
    if (!photo.empty() && !none.empty()) {
        test_photo(photo, none);
    }
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
