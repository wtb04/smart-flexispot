#include "claude_sessions.h"

#include <gtest/gtest.h>

#include <string>

using namespace claude;

namespace {
constexpr std::int64_t min = 60 * 1000;

Event event(const char *session, const char *name, const char *tool = "", const char *target = "")
{
    Event e;
    e.machine = "MbP";
    e.session = session;
    e.project = "tab5-hello";
    e.event   = name;
    e.tool    = tool;
    e.target  = target;
    return e;
}

Snapshot shot(const Table &table, std::int64_t now)
{
    Snapshot out;
    table.snapshot(now, out);
    return out;
}

std::string said(const Action &action, Tense tense)
{
    char text[64];
    describe(action, tense, text, sizeof(text));
    return text;
}
}  // namespace

TEST(ClaudeParse, an_event_as_the_hook_sends_it)
{
    Event e;
    ASSERT_TRUE(parse(R"({"machine":"MbP","session":"s1","project":"tab5-hello","event":"PreToolUse",
                          "tool":"Edit","target":"rail.cpp"})", e));
    EXPECT_EQ(e.machine, "MbP");
    EXPECT_EQ(e.session, "s1");
    EXPECT_EQ(e.event, "PreToolUse");
    EXPECT_EQ(e.tool, "Edit");
    EXPECT_EQ(e.target, "rail.cpp");
    EXPECT_EQ(e.steps_total, -1) << "no step list unless one is sent";
}

TEST(ClaudeParse, what_is_no_event)
{
    Event e;
    EXPECT_FALSE(parse("not json", e));
    EXPECT_FALSE(parse(R"({"event":"Stop"})", e)) << "no session";
    EXPECT_FALSE(parse(R"({"session":"s1"})", e)) << "no event";
}

TEST(ClaudeParse, the_steps_round_the_one_under_way)
{
    std::string items;
    for (int i = 0; i < 9; ++i) {
        const char *state = i < 6 ? "done" : i == 6 ? "now" : "next";
        items += std::string(i ? "," : "") + R"({"state":")" + state + R"(","text":"step )" + std::to_string(i) + "\"}";
    }
    Event e;
    ASSERT_TRUE(parse(R"({"session":"s1","event":"PreToolUse","tool":"TodoWrite","steps":{"done":6,"total":9,"items":[)" +
                          items + "]}}",
                      e));
    EXPECT_EQ(e.steps_done, 6);
    EXPECT_EQ(e.steps_total, 9);
    ASSERT_EQ(e.step_count, kStepCount);
    EXPECT_STREQ(e.steps[0].text, "step 4") << "two done before the one under way";
    EXPECT_EQ(e.steps[2].step, Step::Now);
    EXPECT_STREQ(e.steps[4].text, "step 8");
}

TEST(ClaudeWords, what_a_tool_does)
{
    EXPECT_EQ(act_of("Edit"), Act::Edit);
    EXPECT_EQ(act_of("Write"), Act::Edit);
    EXPECT_EQ(act_of("Bash"), Act::Run);
    EXPECT_EQ(act_of("Grep"), Act::Search);
    EXPECT_EQ(act_of("Agent"), Act::Agents);
    EXPECT_EQ(act_of("WebFetch"), Act::Web);
    EXPECT_EQ(act_of("mcp__linear__search"), Act::Other);
}

TEST(ClaudeWords, said_as_it_happens_as_it_happened_and_as_it_waits)
{
    const Action edit{Act::Edit, "rail.cpp", 0};
    EXPECT_EQ(said(edit, Tense::Now), "Editing rail.cpp");
    EXPECT_EQ(said(edit, Tense::Past), "Edited rail.cpp");
    EXPECT_EQ(said(Action{Act::Run, "idf.py", 0}, Tense::Wanted), "wants to run idf.py");
    EXPECT_EQ(said(Action{Act::Run, "", 0}, Tense::Now), "Running a command") << "a name left out";
    EXPECT_EQ(said(Action{}, Tense::Now), "");
}

TEST(ClaudeTable, a_prompt_sets_it_working_and_its_tools_say_what_it_does)
{
    Table table;
    table.apply(event("s1", "UserPromptSubmit"), 0);
    for (int i = 0; i < kLatelyCount + 2; ++i) {
        table.apply(event("s1", "PreToolUse", "Edit", ("f" + std::to_string(i)).c_str()), 1 * min);
    }
    const Session &s = shot(table, 2 * min).sessions[0];
    EXPECT_EQ(s.state, State::Working);
    EXPECT_EQ(std::string(s.doing.what), "f" + std::to_string(kLatelyCount + 1));
    ASSERT_EQ(s.lately_count, kLatelyCount) << "only the last few";
    EXPECT_STREQ(s.lately[0].what, s.doing.what) << "newest first";
    EXPECT_EQ(std::string(s.lately[kLatelyCount - 1].what), "f2");
}

TEST(ClaudeTable, a_step_list_written_is_no_action)
{
    Table table;
    table.apply(event("s1", "PreToolUse", "Read", "ui.h"), 0);
    Event steps = event("s1", "PreToolUse", "TodoWrite");
    steps.steps_done  = 1;
    steps.steps_total = 3;
    table.apply(steps, 1);
    const Session &s = shot(table, 2).sessions[0];
    EXPECT_EQ(s.lately_count, 1);
    EXPECT_STREQ(s.doing.what, "ui.h");
    EXPECT_EQ(s.steps_total, 3);
}

