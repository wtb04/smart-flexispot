#pragma once

#include <string>
#include <vector>

// An episode's media segments, as Jellyfin keeps them from 10.10 on, most often
// filled in by the Intro Skipper plugin: where its intro and credits are, so the
// media card can offer skipping the one and going on after the other. Pure, so
// the host tests can run it.
namespace room::segments {
enum class Kind { Intro, Credits };

struct Segment {
    Kind kind;
    int  start_s;
    int  end_s;
};

/** The path, after the server's address, that lists `item`'s segments. */
std::string path_for(const std::string &item);

/** What the server's answer lists, the intro and recap as Intro, the credits and
 *  the preview after them as Credits, anything else left out. */
std::vector<Segment> parse(const std::string &answer);
}  // namespace room::segments
