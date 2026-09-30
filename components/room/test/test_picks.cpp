#include "../picks.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <string>

namespace {
using namespace room::picks;

bool has(const std::string &text, const char *part)
{
    return text.find(part) != std::string::npos;
}

TEST(Picks, embed_url)
{
    EXPECT_EQ(embed_url("spotify:playlist:37i9dQZEVXbMDoHDwVN2tF"), "https://open.spotify.com/oembed?url=https://open.spotify.com/playlist/"
              "37i9dQZEVXbMDoHDwVN2tF") << "a playlist is described at its open.spotify.com page";
    EXPECT_EQ(embed_url("spotify:album:1cJOcjPpaDbGxe7qDfA5Cd"), "https://open.spotify.com/oembed?url=https://open.spotify.com/album/"
              "1cJOcjPpaDbGxe7qDfA5Cd") << "and so is an album";
    EXPECT_TRUE(embed_url("").empty() && embed_url("https://open.spotify.com/x").empty() &&
              embed_url("spotify:playlist").empty() && embed_url("spotify::x").empty() &&
              embed_url("spotify:playlist:").empty()) << "what is not a Spotify URI has no page";
}

TEST(Picks, take_embed)
{
    Pick pick{"spotify:playlist:37i9dQZEVXbMDoHDwVN2tF", "", ""};
    EXPECT_TRUE(take_embed(R"({"html":"<iframe/>","title":"Top 50 - Global","thumbnail_url":
        "https://charts-images.scdn.co/region_global_default.jpg","thumbnail_width":300})", pick)) << "an answer with a title is taken";
    EXPECT_TRUE(pick.name == "Top 50 - Global" &&
              pick.image == "https://charts-images.scdn.co/region_global_default.jpg") << "its title and cover";

    Pick bare{"spotify:playlist:x", "", ""};
    EXPECT_TRUE(!take_embed(R"({"error":"not found"})", bare) && !take_embed("<html>", bare) &&
              bare.name.empty()) << "an answer without a title leaves it unnamed";
}

TEST(Picks, play)
{
    const Pick        pick{"spotify:playlist:0Nni", "Techno", ""};
    const std::string play = play_request(pick, "media_player.office_speaker");
    EXPECT_TRUE(has(play, R"("domain":"spotcast")") && has(play, R"("service":"play_media")")) << "Spotcast plays it, waking the speaker";
    EXPECT_TRUE(has(play, R"("media_player":{"entity_id":"media_player.office_speaker"})") &&
              has(play, R"("spotify_uri":"spotify:playlist:0Nni")")) << "this playlist, on the speaker's Cast player";
}
}  // namespace

