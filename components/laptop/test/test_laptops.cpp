#include "laptops.h"

#include <gtest/gtest.h>

#include <string>

using namespace laptop;

namespace {
constexpr std::int64_t S         = 1000 * 1000;
constexpr std::int64_t PING      = 60 * S;
constexpr std::int64_t QUIET     = 30 * S;
const std::string      MAC       = "http://10.1.20.5:47801";
const std::string      LINUX     = "http://10.1.10.40:47801";

NowPlaying showing(const char *title, bool playing)
{
    NowPlaying now;
    now.title   = title;
    now.active  = title[0] != '\0';
    now.playing = now.active && playing;
    now.port    = 47801;
    return now;
}

NowPlaying playing(const char *title) { return showing(title, true); }
NowPlaying paused(const char *title) { return showing(title, false); }
NowPlaying nothing() { return showing("", false); }
}  // namespace

TEST(Laptops, a_playing_laptop_beats_a_paused_one)
{
    Laptops laptops;
    laptops.take(MAC, paused("Song"), 0, PING);
    laptops.take(LINUX, playing("Video"), 1 * S, PING);
    EXPECT_EQ(laptops.current_origin(), LINUX);
    laptops.take(MAC, paused("Song"), 2 * S, PING);
    EXPECT_EQ(laptops.current_origin(), LINUX) << "a paused laptop's heartbeat does not take it back";
}

TEST(Laptops, the_later_start_wins_and_heartbeats_never_switch)
{
    Laptops laptops;
    laptops.take(MAC, playing("Song"), 0, PING);
    laptops.take(LINUX, playing("Video"), 5 * S, PING);
    EXPECT_EQ(laptops.current_origin(), LINUX);
    for (int beat = 1; beat <= 3; ++beat) {
        laptops.take(LINUX, playing("Video"), (5 + 10 * beat) * S, PING);
        laptops.take(MAC, playing("Song"), (6 + 10 * beat) * S, PING);  // the Mac spoke last
        EXPECT_EQ(laptops.current_origin(), LINUX) << "beat " << beat;
    }
    laptops.take(MAC, paused("Song"), 40 * S, PING);
    laptops.take(MAC, playing("Song"), 41 * S, PING);
    EXPECT_EQ(laptops.current_origin(), MAC) << "pressing play there takes it over";
}

TEST(Laptops, both_paused_keeps_the_current)
{
    Laptops laptops;
    laptops.take(LINUX, playing("Video"), 0, PING);
    laptops.take(MAC, playing("Song"), 5 * S, PING);
    laptops.take(MAC, paused("Song"), 6 * S, PING);  // back to Linux, still playing
    EXPECT_EQ(laptops.current_origin(), LINUX);
    laptops.take(LINUX, paused("Video"), 7 * S, PING);
    EXPECT_EQ(laptops.current_origin(), LINUX) << "though the Mac began later, the panel keeps the one it had";
    laptops.take(MAC, paused("Song"), 16 * S, PING);
    EXPECT_EQ(laptops.current_origin(), LINUX);
}

TEST(Laptops, a_current_laptop_with_nothing_left_hands_over)
{
    Laptops laptops;
    laptops.take(LINUX, paused("Video"), 0, PING);
    laptops.take(MAC, playing("Song"), 1 * S, PING);
    laptops.take(MAC, nothing(), 2 * S, PING);
    EXPECT_EQ(laptops.current_origin(), LINUX);
    EXPECT_EQ(laptops.current_now().title, "Video");
    laptops.take(LINUX, nothing(), 3 * S, PING);
    EXPECT_EQ(laptops.current_origin(), "");
    EXPECT_FALSE(laptops.current_now().active);
}

TEST(Laptops, a_quiet_laptop_is_forgotten_and_the_other_takes_over)
{
    Laptops laptops;
    laptops.take(LINUX, paused("Video"), 0, PING);
    laptops.take(MAC, playing("Song"), 1 * S, PING);
    laptops.take(LINUX, paused("Video"), 25 * S, PING);
    EXPECT_FALSE(laptops.forget_quiet(30 * S, QUIET)) << "both still heard within 30 s";
    EXPECT_TRUE(laptops.forget_quiet(32 * S, QUIET));
    EXPECT_EQ(laptops.current_origin(), LINUX);
    EXPECT_EQ(laptops.count(), 1);
    EXPECT_FALSE(laptops.forget_quiet(40 * S, QUIET));
}

TEST(Laptops, a_full_table_drops_the_longest_quiet_that_is_not_followed)
{
    Laptops laptops;
    laptops.take("http://a:1", playing("A"), 0, PING);
    laptops.take("http://b:1", paused("B"), 1 * S, PING);
    laptops.take("http://c:1", paused("C"), 2 * S, PING);
    laptops.take("http://d:1", paused("D"), 3 * S, PING);
    laptops.take("http://e:1", paused("E"), 4 * S, PING);
    EXPECT_EQ(laptops.count(), Laptops::kMax);
    EXPECT_EQ(laptops.current_origin(), "http://a:1") << "the followed one is kept though heard first";
    EXPECT_EQ(laptops.reaches_of("http://b:1"), -2) << "b went, as the longest quiet";
}

