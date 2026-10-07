#include "topics.h"

#include <deque>

namespace ui::detail {
namespace {
constexpr std::uint32_t SECOND_MS = 1000;

struct Follower {
    Topic                 topic;
    ViewId                view;
    std::function<void()> changed;
};

// A deque, as one follower may subscribe another while it is being called, and
// that must not move the one being called.
std::deque<Follower> &followers()
{
    static std::deque<Follower> list;
    return list;
}

bool s_changed[static_cast<int>(Topic::Count)] = {};
lv_timer_t *s_second = nullptr;
bool        s_delivery_due = false;

void second(lv_timer_t *)
{
    publish(Topic::Second);
    deliver_topics();
}

void tell(Follower &follower)
{
    if (follower.view == kNoView || view_open(follower.view)) {
        follower.changed();
    }
}
}  // namespace

void subscribe(Topic topic, ViewId view, std::function<void()> changed)
{
    if (s_second == nullptr) {
        s_second = lv_timer_create(second, SECOND_MS, nullptr);
    }
    followers().push_back({topic, view, std::move(changed)});
    tell(followers().back());
}

void publish(Topic topic)
{
    s_changed[static_cast<int>(topic)] = true;
    // Changed by a tap rather than an update: told on the next pass of the LVGL
    // task all the same, once however many changed by then.
    if (!s_delivery_due) {
        s_delivery_due = true;
        lv_async_call([](void *) { deliver_topics(); }, nullptr);
    }
}

void deliver_topics()
{
    s_delivery_due = false;
    bool changed[static_cast<int>(Topic::Count)];
    for (int i = 0; i < static_cast<int>(Topic::Count); ++i) {
        changed[i]   = s_changed[i];
        s_changed[i] = false;  // a follower may publish again, for the next time round
    }
    // By index, as the list may grow on the way.
    for (std::size_t i = 0; i < followers().size(); ++i) {
        if (changed[static_cast<int>(followers()[i].topic)]) {
            tell(followers()[i]);
        }
    }
}

void deliver_to_view(ViewId view)
{
    for (std::size_t i = 0; i < followers().size(); ++i) {
        if (followers()[i].view == view) {
            tell(followers()[i]);
        }
    }
}

}  // namespace ui::detail
