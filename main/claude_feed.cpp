#include "claude_feed.h"

#include "claude.h"
#include "sound.h"
#include "ui.h"

#include "esp_check.h"
#include "esp_log.h"

#include <cstdio>

namespace claude_feed {
namespace {
constexpr char        TAG[]        = "claude";
constexpr int         ASKED_MS     = 60 * 1000;  // the pill stays amber after, until it is answered

// One per session, so its answer can take back what asked it.
void key_of(const claude::Session &session, char *out, std::size_t size)
{
    std::snprintf(out, size, "claude:%s", session.id);
}

void tell_asked(const claude::Session &session)
{
    char wants[96];
    claude::describe(session.doing, claude::Tense::Wanted, wants, sizeof(wants));
    char title[64];
    std::snprintf(title, sizeof(title), "%s needs you", session.project);
    char message[128];
    std::snprintf(message, sizeof(message), "%s %s", session.machine, wants);
    char key[48];
    key_of(session, key, sizeof(key));
    sound::ding();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify("Claude Code", title, message, ui::Level::Warn, ASKED_MS, key));
}

}  // namespace

void take(const char *event, std::size_t length)
{
    claude::Session asked;
    claude::Session answered;
    bool            was_asked    = false;
    bool            was_answered = false;
    if (!claude::take(event, length, asked, was_asked, answered, was_answered)) {
        ESP_LOGW(TAG, "not an event: %.*s", static_cast<int>(length < 120 ? length : 120), event);
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_claude());
    if (was_answered) {
        char key[48];
        key_of(answered, key, sizeof(key));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::withdraw(key));
    }
    if (was_asked) {
        tell_asked(asked);
    }
}

}  // namespace claude_feed
