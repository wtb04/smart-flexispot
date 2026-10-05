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
        R"j("artist":"RTV Oost","position":167.4,"duration":540.06,"art":"3f2a",)j"
        R"j("takes":["pause","seek","next"]})j");
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
    EXPECT_TRUE(now.takes_pause);
    EXPECT_TRUE(now.takes_seek);
    EXPECT_TRUE(now.takes_next);
    EXPECT_FALSE(now.takes_previous) << "only what it lists";
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
    EXPECT_EQ(laptop::command_body(Command::Pause), R"({"command":"pause"})");
    EXPECT_EQ(laptop::command_body(Command::Play), R"({"command":"play"})");
    EXPECT_EQ(laptop::command_body(Command::Next), R"({"command":"next"})");
    EXPECT_EQ(laptop::command_body(Command::Previous), R"({"command":"previous"})");
    EXPECT_EQ(laptop::command_body(Command::Seek, 90), R"({"command":"seek","position":90})");
}

TEST(LaptopProtocol, art_path)
{
    EXPECT_EQ(laptop::art_path("3f2a"), "/art.jpg?v=3f2a");
    EXPECT_EQ(laptop::art_path(""), "") << "no picture, nothing to fetch";
}
