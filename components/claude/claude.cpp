#include "claude.h"

#include <chrono>
#include <mutex>

namespace claude {
namespace {
std::mutex s_lock;
Table      s_table;
}  // namespace

std::int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool take(const char *json, std::size_t length, Session &asked, bool &was_asked)
{
    Event event;
    if (!parse({json, length}, event)) {
        return false;
    }
    const std::lock_guard<std::mutex> hold(s_lock);
    asked = Session{};
    s_table.apply(event, now_ms(), &asked);
    was_asked = asked.id[0] != '\0';
    return true;
}

void snapshot(Snapshot &out)
{
    const std::lock_guard<std::mutex> hold(s_lock);
    s_table.snapshot(now_ms(), out);
}

}  // namespace claude
