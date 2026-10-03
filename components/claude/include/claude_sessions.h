#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The Claude Code sessions on the laptops, as their hooks tell of them: one
// event at a time, each from tools/claude-hook. Plain C++, no ESP-IDF, so it
// is tested on the host; claude.h keeps one table for the whole panel.
namespace claude {

inline constexpr int kSessionCount = 6;  // the most kept; the one heard from longest ago makes room
inline constexpr int kLatelyCount  = 4;
inline constexpr int kStepCount    = 5;  // two done, the one under way, two to come

// Shown this long after the last event from any session, and while one waits on you.
inline constexpr std::int64_t kShownForMs = 10 * 60 * 1000;
// A session silent this long has gone without saying so: a lid shut, a laptop away.
inline constexpr std::int64_t kForgetAfterMs = 12 * 60 * 60 * 1000;

enum class State : std::uint8_t { Idle, Working, NeedsYou, Done, Failed };

enum class Act : std::uint8_t { None, Edit, Read, Run, Search, Agents, Web, Other };

struct Action {
    Act          act    = Act::None;
    char         what[40] = "";  // a file's name, a command's first word, a pattern, a host
    std::int64_t at_ms  = 0;
};

enum class Step : std::uint8_t { Done, Now, Next };

struct StepItem {
    Step step     = Step::Next;
    char text[48] = "";
};

struct Session {
    char         id[40]      = "";
    char         machine[24] = "";
    char         project[32] = "";
    State        state       = State::Idle;
    std::int64_t since_ms    = 0;  // in this state since
    std::int64_t turn_ms     = -1;  // the prompt this turn began with, -1 between turns
    std::int64_t ran_ms      = 0;  // how long the last finished turn took
    std::int64_t heard_ms    = 0;
    Action       doing;            // what it does, or wants to do while it waits on you
    Action       lately[kLatelyCount]{};  // newest first
    int          lately_count = 0;
    int          agents       = 0;
    int          steps_done   = 0;
    int          steps_total  = 0;
    StepItem     steps[kStepCount]{};  // round the one under way
    int          step_count   = 0;
};

struct Snapshot {
    std::int64_t now_ms = 0;
    bool         shown  = false;
    int          count  = 0;
    int          machines = 0;
    Session      sessions[kSessionCount]{};  // what needs you first, then what failed, works, rests
};

/** One event as tools/claude-hook sends it, its fields as named there. */
struct Event {
    std::string machine;
    std::string session;
    std::string project;
    std::string event;  // the hook's own name for it: PreToolUse, Stop, ...
    std::string tool;
    std::string target;
    int         steps_done  = -1;  // with a step list, else -1
    int         steps_total = -1;
    std::array<StepItem, kStepCount> steps{};
    int         step_count = 0;
};

/** False when it is not JSON, or says nothing of a session. */
bool parse(std::string_view json, Event &out);

/** What a tool does, from its name: Edit and Write edit, Bash runs, and so on. */
Act act_of(std::string_view tool);

enum class Tense : std::uint8_t { Now, Past, Wanted };

/** "Editing rail.cpp", "Edited rail.cpp", "wants to edit rail.cpp". */
void describe(const Action &action, Tense tense, char *out, std::size_t size);

class Table {
public:
    /** False when the event changed nothing. With `asked`, a session that has
     *  just begun to wait on you is copied there. */
    bool apply(const Event &event, std::int64_t now_ms, Session *asked = nullptr);

    void snapshot(std::int64_t now_ms, Snapshot &out) const;

private:
    Session *find(std::string_view id);
    Session &make_room(std::int64_t now_ms);
    void     forget_stale(std::int64_t now_ms);
    void     remove(int index);

    std::array<Session, kSessionCount> rows_{};
    int                                count_    = 0;
    std::int64_t                       heard_ms_ = 0;
};

}  // namespace claude
