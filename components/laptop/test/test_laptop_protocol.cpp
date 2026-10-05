#include "laptop_protocol.h"

#include <gtest/gtest.h>

#include <string>

using laptop::Command;
using laptop::NowPlaying;

namespace {
NowPlaying read(const std::string &body)
{
    NowPlaying out;
    EXPECT_TRUE(laptop::read(body.data(), body.size(), out)) << body;
    return out;
}
}  // namespace

TEST(LaptopProtocol, reads_a_video_playing)
{
    const NowPlaying now = read(
        R"j({"machine":"wtb-mbp","port":47801,"app":"Safari","playing":true,"title":"Aart (86)",)j"
        R"j("artist":"RTV Oost","position":167.4,"duration":540.06,"art":"3f2a","video":true,)j"
        R"j("volume":40,"muted":true,"takes":["pause","seek","next","volume"]})j");
    EXPECT_TRUE(now.active);
    EXPECT_TRUE(now.playing);
    EXPECT_EQ(now.machine, "wtb-mbp");
    EXPECT_EQ(now.port, 47801);
    EXPECT_EQ(now.app, "Safari");
    EXPECT_EQ(now.title, "Aart (86)");
    EXPECT_EQ(now.artist, "RTV Oost");
    EXPECT_EQ(now.position_s, 167);
    EXPECT_EQ(now.duration_s, 540);
    EXPECT_EQ(now.art, "3f2a");
    EXPECT_TRUE(now.video);
    EXPECT_TRUE(now.takes_pause);
    EXPECT_TRUE(now.takes_seek);
    EXPECT_TRUE(now.takes_next);
    EXPECT_FALSE(now.takes_previous) << "only what it lists";
    EXPECT_TRUE(now.takes_volume);
    EXPECT_EQ(now.volume, 40);
    EXPECT_TRUE(now.muted);
}

TEST(LaptopProtocol, an_empty_title_is_nothing_playing)
{
    const NowPlaying now = read(R"({"port":47801,"title":"","playing":true})");
    EXPECT_FALSE(now.active);
    EXPECT_FALSE(now.playing) << "nothing to be playing";
}

TEST(LaptopProtocol, paused_and_without_a_length)
{
    const NowPlaying now = read(R"({"port":47801,"title":"Live","playing":false,"duration":-1})");
    EXPECT_TRUE(now.active);
    EXPECT_FALSE(now.playing);
    EXPECT_EQ(now.duration_s, 0) << "a live stream has none";
    EXPECT_EQ(now.volume, -1) << "a Mac that cannot set its output says none";
    EXPECT_FALSE(now.takes_volume);
    EXPECT_FALSE(now.takes_volume_step);
    EXPECT_FALSE(now.video) << "music unless it says";
}

TEST(LaptopProtocol, refuses_what_is_not_a_report)
{
    NowPlaying out;
    for (const std::string body : {std::string("not json"), std::string("[1]"), std::string(R"({"title":"x"})"),
                                   std::string(R"({"port":0,"title":"x"})"),
                                   std::string(R"({"port":70000,"title":"x"})"),
                                   std::string(R"({"port":"47801","title":"x"})")}) {
        EXPECT_FALSE(laptop::read(body.data(), body.size(), out)) << body;
    }
}

TEST(LaptopProtocol, commands)
{
    EXPECT_EQ(laptop::command_body(Command::Pause, 0, 5), R"({"type":"command","at":5,"command":"pause"})");
    EXPECT_EQ(laptop::command_body(Command::Play, 0, 5), R"({"type":"command","at":5,"command":"play"})");
    EXPECT_EQ(laptop::command_body(Command::Next, 0, 5), R"({"type":"command","at":5,"command":"next"})");
    EXPECT_EQ(laptop::command_body(Command::Seek, 90, 5),
              R"({"type":"command","at":5,"command":"seek","position":90})");
    EXPECT_EQ(laptop::command_body(Command::Volume, 35, 5),
              R"({"type":"command","at":5,"command":"volume","level":35})");
    EXPECT_EQ(laptop::command_body(Command::Mute, 1, 5), R"({"type":"command","at":5,"command":"mute","muted":true})");
}

TEST(LaptopProtocol, a_volume_that_only_steps)
{
    const NowPlaying now = read(R"({"port":47801,"title":"x","takes":["pause","volume_step"]})");
    EXPECT_TRUE(now.takes_volume_step);
    EXPECT_FALSE(now.takes_volume) << "no level to set or show";
    EXPECT_EQ(now.volume, -1);
    EXPECT_EQ(laptop::command_body(Command::VolumeUp, 0, 5), R"({"type":"command","at":5,"command":"volume_up"})");
    EXPECT_EQ(laptop::command_body(Command::VolumeDown, 0, 5),
              R"({"type":"command","at":5,"command":"volume_down"})");
}

TEST(LaptopProtocol, head)
{
    laptop::Head      head;
    const std::string state = R"({"type":"state","at":1791223792000,"port":47801})";
    ASSERT_TRUE(laptop::read_head(state.data(), state.size(), head));
    EXPECT_EQ(head.type, "state");
    EXPECT_EQ(head.at_ms, 1791223792000) << "milliseconds, past what an int holds";
    const std::string untimed = R"({"type":"state"})";
    EXPECT_FALSE(laptop::read_head(untimed.data(), untimed.size(), head)) << "without when, it cannot be fresh";
}

TEST(LaptopProtocol, answers)
{
    EXPECT_EQ(laptop::pong_body("smart-flexispot", "v0.9.0", 1, 7),
              R"({"type":"pong","at":7,"panel":"smart-flexispot","firmware":"v0.9.0","reaches":true})");
    EXPECT_EQ(laptop::pong_body("p", "f", -1, 7), R"({"type":"pong","at":7,"panel":"p","firmware":"f"})")
        << "not said before the panel has tried";
    EXPECT_EQ(laptop::ping_body(7), R"({"type":"ping","at":7})");
    EXPECT_EQ(laptop::ok_body(7), R"({"type":"ok","at":7})");
    EXPECT_EQ(laptop::cover_body("3f2a", 7), R"({"type":"cover","at":7,"art":"3f2a"})");
}

TEST(LaptopProtocol, claude_event)
{
    const std::string message = R"({"type":"claude","at":7,"event":{"machine":"wtb-mbp","event":"Stop"}})";
    EXPECT_EQ(laptop::claude_event(message.data(), message.size()), R"({"machine":"wtb-mbp","event":"Stop"})");
    const std::string empty = R"({"type":"claude","at":7})";
    EXPECT_EQ(laptop::claude_event(empty.data(), empty.size()), "");
}
