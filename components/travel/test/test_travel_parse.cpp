#include "travel_parse.cpp"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int run(const std::string &text, travel::Option *out, int capacity)
{
    return travel::parse(text.data(), text.size(), out, capacity);
}

const char *kTwo = R"({
  "generatedAt": 1789105000,
  "options": [
    {"leaveAt": 1789105920, "arriveAt": 1789108140, "legs": [
        {"mode":"bus","line":"1","from":"Marktplein","to":"Enschede","dep":1789105920,"arr":1789106700},
        {"mode":"train","line":"Sprinter","from":"Enschede","to":"Kennispark","dep":1789107060,"arr":1789107300}
    ]},
    {"leaveAt": 1789106520, "arriveAt": 1789108740, "legs": [
        {"mode":"walk","line":"","from":"Home","to":"Marktplein","dep":1789106520,"arr":1789106700}
    ]}
  ]})";
}  // namespace

TEST(TravelParse, two_options)
{
    travel::Option options[travel::kOptionsMax]{};
    const int n = run(kTwo, options, travel::kOptionsMax);
    EXPECT_EQ(n, 2) << "two options";
    EXPECT_EQ(options[0].leave, 1789105920) << "leaveAt";
    EXPECT_EQ(options[0].arrive, 1789108140) << "arriveAt";
    EXPECT_EQ(options[0].leg_count, 2) << "two legs";
    EXPECT_EQ(std::strcmp(options[0].legs[0].mode, "bus"), 0) << "first leg mode";
    EXPECT_EQ(std::strcmp(options[0].legs[0].line, "1"), 0) << "first leg line";
    EXPECT_EQ(std::strcmp(options[0].legs[1].from, "Enschede"), 0) << "second leg from";
    EXPECT_EQ(options[0].legs[1].depart, 1789107060) << "second leg departure";
    EXPECT_EQ(options[1].leg_count, 1) << "second option has one leg";
    EXPECT_EQ(options[1].legs[0].line[0], '\0') << "an empty line stays empty";
}

TEST(TravelParse, a_leg_allowed_for_rather_than_known_says_so)
{
    const char body[] = R"({"options":[{"leaveAt":10,"arriveAt":90,"legs":[
        {"mode":"train","line":"SPR","from":"A","to":"B","dep":10,"arr":40},
        {"mode":"bus","line":"9","from":"B","to":"C","dep":45,"arr":75,"estimated":true}]}]})";
    travel::Option options[1];
    const int n = travel::parse(body, sizeof(body) - 1, options, 1);
    EXPECT_TRUE(n == 1 && !options[0].legs[0].estimated && options[0].legs[1].estimated) << "a leg allowed for rather than known says so";
}

TEST(TravelParse, both_options_read)
{
    travel::Option options[travel::kOptionsMax]{};
    // A field that only exists in the next option must not leak backwards.
    const std::string text =
        R"({"options":[{"leaveAt":100,"legs":[]},{"leaveAt":200,"arriveAt":999,"legs":[]}]})";
    const int n = run(text, options, travel::kOptionsMax);
    EXPECT_EQ(n, 2) << "both options read";
    EXPECT_TRUE(options[0].leave == 100 && options[0].arrive == 0) << "a missing field does not borrow from the next option";
    EXPECT_EQ(options[1].arrive, 999) << "the next option keeps its own";
}

TEST(TravelParse, escaped_quotes_parsed)
{
    travel::Option options[travel::kOptionsMax]{};
    const std::string text =
        R"({"options":[{"leaveAt":1,"legs":[{"mode":"bus","from":"A \"B\" C","to":"D"}]}]})";
    const int n = run(text, options, travel::kOptionsMax);
    EXPECT_TRUE(n == 1 && options[0].leg_count == 1) << "escaped quotes parsed";
    EXPECT_EQ(std::strcmp(options[0].legs[0].from, "A \"B\" C"), 0) << "escapes are undone";
}

TEST(TravelParse, capacity_is_respected)
{
    travel::Option options[travel::kOptionsMax]{};
    std::string many = R"({"options":[)";
    for (int i = 0; i < 6; ++i) {
        many += R"({"leaveAt":1,"legs":[]},)";
    }
    many.pop_back();
    many += "]}";
    const int n = run(many, options, travel::kOptionsMax);
    EXPECT_EQ(n, travel::kOptionsMax) << "capacity is respected";
}

