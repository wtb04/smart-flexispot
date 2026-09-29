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
     "PlayState":{"IsPaused":false,"PositionTicks":1040000000,"CanSeek":true,"VolumeLevel":55},
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
    check(now.volume == 55, "how loud it is");
    check(now.kind == "Episode" && now.title == "The Fight" &&
              now.series == "Parks and Recreation" && now.series_id == "s1" && now.season_id == "se3" && now.season == 3 &&
              now.episode == 13,
          "what it is");

    const NowPlaying paused = now_playing(R"({"MessageType":"Sessions","Data":[
        {"Id":"tv","DeviceId":"tv1","PlayState":{"IsPaused":true},
         "NowPlayingItem":{"Id":"m1","Name":"A Film","Type":"Movie"}}]})", "smart-flexispot");
    check(paused.active && paused.paused && paused.item == "m1", "a paused one when none plays");
    check(paused.volume == -1, "a player that does not say how loud is left unknown");
    check(now.subtitle == -1 && now.subtitle_track == -1, "no subtitles to show");

    const NowPlaying subtitled = now_playing(R"({"MessageType":"Sessions","Data":[
        {"Id":"tv","DeviceId":"tv1","PlayState":{"SubtitleStreamIndex":4},
         "NowPlayingItem":{"Id":"m1","MediaStreams":[{"Type":"Video","Index":0},
           {"Type":"Subtitle","Index":3},{"Type":"Subtitle","Index":4,"IsDefault":true},
           {"Type":"Subtitle","Index":5}]}}]})", "smart-flexispot");
    check(subtitled.subtitle == 4 && subtitled.subtitle_track == 4, "the default subtitles, shown");
    const NowPlaying plain = now_playing(R"({"MessageType":"Sessions","Data":[
        {"Id":"tv","DeviceId":"tv1","PlayState":{"SubtitleStreamIndex":-1},
         "NowPlayingItem":{"Id":"m1","MediaStreams":[{"Type":"Audio","Index":1},
           {"Type":"Subtitle","Index":2}]}}]})", "smart-flexispot");
    check(plain.subtitle == -1 && plain.subtitle_track == 2, "the first subtitles, when none is the default");

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

void test_neighbours()
{
    check(neighbours_path("s1", "e14") == "/Shows/s1/Episodes?adjacentTo=e14",
          "the episodes either side are asked for");
    const Neighbours both = neighbours(
        R"({"Items":[{"Id":"e13","Name":"The Fight"},{"Id":"e14"},{"Id":"e15"}]})", "e14");
    check(both.previous == "e13" && both.next == "e15", "one before and one after");
    const Neighbours first = neighbours(R"({"Items":[{"Id":"e1"},{"Id":"e2"}]})", "e1");
    check(first.previous.empty() && first.next == "e2", "the series' first has none before");
    const Neighbours last = neighbours(R"({"Items":[{"Id":"e9"},{"Id":"e10"}]})", "e10");
    check(last.previous == "e9" && last.next.empty(), "its last none after");
    check(neighbours(R"({"Items":[{"Id":"x"}]})", "e14").next.empty() &&
              neighbours("{broken", "e14").previous.empty(),
          "an answer without the episode says nothing");
    check(play_now_path("835c", "e15") == "/Sessions/835c/Playing?playCommand=PlayNow&itemIds=e15",
          "an episode started in its place");
}

void test_paths()
{
    check(pause_path("835c", true) == "/Sessions/835c/Playing/Pause" &&
              pause_path("835c", false) == "/Sessions/835c/Playing/Unpause",
          "pause, or carry on, outright");
    check(seek_path("835c", 90) == "/Sessions/835c/Playing/Seek?SeekPositionTicks=900000000",
          "a jump, in ticks");
    check(command_path("835c") == "/Sessions/835c/Command", "a general command");
    check(has(set_volume_body(35), R"("Name":"SetVolume")") &&
              has(set_volume_body(35), R"("Arguments":{"Volume":"35"})"),
          "the volume set, as a string as the server takes it");
    check(has(set_subtitle_body(-1), R"("Name":"SetSubtitleStreamIndex")") &&
              has(set_subtitle_body(-1), R"("Arguments":{"Index":"-1"})"),
          "subtitles off");
    check(cover_path("6f76", 400) == "/Items/6f76/Images/Primary?maxHeight=400&format=Jpg&quality=85",
          "the cover, as a JPEG");
}
}  // namespace

int main()
{
    test_messages();
    test_now_playing();
    test_trimmer();
    test_neighbours();
    test_paths();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
