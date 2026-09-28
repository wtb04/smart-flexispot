#pragma once

#include <string>
#include <vector>

namespace live {
struct Answer {
    int         status = 0;  // 0 when nothing came back at all
    std::string body;
};

/** Blocking, on whichever thread asks; `headers` as "Name: value". */
Answer get(const std::string &url, const std::vector<std::string> &headers = {});
}  // namespace live
