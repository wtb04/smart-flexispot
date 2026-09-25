#pragma once

#include <string>

// Favourites to start from the media card: Spotify links chosen by hand, their
// titles and covers asked of Spotify's embed service, which needs no account
// and so sees the Top 50s and Daily Mixes its API keeps from new apps. Played
// by Spotcast, which wakes a Cast speaker Spotify has lost sight of. Pure, so
// the host tests can run it.
namespace room::picks {
struct Pick {
    std::string uri;    // such as spotify:playlist:37i9dQZEVXbMDoHDwVN2tF
    std::string name;   // empty until the embed service has answered
    std::string image;  // the cover's address, on a Spotify CDN
};

/** Where the embed service describes `uri`; empty when it is not a Spotify URI. */
std::string embed_url(const std::string &uri);

/** Takes the title and cover from the embed service's answer; false without a title. */
bool take_embed(const std::string &answer, Pick &pick);

/** Plays `pick` from its start on `speaker`, a Cast player, waking it first. */
std::string play_request(const Pick &pick, const std::string &speaker);
}  // namespace room::picks