TEST(Laptops, reaches_is_each_laptops_own)
{
    Laptops laptops;
    laptops.take(MAC, playing("Song"), 0, PING);
    laptops.take(LINUX, paused("Video"), 1 * S, PING);
    EXPECT_TRUE(laptops.reached(LINUX, true));
    EXPECT_FALSE(laptops.reached(LINUX, true)) << "unchanged";
    EXPECT_EQ(laptops.take(LINUX, paused("Video"), 2 * S, PING).reaches, 1);
    EXPECT_EQ(laptops.take(MAC, playing("Song"), 3 * S, PING).reaches, -1);
    laptops.reached(MAC, false);
    EXPECT_EQ(laptops.reaches_of(MAC), -1) << "one missed ping is not yet no";
    laptops.reached(MAC, false);
    EXPECT_EQ(laptops.reaches_of(MAC), 0);
    EXPECT_FALSE(laptops.reached("http://nobody:1", true));
}

TEST(Laptops, the_panels_own_play_counts_as_a_start)
{
    Laptops laptops;
    laptops.take(MAC, paused("Song"), 0, PING);
    laptops.take(LINUX, playing("Video"), 1 * S, PING);
    laptops.take(MAC, paused("Song"), 2 * S, PING);
    EXPECT_EQ(laptops.current_origin(), LINUX);
    laptops.take(LINUX, paused("Video"), 3 * S, PING);
    EXPECT_TRUE(laptops.set_playing(true, 4 * S));
    EXPECT_TRUE(laptops.current_now().playing);
    laptops.take(MAC, paused("Song"), 5 * S, PING);
    EXPECT_EQ(laptops.current_origin(), LINUX);
    Laptops empty;
    EXPECT_FALSE(empty.set_playing(true, 0));
}

TEST(Laptops, pings_when_new_and_then_every_minute)
{
    Laptops laptops;
    EXPECT_TRUE(laptops.take(MAC, playing("Song"), 0, PING).ping);
    EXPECT_FALSE(laptops.take(MAC, playing("Song"), 10 * S, PING).ping);
    EXPECT_TRUE(laptops.take(MAC, playing("Song"), 61 * S, PING).ping);
    EXPECT_TRUE(laptops.take(LINUX, paused("Video"), 62 * S, PING).ping) << "each on its own schedule";
}

TEST(Laptops, tells_the_followed_laptops_states_and_a_change_of_laptop_only)
{
    Laptops laptops;
    EXPECT_TRUE(laptops.take(MAC, playing("Song"), 0, PING).tell);
    EXPECT_TRUE(laptops.take(MAC, playing("Song"), 10 * S, PING).tell) << "its position moves on";
    EXPECT_FALSE(laptops.take(LINUX, paused("Video"), 11 * S, PING).tell);
    EXPECT_TRUE(laptops.take(LINUX, playing("Video"), 12 * S, PING).tell) << "it took over";
    EXPECT_FALSE(laptops.take(MAC, playing("Song"), 20 * S, PING).tell);
}

TEST(Laptops, a_new_port_is_a_new_laptop)
{
    Laptops laptops;
    laptops.take("http://10.1.10.40:47801", playing("Video"), 0, PING);
    laptops.take("http://10.1.10.40:47802", playing("Video"), 1 * S, PING);
    EXPECT_EQ(laptops.count(), 2);
    EXPECT_EQ(laptops.current_origin(), "http://10.1.10.40:47802");
}

TEST(Laptops, a_laptop_at_a_new_address_takes_its_own_row_over)
{
    Laptops    laptops;
    NowPlaying before = paused("Song");
    before.machine    = "CloudPad";
    laptops.take("http://10.1.10.40:47801", before, 0, PING);
    laptops.reached("http://10.1.10.40:47801", true);
    NowPlaying after = paused("Song");
    after.machine    = "CloudPad";  // Desk Link restarted, or the laptop docked onto another network
    const Taken taken = laptops.take("http://10.1.20.77:47801", after, 1 * S, PING);
    EXPECT_EQ(laptops.count(), 1);
    EXPECT_EQ(laptops.current_origin(), "http://10.1.20.77:47801");
    EXPECT_TRUE(taken.tell);
    EXPECT_TRUE(taken.ping) << "the new address is tried back";
    EXPECT_EQ(taken.reaches, -1) << "what was known of the old address says nothing of the new";
}

TEST(Laptops, counts_those_sharing_as_they_join_and_go_quiet)
{
    Laptops laptops;
    EXPECT_EQ(laptops.count(), 0);
    laptops.take(MAC, nothing(), 0, PING);
    EXPECT_EQ(laptops.count(), 1) << "sharing with nothing playing is sharing all the same";
    laptops.take(MAC, playing("Song"), 10 * S, PING);
    EXPECT_EQ(laptops.count(), 1) << "a heartbeat is the same laptop";
    laptops.take(LINUX, paused("Video"), 12 * S, PING);
    EXPECT_EQ(laptops.count(), 2);
    laptops.take(MAC, playing("Song"), 20 * S, PING);
    EXPECT_FALSE(laptops.forget_quiet(43 * S, QUIET)) << "Linux went quiet, but the Mac is still followed";
    EXPECT_EQ(laptops.count(), 1);
    EXPECT_EQ(laptops.current_origin(), MAC);
    EXPECT_TRUE(laptops.forget_quiet(51 * S, QUIET));
    EXPECT_EQ(laptops.count(), 0);
}