TEST(TravelParse, leg_capacity_is_respected)
{
    travel::Option options[travel::kOptionsMax]{};
    std::string legs = R"({"options":[{"leaveAt":1,"legs":[)";
    for (int i = 0; i < 9; ++i) {
        legs += R"({"mode":"bus"},)";
    }
    legs.pop_back();
    legs += "]}]}";
    const int n = run(legs, options, travel::kOptionsMax);
    EXPECT_TRUE(n == 1 && options[0].leg_count == travel::kLegsMax) << "leg capacity is respected";
}

TEST(TravelParse, bodies_without_options_give_none)
{
    travel::Option options[travel::kOptionsMax]{};
    EXPECT_EQ(run("", options, travel::kOptionsMax), 0) << "empty body";
    EXPECT_EQ(run("not json at all", options, travel::kOptionsMax), 0) << "rubbish body";
    EXPECT_EQ(run(R"({"options":[]})", options, travel::kOptionsMax), 0) << "no options";
    EXPECT_EQ(run(R"({"error":"upstream down"})", options, travel::kOptionsMax), 0) << "an error body yields nothing";
    EXPECT_TRUE(run(R"({"options":[{"leaveAt":)", options, travel::kOptionsMax) >= 0) << "a truncated body does not run off the end";
}

TEST(TravelParse, a_cancelled_option_is_still_answered_with)
{
    travel::Option options[travel::kOptionsMax]{};
    const std::string text =
        R"({"options":[{"leaveAt":1,"off":true,"legs":[)"
        R"({"mode":"train","off":true,"dep":1},{"mode":"bus","dep":2}]}]})";
    const int n = run(text, options, travel::kOptionsMax);
    EXPECT_EQ(n, 1) << "a cancelled option is still answered with";
    EXPECT_TRUE(options[0].cancelled) << "the option counts as off";
    EXPECT_TRUE(options[0].legs[0].cancelled) << "the cancelled leg is marked";
    EXPECT_TRUE(!options[0].legs[1].cancelled) << "the running leg is not";
}

TEST(TravelParse, off_belongs_to_its_own_leg)
{
    travel::Option options[travel::kOptionsMax]{};
    // "off" belongs to the leg it sits in, not to whatever follows.
    const std::string text =
        R"({"options":[{"leaveAt":1,"legs":[{"mode":"bus","dep":1},)"
        R"({"mode":"train","off":true,"dep":2}]}]})";
    const int n = run(text, options, travel::kOptionsMax);
    EXPECT_EQ(n, 1) << "parsed";
    EXPECT_TRUE(!options[0].legs[0].cancelled && options[0].legs[1].cancelled) << "off belongs to the leg it sits in, not to the one before it";
    EXPECT_TRUE(options[0].cancelled) << "a journey with a leg off is off";
}

TEST(TravelParse, long_name_parsed)
{
    travel::Option options[travel::kOptionsMax]{};
    const std::string text =
        R"({"options":[{"leaveAt":5,"legs":[{"mode":"train","from":"AVeryLongStationNameThatGoesOnAndOnForever"}]}]})";
    const int n = run(text, options, travel::kOptionsMax);
    EXPECT_EQ(n, 1) << "long name parsed";
    EXPECT_EQ(std::strlen(options[0].legs[0].from), travel::kPlaceMax - 1) << "name truncated";
}

TEST(TravelParse, three_parsed)
{
    travel::Option options[travel::kOptionsMax]{};
    const std::string text =
        R"({"options":[{"leaveAt":9,"legs":[{"mode":"train","dep":9}],"late":true},)"
        R"({"leaveAt":5,"legs":[{"mode":"train","dep":5}],"late":false},)"
        R"({"leaveAt":3,"legs":[{"mode":"train","dep":3}]}]})";
    const int n = run(text, options, travel::kOptionsMax);
    EXPECT_EQ(n, 3) << "three parsed";
    EXPECT_TRUE(options[0].late && !options[1].late && !options[2].late) << "late is read per option, and absent means in time";
}
