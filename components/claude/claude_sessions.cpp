#include "claude_sessions.h"

#include "cJSON.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace claude {
namespace {

// Cut at a character's start, so a long name never ends half an é.
template <std::size_t N>
void copy(char (&dest)[N], std::string_view source)
{
    std::size_t n = std::min(source.size(), N - 1);
    if (n < source.size()) {
        while (n > 0 && (static_cast<unsigned char>(source[n]) & 0xc0) == 0x80) {
            --n;
        }
    }
    std::memcpy(dest, source.data(), n);
    dest[n] = '\0';
}

std::string text_of(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

int number_of(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(item) ? item->valueint : -1;
}

// Those either side of the step under way, two done before it where there are.
void take_steps(const cJSON *steps, Event &out)
{
    out.steps_done  = std::max(0, number_of(steps, "done"));
    out.steps_total = std::max(0, number_of(steps, "total"));
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(steps, "items");
    const int    n     = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    int          now   = -1;
    for (int i = 0; i < n && now < 0; ++i) {
        if (text_of(cJSON_GetArrayItem(items, i), "state") == "now") {
            now = i;
        }
    }
    for (int i = 0; i < n && now < 0; ++i) {
        if (text_of(cJSON_GetArrayItem(items, i), "state") == "next") {
            now = i;
        }
    }
    if (now < 0) {
        now = n - 1;
    }
    const int first = std::clamp(now - 2, 0, std::max(0, n - kStepCount));
    for (int i = first; i < n && out.step_count < kStepCount; ++i) {
        const cJSON      *item  = cJSON_GetArrayItem(items, i);
        const std::string state = text_of(item, "state");
        StepItem         &step  = out.steps[static_cast<std::size_t>(out.step_count++)];
        step.step               = state == "done" ? Step::Done : state == "now" ? Step::Now : Step::Next;
        copy(step.text, text_of(item, "text"));
    }
}

int rank(State state)
{
    switch (state) {
        case State::NeedsYou: return 0;
        case State::Failed:   return 1;
        case State::Working:  return 2;
        default:              return 3;
    }
}
}  // namespace

bool parse(std::string_view json, Event &out)
{
    cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
    if (root == nullptr) {
        return false;
    }
    out         = Event{};
    out.machine = text_of(root, "machine");
    out.session = text_of(root, "session");
    out.project = text_of(root, "project");
    out.event   = text_of(root, "event");
    out.tool    = text_of(root, "tool");
    out.target  = text_of(root, "target");
    if (const cJSON *steps = cJSON_GetObjectItemCaseSensitive(root, "steps"); cJSON_IsObject(steps)) {
        take_steps(steps, out);
    }
    cJSON_Delete(root);
    return !out.session.empty() && !out.event.empty();
}

Act act_of(std::string_view tool)
{
    if (tool == "Edit" || tool == "Write" || tool == "MultiEdit" || tool == "NotebookEdit") {
        return Act::Edit;
    }
    if (tool == "Read") {
        return Act::Read;
    }
    if (tool == "Bash") {
        return Act::Run;
    }
    if (tool == "Grep" || tool == "Glob") {
        return Act::Search;
    }
    if (tool == "Task" || tool == "Agent") {
        return Act::Agents;
    }
    if (tool == "WebFetch" || tool == "WebSearch") {
        return Act::Web;
    }
    return tool.empty() ? Act::None : Act::Other;
}

void describe(const Action &action, Tense tense, char *out, std::size_t size)
{
    struct Words {
        const char *now, *past, *wanted, *nothing;  // the last for when it names nothing
    };
    static constexpr Words WORDS[] = {
        {"", "", "", ""},
        {"Editing", "Edited", "wants to edit", "a file"},
        {"Reading", "Read", "wants to read", "a file"},
        {"Running", "Ran", "wants to run", "a command"},
        {"Searching for", "Searched for", "wants to search for", "something"},
        {"Starting an agent", "Started an agent", "wants to start an agent", ""},
        {"Reading", "Read", "wants to open", "the web"},
        {"Using", "Used", "wants to use", "a tool"},
    };
    const Words &words = WORDS[static_cast<std::size_t>(action.act)];
    const char  *verb  = tense == Tense::Now ? words.now : tense == Tense::Past ? words.past : words.wanted;
    if (action.act == Act::None || size == 0) {
        if (size > 0) {
            out[0] = '\0';
        }
        return;
    }
    if (action.act == Act::Agents) {
        std::snprintf(out, size, "%s", verb);
        return;
    }
    std::snprintf(out, size, "%s %s", verb, action.what[0] != '\0' ? action.what : words.nothing);
}

Session *Table::find(std::string_view id)
{
    for (int i = 0; i < count_; ++i) {
        if (id == rows_[static_cast<std::size_t>(i)].id) {
            return &rows_[static_cast<std::size_t>(i)];
        }
    }
    return nullptr;
}

void Table::remove(int index)
{
    for (int i = index; i + 1 < count_; ++i) {
        rows_[static_cast<std::size_t>(i)] = rows_[static_cast<std::size_t>(i + 1)];
    }
    rows_[static_cast<std::size_t>(--count_)] = Session{};
}

void Table::forget_stale(std::int64_t now_ms)
{
    for (int i = count_ - 1; i >= 0; --i) {
        if (now_ms - rows_[static_cast<std::size_t>(i)].heard_ms > kForgetAfterMs) {
            remove(i);
        }
    }
}

Session &Table::make_room(std::int64_t now_ms)
{
    forget_stale(now_ms);
    if (count_ == kSessionCount) {
        int oldest = 0;
        for (int i = 1; i < count_; ++i) {
            if (rows_[static_cast<std::size_t>(i)].heard_ms < rows_[static_cast<std::size_t>(oldest)].heard_ms) {
                oldest = i;
            }
        }
        remove(oldest);
    }
    Session &row = rows_[static_cast<std::size_t>(count_++)];
    row          = Session{};
    return row;
}

bool Table::apply(const Event &event, std::int64_t now_ms, Session *asked)
{
    heard_ms_            = now_ms;
    const std::string &e = event.event;
    Session           *s = find(event.session);
    if (e == "SessionEnd") {
        if (s != nullptr) {
            remove(static_cast<int>(s - rows_.data()));
        }
        return true;
    }
    if (s == nullptr) {
        s = &make_room(now_ms);
        copy(s->id, event.session);
        s->since_ms = now_ms;
    }
    if (!event.machine.empty()) {
        copy(s->machine, event.machine);
    }
    if (!event.project.empty()) {
        copy(s->project, event.project);
    }
    s->heard_ms = now_ms;
    if (event.steps_total >= 0) {
        s->steps_done  = event.steps_done;
        s->steps_total = event.steps_total;
        s->step_count  = event.step_count;
        std::copy_n(event.steps.begin(), event.step_count, s->steps);
    }

    const auto begin = [&](State state) {
        if (s->state != state) {
            s->state    = state;
            s->since_ms = now_ms;
        }
    };
    if (e == "UserPromptSubmit") {
        begin(State::Working);
        s->since_ms = now_ms;
        s->turn_ms  = now_ms;
        s->doing    = Action{};
        s->agents   = 0;
    } else if (e == "PreToolUse") {
        if (s->turn_ms < 0) {  // the prompt was before the panel heard of it
            s->turn_ms = now_ms;
        }
        begin(State::Working);
        if (event.steps_total < 0) {  // a step list written is no action of its own
            s->doing = Action{act_of(event.tool), "", now_ms};
            copy(s->doing.what, event.target);
            std::copy_backward(s->lately, s->lately + kLatelyCount - 1, s->lately + kLatelyCount);
            s->lately[0]    = s->doing;
            s->lately_count = std::min(s->lately_count + 1, kLatelyCount);
        }
    } else if (e == "PostToolUse") {
        if (s->state == State::NeedsYou) {
            begin(State::Working);
        }
    } else if (e == "PermissionRequest") {
        const bool already = s->state == State::NeedsYou;
        begin(State::NeedsYou);
        s->doing = Action{act_of(event.tool), "", now_ms};
        copy(s->doing.what, event.target);
        if (asked != nullptr && !already) {
            *asked = *s;
        }
    } else if (e == "Stop") {
        s->ran_ms  = s->turn_ms >= 0 ? now_ms - s->turn_ms : 0;
        s->turn_ms = -1;
        begin(State::Done);
        s->doing  = Action{};
        s->agents = 0;
    } else if (e == "StopFailure") {
        begin(State::Failed);
        s->doing  = Action{};
        s->agents = 0;
    } else if (e == "SubagentStart") {
        ++s->agents;
    } else if (e == "SubagentStop") {
        s->agents = std::max(0, s->agents - 1);
    }
    return true;
}

void Table::snapshot(std::int64_t now_ms, Snapshot &out) const
{
    out        = Snapshot{};
    out.now_ms = now_ms;
    bool waits = false;
    for (int i = 0; i < count_; ++i) {
        const Session &row = rows_[static_cast<std::size_t>(i)];
        if (now_ms - row.heard_ms > kForgetAfterMs) {
            continue;
        }
        out.sessions[out.count++] = row;
        waits = waits || row.state == State::NeedsYou;
    }
    std::stable_sort(out.sessions, out.sessions + out.count, [](const Session &a, const Session &b) {
        return rank(a.state) != rank(b.state) ? rank(a.state) < rank(b.state) : a.since_ms > b.since_ms;
    });
    for (int i = 0; i < out.count; ++i) {
        bool seen = false;
        for (int j = 0; j < i && !seen; ++j) {
            seen = std::strcmp(out.sessions[i].machine, out.sessions[j].machine) == 0;
        }
        out.machines += seen ? 0 : 1;
    }
    out.shown = out.count > 0 && (waits || now_ms - heard_ms_ < kShownForMs);
}

}  // namespace claude
