#include "../picks.h"

#include <cstdio>
#include <string>

namespace {
using namespace room::picks;

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

void test_embed_url()
{
    check(embed_url("spotify:playlist:37i9dQZEVXbMDoHDwVN2tF") ==
              "https://open.spotify.com/oembed?url=https://open.spotify.com/playlist/"
              "37i9dQZEVXbMDoHDwVN2tF",
          "a playlist is described at its open.spotify.com page");
    check(embed_url("spotify:album:1cJOcjPpaDbGxe7qDfA5Cd") ==
              "https://open.spotify.com/oembed?url=https://open.spotify.com/album/"
              "1cJOcjPpaDbGxe7qDfA5Cd",
          "and so is an album");
    check(embed_url("").empty() && embed_url("https://open.spotify.com/x").empty() &&
              embed_url("spotify:playlist").empty() && embed_url("spotify::x").empty() &&
              embed_url("spotify:playlist:").empty(),
          "what is not a Spotify URI has no page");
}

void test_take_embed()
{
    Pick pick{"spotify:playlist:37i9dQZEVXbMDoHDwVN2tF", "", ""};
    check(take_embed(R"({"html":"<iframe/>","title":"Top 50 - Global","thumbnail_url":
        "https://charts-images.scdn.co/region_global_default.jpg","thumbnail_width":300})", pick),
          "an answer with a title is taken");
    check(pick.name == "Top 50 - Global" &&
              pick.image == "https://charts-images.scdn.co/region_global_default.jpg",
          "its title and cover");

    Pick bare{"spotify:playlist:x", "", ""};
    check(!take_embed(R"({"error":"not found"})", bare) && !take_embed("<html>", bare) &&
              bare.name.empty(),
          "an answer without a title leaves it unnamed");
}

void test_play()
{
    const Pick        pick{"spotify:playlist:0Nni", "Techno", ""};
    const std::string play = play_request(pick, "media_player.office_speaker");
    check(has(play, R"("domain":"spotcast")") && has(play, R"("service":"play_media")"),
          "Spotcast plays it, waking the speaker");
    check(has(play, R"("media_player":{"entity_id":"media_player.office_speaker"})") &&
              has(play, R"("spotify_uri":"spotify:playlist:0Nni")"),
          "this playlist, on the speaker's Cast player");
}
}  // namespace

int main()
{
    test_embed_url();
    test_take_embed();
    test_play();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