TEST(ClaudeTable, waiting_on_you_is_told_once_and_ends_when_it_goes_on)
{
    Table   table;
    Session asked;
    table.apply(event("s1", "UserPromptSubmit"), 0);
    table.apply(event("s1", "PermissionRequest", "Bash", "idf.py"), 1 * min, &asked);
    EXPECT_STREQ(asked.id, "s1");
    EXPECT_EQ(asked.state, State::NeedsYou);
    EXPECT_STREQ(asked.doing.what, "idf.py");

    Session again;
    table.apply(event("s1", "PermissionRequest", "Bash", "idf.py"), 2 * min, &again);
    EXPECT_STREQ(again.id, "") << "asked already";

    table.apply(event("s1", "PostToolUse", "Bash"), 3 * min);
    EXPECT_EQ(shot(table, 3 * min).sessions[0].state, State::Working) << "answered, and run";
}

TEST(ClaudeTable, a_turn_ends_done_or_failed)
{
    Table table;
    table.apply(event("s1", "UserPromptSubmit"), 0);
    table.apply(event("s1", "SubagentStart"), 1 * min);
    EXPECT_EQ(shot(table, 1 * min).sessions[0].agents, 1);
    table.apply(event("s1", "Stop"), 9 * min);
    const Session &s = shot(table, 9 * min).sessions[0];
    EXPECT_EQ(s.state, State::Done);
    EXPECT_EQ(s.ran_ms, 9 * min) << "from the prompt";
    EXPECT_EQ(s.agents, 0);

    table.apply(event("s2", "UserPromptSubmit"), 10 * min);
    table.apply(event("s2", "StopFailure"), 11 * min);
    EXPECT_EQ(shot(table, 11 * min).sessions[0].state, State::Failed);
}

TEST(ClaudeTable, starting_again_mid_turn_leaves_it_working)
{
    Table table;
    table.apply(event("s1", "UserPromptSubmit"), 0);
    table.apply(event("s1", "SessionStart"), 1 * min);  // as compacting does
    EXPECT_EQ(shot(table, 1 * min).sessions[0].state, State::Working);
}

TEST(ClaudeTable, what_needs_you_comes_first_then_what_failed_works_and_rests)
{
    Table table;
    table.apply(event("done", "Stop"), 0);
    table.apply(event("old", "UserPromptSubmit"), 1);
    table.apply(event("new", "UserPromptSubmit"), 2);
    table.apply(event("fail", "StopFailure"), 3);
    table.apply(event("ask", "PermissionRequest", "Bash"), 4);
    const Snapshot s = shot(table, 5);
    ASSERT_EQ(s.count, 5);
    EXPECT_STREQ(s.sessions[0].id, "ask");
    EXPECT_STREQ(s.sessions[1].id, "fail");
    EXPECT_STREQ(s.sessions[2].id, "new") << "the latest first among equals";
    EXPECT_STREQ(s.sessions[3].id, "old");
    EXPECT_STREQ(s.sessions[4].id, "done");
}

TEST(ClaudeTable, shown_for_ten_minutes_after_any_event)
{
    Table table;
    EXPECT_FALSE(shot(table, 0).shown) << "nothing to show";
    table.apply(event("s1", "Stop"), 0);
    EXPECT_TRUE(shot(table, 9 * min).shown);
    EXPECT_FALSE(shot(table, 10 * min).shown);
    table.apply(event("s2", "SessionStart"), 15 * min);
    EXPECT_TRUE(shot(table, 24 * min).shown) << "any session's event counts";
}

TEST(ClaudeTable, shown_while_one_waits_on_you_however_long)
{
    Table table;
    table.apply(event("s1", "PermissionRequest", "Edit"), 0);
    EXPECT_TRUE(shot(table, 3 * 60 * min).shown);
}

TEST(ClaudeTable, the_last_one_ending_takes_it_away)
{
    Table table;
    table.apply(event("s1", "UserPromptSubmit"), 0);
    table.apply(event("s1", "SessionEnd"), 1 * min);
    const Snapshot s = shot(table, 1 * min);
    EXPECT_EQ(s.count, 0);
    EXPECT_FALSE(s.shown);
}

TEST(ClaudeTable, a_session_silent_for_half_a_day_is_forgotten)
{
    Table table;
    table.apply(event("s1", "PermissionRequest", "Bash"), 0);
    EXPECT_EQ(shot(table, kForgetAfterMs).count, 1);
    EXPECT_EQ(shot(table, kForgetAfterMs + 1).count, 0);
}

TEST(ClaudeTable, full_it_makes_room_by_the_one_heard_from_longest_ago)
{
    Table table;
    for (int i = 0; i < kSessionCount; ++i) {
        table.apply(event(("s" + std::to_string(i)).c_str(), "Stop"), i);
    }
    table.apply(event("s0", "Stop"), 100);  // heard again, so s1 is now the oldest
    table.apply(event("late", "Stop"), 101);
    const Snapshot s = shot(table, 102);
    EXPECT_EQ(s.count, kSessionCount);
    for (int i = 0; i < s.count; ++i) {
        EXPECT_STRNE(s.sessions[i].id, "s1");
    }
}

TEST(ClaudeTable, the_laptops_are_counted_once_each)
{
    Table table;
    Event work = event("s3", "Stop");
    work.machine = "work";
    table.apply(event("s1", "Stop"), 0);
    table.apply(event("s2", "Stop"), 0);
    table.apply(work, 0);
    EXPECT_EQ(shot(table, 1).machines, 2);
}
