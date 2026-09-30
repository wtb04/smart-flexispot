#include "../segments.h"

#include <gtest/gtest.h>
#include <cstdio>

namespace {
using namespace room::segments;

TEST(Segments, path)
{
    EXPECT_EQ(path_for("6f7678e7dd43ff72908151c0f5126b75"), "/MediaSegments/6f7678e7dd43ff72908151c0f5126b75") << "an episode's segments are asked for by its id";
}

TEST(Segments, parse)
{
    const auto found = parse(R"({"Items":[
        {"Type":"Intro","StartTicks":0,"EndTicks":452000000},
        {"Type":"Commercial","StartTicks":6000000000,"EndTicks":6300000000},
        {"Type":"Recap","StartTicks":470000000,"EndTicks":900000000},
        {"Type":"Outro","StartTicks":14200000000,"EndTicks":15030000000},
        {"Type":"Intro","StartTicks":50,"EndTicks":10},
        {"Type":"Preview","StartTicks":15030000000,"EndTicks":15300000000}],
        "TotalRecordCount":6})");
    EXPECT_EQ(found.size(), 4) << "commercials and backwards segments are left out";
    EXPECT_TRUE(found[0].kind == Kind::Intro && found[0].start_s == 0 && found[0].end_s == 45) << "the intro, in whole seconds";
    EXPECT_TRUE(found[1].kind == Kind::Intro && found[1].start_s == 47 && found[1].end_s == 90) << "a recap is skipped as an intro is";
    EXPECT_TRUE(found[2].kind == Kind::Credits && found[2].start_s == 1420 && found[2].end_s == 1503) << "the credits";
    EXPECT_EQ(found[3].kind, Kind::Credits) << "the preview after them counts as credits";

    EXPECT_TRUE(parse("").empty() && parse("{broken").empty() && parse(R"({"Items":[]})").empty()) << "nothing to read, no segments";
}
}  // namespace

