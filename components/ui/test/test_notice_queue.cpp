#include "notice_queue.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {
struct Item {
    char key[16];
    int  n;
};

Item item(const char *key, int n)
{
    Item out{};
    std::strncpy(out.key, key, sizeof(out.key) - 1);
    out.n = n;
    return out;
}
}  // namespace

TEST(NoticeQueue, withdrawing_takes_every_one_with_the_key_and_keeps_the_order)
{
    Item queue[5] = {item("a", 1), item("claude:s1", 2), item("", 3), item("claude:s1", 4), item("b", 5)};
    int  count    = ui::detail::withdraw_keyed(queue, 5, "claude:s1");
    ASSERT_EQ(count, 3);
    EXPECT_EQ(queue[0].n, 1);
    EXPECT_EQ(queue[1].n, 3);
    EXPECT_EQ(queue[2].n, 5);
}

TEST(NoticeQueue, an_empty_key_withdraws_nothing)
{
    Item queue[2] = {item("", 1), item("", 2)};
    EXPECT_EQ(ui::detail::withdraw_keyed(queue, 2, ""), 2) << "a notice without a key is never withdrawn";
    EXPECT_EQ(ui::detail::withdraw_keyed(queue, 2, nullptr), 2);
}

TEST(NoticeQueue, keyed_matches_only_the_same_key)
{
    EXPECT_TRUE(ui::detail::keyed(item("claude:s1", 0), "claude:s1"));
    EXPECT_FALSE(ui::detail::keyed(item("claude:s1", 0), "claude:s2"));
    EXPECT_FALSE(ui::detail::keyed(item("", 0), ""));
}
