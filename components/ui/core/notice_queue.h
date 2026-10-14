#pragma once

#include <cstring>

// Taking back notices by the key they were sent with, as a question that has
// been answered takes back the one that asked it. Pure, so the host tests can
// run it.
namespace ui::detail {

/** A notice with no key is never one being taken back. */
inline bool keyed_text(const char *have, const char *key)
{
    return key != nullptr && key[0] != '\0' && std::strcmp(have, key) == 0;
}

template <typename Item>
bool keyed(const Item &item, const char *key)
{
    return keyed_text(item.key, key);
}

/** Every waiting item with `key` taken out, the rest kept in order; the count left. */
template <typename Item>
int withdraw_keyed(Item *items, int count, const char *key)
{
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        if (!keyed(items[i], key)) {
            items[kept++] = items[i];
        }
    }
    return kept;
}

}  // namespace ui::detail
