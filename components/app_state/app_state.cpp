#include "app_state.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace app {
namespace {
constexpr int FACTS = 2;

struct Kept {
    std::atomic<bool>                      value;
    std::vector<std::function<void(bool)>> watchers;
};

Kept       s_facts[FACTS] = {{true, {}}, {false, {}}};  // it starts lit, and offline
std::mutex s_watchers_lock;  // taken to add and to copy, never while telling

Kept &kept(Fact fact)
{
    return s_facts[static_cast<int>(fact)];
}
}  // namespace

bool get(Fact fact)
{
    return kept(fact).value.load(std::memory_order_relaxed);
}

void set(Fact fact, bool value)
{
    if (kept(fact).value.exchange(value, std::memory_order_relaxed) == value) {
        return;
    }
    std::vector<std::function<void(bool)>> watchers;
    {
        std::lock_guard<std::mutex> hold(s_watchers_lock);
        watchers = kept(fact).watchers;
    }
    for (const auto &watcher : watchers) {
        watcher(value);
    }
}

void watch(Fact fact, std::function<void(bool)> on_change)
{
    std::lock_guard<std::mutex> hold(s_watchers_lock);
    kept(fact).watchers.push_back(std::move(on_change));
}

}  // namespace app
