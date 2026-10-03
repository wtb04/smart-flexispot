#include "claude_stub.h"

#include "claude.h"
#include "ui.h"

#include <cstdio>
#include <initializer_list>
#include <iterator>

namespace claude_stub {
namespace {
constexpr const char *STEPS =
    R"("steps":{"done":4,"total":7,"items":[{"state":"done","text":"Tests for the table"},)"
    R"({"state":"done","text":"ui::set_claude and its model"},{"state":"now","text":"Pill in the top bar"},)"
    R"({"state":"next","text":"Sessions card"},{"state":"next","text":"Simulator keys"}]})";

int s_scene = 0;

void send(std::initializer_list<std::string> events)
{
    for (const std::string &event : events) {
        from_hook(event);
    }
}

std::string event(const char *session, const char *machine, const char *project, const char *name,
                  const char *tool = nullptr, const char *target = nullptr, const char *extra = nullptr)
{
    std::string json = std::string(R"({"session":")") + session + R"(","machine":")" + machine +
                       R"(","project":")" + project + R"(","event":")" + name + '"';
    if (tool != nullptr) {
        json += std::string(R"(,"tool":")") + tool + '"';
    }
    if (target != nullptr) {
        json += std::string(R"(,"target":")") + target + '"';
    }
    if (extra != nullptr) {
        json += std::string(",") + extra;
    }
    return json + "}";
}
}  // namespace

void from_hook(const std::string &payload)
{
    claude::Session asked;
    bool            was_asked = false;
    if (!claude::take(payload.data(), payload.size(), asked, was_asked)) {
        std::printf("W (sim) not an event the panel would take: %s\n", payload.c_str());
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_claude());
    if (was_asked) {
        char wants[96];
        claude::describe(asked.doing, claude::Tense::Wanted, wants, sizeof(wants));
        char title[64];
        std::snprintf(title, sizeof(title), "%s needs you", asked.project);
        char message[128];
        std::snprintf(message, sizeof(message), "%s %s", asked.machine, wants);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify("Claude Code", title, message, ui::Level::Warn, 60 * 1000));
    }
}

void next_scene()
{
    switch (s_scene) {
        case 0:
            send({event("a", "MbP", "tab5-hello", "UserPromptSubmit"),
                  event("a", "MbP", "tab5-hello", "PreToolUse", "TodoWrite", nullptr, STEPS),
                  event("a", "MbP", "tab5-hello", "PreToolUse", "Read", "topics.h"),
                  event("a", "MbP", "tab5-hello", "PreToolUse", "Bash", "idf.py"),
                  event("a", "MbP", "tab5-hello", "PreToolUse", "Edit", "claude_pill.cpp"),
                  event("b", "MbP", "MediaCenter", "Stop")});
            break;
        case 1:
            send({event("c", "work", "billing-api", "UserPromptSubmit"),
                  event("c", "work", "billing-api", "SubagentStart"),
                  event("c", "work", "billing-api", "SubagentStart"),
                  event("c", "work", "billing-api", "PreToolUse", "Grep", "invoice_total"),
                  event("a", "MbP", "tab5-hello", "PermissionRequest", "Bash", "idf.py")});
            break;
        case 2:
            send({event("a", "MbP", "tab5-hello", "PostToolUse", "Bash"),
                  event("a", "MbP", "tab5-hello", "Stop"),
                  event("c", "work", "billing-api", "Stop")});
            break;
        default:
            send({event("a", "MbP", "tab5-hello", "SessionEnd"), event("b", "MbP", "MediaCenter", "SessionEnd"),
                  event("c", "work", "billing-api", "SessionEnd")});
            break;
    }
    s_scene = (s_scene + 1) % 4;
}

}  // namespace claude_stub
