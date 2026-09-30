#pragma once

#include "net_types.h"

#include <string>
#include <vector>

namespace live {
struct Answer {
    int         status = 0;  // 0 when nothing came back at all
    std::string body;
};

/** Through net, blocking, on whichever thread asks; `headers` as "Name: value",
 *  and `agent` kept for the host from its first request. Only the feeds net_desktop.cpp
 *  allows are reached. */
Answer get(const std::string &url, const std::vector<std::string> &headers = {},
           net::Priority priority = net::Priority::Now, const char *agent = "smart-flexispot-sim");
}  // namespace live
