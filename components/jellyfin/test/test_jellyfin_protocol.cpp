#include "jellyfin_protocol.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {
using namespace jellyfin;

int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

bool has(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

void test_messages()
{
    check(has(sessions_start(1000), R"("MessageType":"SessionsStart")") &&
              has(sessions_start(1000), R"("Data":"0,1000")"),
          "sessions asked for at once, then each second");
    check(has(keep_alive(), R"("MessageType":"KeepAlive")"), "a keep-alive");
    check(message_type(R"({"MessageType":"ForceKeepAlive","Data":60})") == "ForceKeepAlive" &&
              message_type("not json").empty(),
          "a message's type");
}

constexpr char SESSIONS[] = R"({"MessageType":"Sessions","Data":[
    {"Id":"panel","DeviceId":"smart-flexispot","NowPlayingItem":{"Id":"x","Name":"Own"}},
    {"Id":"tv","DeviceId":"tv1","PlayState":{"IsPaused":true,"PositionTicks":100000000},
     "NowPlayingItem":{"Id":"m1","Name":"A Film","Type":"Movie","RunTimeTicks":72000000000}},
    {"Id":"idle","DeviceId":"phone"},
    {"Id":"835c9059","DeviceId":"mac","DeviceName":"MacBook Pro",
     "PlayState":{"IsPaused":false,"PositionTicks":1040000000,"CanSeek":true},
     "NowPlayingItem":{"Id":"6f76","Name":"The Fight","Type":"Episode",
       "SeriesName":"Parks and Recreation","SeriesId":"s1","SeasonId":"se3","ParentIndexNumber":3,"IndexNumber":13,
       "RunTimeTicks":15040000000}}]})";

void test_now_playing()
{
    const NowPlaying now = now_playing(SESSIONS, "smart-flexispot");
    check(now.active && now.session == "835c9059" && now.item == "6f76",
          "the playing session is followed, before a paused one, never the panel's own");
    check(!now.paused && now.position_s == 104 && now.duration_s == 1504,
          "where it is, in seconds");
    check(now.kind == "Episode" && now.title == "The Fight" &&
              now.series == "Parks and Recreation" && now.series_id == "s1" && now.season_id == "se3" && now.season == 3 &&
              now.episode == 13,
          "what it is");

    const NowPlaying paused = now_playing(R"({"MessageType":"Sessions","Data":[
        {"Id":"tv","DeviceId":"tv1","PlayState":{"IsPaused":true},
         "NowPlayingItem":{"Id":"m1","Name":"A Film","Type":"Movie"}}]})", "smart-flexispot");
    check(paused.active && paused.paused && paused.item == "m1", "a paused one when none plays");

    check(!now_playing(R"({"MessageType":"Sessions","Data":[{"Id":"idle"}]})", "p").active &&
              !now_playing("{broken", "p").active,
          "nothing playing, nothing followed");
}

void test_trimmer()
{
    const std::string message =
        R"({"Data":[{"Id":"a","NowPlayingQueueFullItems":[{"Name":"x ]} \" [","Id":"1"},)"
        R"({"Name":"y","Streams":[{"A":1}]}],"NowPlayingItem":{"Id":"6f76","Name":"The Fight"}}]})";
    // Fed in awkward pieces, as frames split it.
    for (std::size_t piece : {1u, 3u, 7u, 1000u}) {
        Trimmer     trim;
        std::string out;
        for (std::size_t at = 0; at < message.size(); at += piece) {
            trim.feed(message.data() + at, std::min(piece, message.size() - at), out);
        }
        const bool kept = out == R"({"Data":[{"Id":"a","NowPlayingQueueFullItems":null,)"
                                 R"("NowPlayingItem":{"Id":"6f76","Name":"The Fight"}}]})";
        check(kept, "the queue's items are dropped, whatever its titles hold and however it arrives");
    }

    Trimmer     trim;
    std::string out;
    const std::string untouched = R"({"Name":"\"NowPlayingQueueFullItems\":[1]","B":null})";
    trim.feed(untouched.data(), untouched.size(), out);
    check(out == untouched, "the key's name inside a string is left alone");

    trim.reset();
    out.clear();
    const std::string empty = R"({"NowPlayingQueueFullItems":null,"C":1})";
    trim.feed(empty.data(), empty.size(), out);
    check(out == empty, "a null queue passes as it is");
}

void test_paths()
{
    check(play_pause_path("835c") == "/Sessions/835c/Playing/PlayPause", "pause or play");
    check(seek_path("835c", 90) == "/Sessions/835c/Playing/Seek?SeekPositionTicks=900000000",
          "a jump, in ticks");
    check(cover_path("6f76", 400) == "/Items/6f76/Images/Primary?maxHeight=400&format=Jpg&quality=85",
          "the cover, as a JPEG");
}
}  // namespace

int main()
{
    test_messages();
    test_now_playing();
    test_trimmer();
    test_paths();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
