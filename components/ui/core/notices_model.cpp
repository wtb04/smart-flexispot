#include "notices_model.h"

#include "notice_queue.h"
#include "topics.h"

namespace ui::detail {
namespace {
NoticesState s_notices;

void drop_oldest()
{
    for (int i = 1; i < s_notices.count; ++i) {
        s_notices.waiting[i - 1] = s_notices.waiting[i];
    }
    --s_notices.count;
}
}  // namespace

const NoticesState &notices_state()
{
    return s_notices;
}

void notices_take(const Notice &notice)
{
    if (s_notices.count == NOTIFY_QUEUE_LEN) {
        ESP_LOGW(TAG, "notification queue full, dropping oldest");
        drop_oldest();
    }
    s_notices.waiting[s_notices.count++] = notice;
    publish(Topic::Notices);
}

bool notices_take_next(Notice &notice)
{
    if (s_notices.count == 0) {
        return false;
    }
    notice = s_notices.waiting[0];
    drop_oldest();
    publish(Topic::Notices);
    return true;
}

void notices_withdraw(const char *key)
{
    const int left = withdraw_keyed(s_notices.waiting, s_notices.count, key);
    if (left != s_notices.count) {
        s_notices.count = left;
        publish(Topic::Notices);
    }
}

}  // namespace ui::detail
