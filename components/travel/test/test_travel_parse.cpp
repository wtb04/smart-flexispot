#include "travel_parse.cpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures = 0;
int checks   = 0;

void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

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

int main()
{
    travel::Option options[travel::kOptionsMax]{};

    {
        const int n = run(kTwo, options, travel::kOptionsMax);
        check(n == 2, "two options");
        check(options[0].leave == 1789105920, "leaveAt");
        check(options[0].arrive == 1789108140, "arriveAt");
        check(options[0].leg_count == 2, "two legs");
        check(std::strcmp(options[0].legs[0].mode, "bus") == 0, "first leg mode");
        check(std::strcmp(options[0].legs[0].line, "1") == 0, "first leg line");
        check(std::strcmp(options[0].legs[1].from, "Enschede") == 0, "second leg from");
        check(options[0].legs[1].depart == 1789107060, "second leg departure");
        check(options[1].leg_count == 1, "second option has one leg");
        check(options[1].legs[0].line[0] == '\0', "an empty line stays empty");
    }

    {
        // A field that only exists in the next option must not leak backwards.
        const std::string text =
            R"({"options":[{"leaveAt":100,"legs":[]},{"leaveAt":200,"arriveAt":999,"legs":[]}]})";
        const int n = run(text, options, travel::kOptionsMax);
        check(n == 2, "both options read");
        check(options[0].leave == 100 && options[0].arrive == 0,
              "a missing field does not borrow from the next option");
        check(options[1].arrive == 999, "the next option keeps its own");
    }

    {
        const std::string text =
            R"({"options":[{"leaveAt":1,"legs":[{"mode":"bus","from":"A \"B\" C","to":"D"}]}]})";
        const int n = run(text, options, travel::kOptionsMax);
        check(n == 1 && options[0].leg_count == 1, "escaped quotes parsed");
        check(std::strcmp(options[0].legs[0].from, "A \"B\" C") == 0, "escapes are undone");
    }

    {
        std::string many = R"({"options":[)";
        for (int i = 0; i < 6; ++i) {
            many += R"({"leaveAt":1,"legs":[]},)";
        }
        many.pop_back();
        many += "]}";
        const int n = run(many, options, travel::kOptionsMax);
        check(n == travel::kOptionsMax, "capacity is respected");
    }

    {
        std::string legs = R"({"options":[{"leaveAt":1,"legs":[)";
        for (int i = 0; i < 9; ++i) {
            legs += R"({"mode":"bus"},)";
        }
        legs.pop_back();
        legs += "]}]}";
        const int n = run(legs, options, travel::kOptionsMax);
        check(n == 1 && options[0].leg_count == travel::kLegsMax, "leg capacity is respected");
    }

    {
        check(run("", options, travel::kOptionsMax) == 0, "empty body");
        check(run("not json at all", options, travel::kOptionsMax) == 0, "rubbish body");
        check(run(R"({"options":[]})", options, travel::kOptionsMax) == 0, "no options");
        check(run(R"({"error":"upstream down"})", options, travel::kOptionsMax) == 0,
              "an error body yields nothing");
        check(run(R"({"options":[{"leaveAt":)", options, travel::kOptionsMax) >= 0,
              "a truncated body does not run off the end");
    }

    {
        const std::string text =
            R"({"options":[{"leaveAt":1,"off":true,"legs":[)"
            R"({"mode":"train","off":true,"dep":1},{"mode":"bus","dep":2}]}]})";
        const int n = run(text, options, travel::kOptionsMax);
        check(n == 1, "a cancelled option is still answered with");
        check(options[0].cancelled, "the option counts as off");
        check(options[0].legs[0].cancelled, "the cancelled leg is marked");
        check(!options[0].legs[1].cancelled, "the running leg is not");
    }

    {
        // "off" belongs to the leg it sits in, not to whatever follows.
        const std::string text =
            R"({"options":[{"leaveAt":1,"legs":[{"mode":"bus","dep":1},)"
            R"({"mode":"train","off":true,"dep":2}]}]})";
        const int n = run(text, options, travel::kOptionsMax);
        check(n == 1, "parsed");
        check(!options[0].legs[0].cancelled && options[0].legs[1].cancelled,
              "off belongs to the leg it sits in, not to the one before it");
        check(options[0].cancelled, "a journey with a leg off is off");
    }

    {
        const std::string text =
            R"({"options":[{"leaveAt":5,"legs":[{"mode":"train","from":"AVeryLongStationNameThatGoesOnAndOnForever"}]}]})";
        const int n = run(text, options, travel::kOptionsMax);
        check(n == 1, "long name parsed");
        check(std::strlen(options[0].legs[0].from) == travel::kPlaceMax - 1, "name truncated");
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        std::printf("ALL PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
