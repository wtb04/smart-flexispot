#include "http.h"

#include "net.h"

#include <memory>
#include <mutex>

namespace live {
namespace {
constexpr int         TIMEOUT_MS = 15 * 1000;
constexpr std::size_t MAX_BODY   = 8 * 1024 * 1024;  // a work calendar, a gzipped trace inflated

// What each host's config points at, kept for as long as net is.
const char *kept(const std::string &text)
{
    static std::mutex                               lock;
    static std::vector<std::unique_ptr<std::string>> texts;
    std::lock_guard<std::mutex>                     hold(lock);
    texts.push_back(std::make_unique<std::string>(text));
    return texts.back()->c_str();
}
}  // namespace

Answer get(const std::string &url, const std::vector<std::string> &headers, net::Priority priority)
{
    std::string lines;
    for (const std::string &header : headers) {
        lines += header + '\n';
    }
    net::HostConfig like;
    like.agent      = "smart-flexispot-sim";
    like.headers    = kept(lines);
    like.timeout_ms = TIMEOUT_MS;
    like.connections = 2;

    net::Request request;
    request.host     = net::host_for(url, like);
    request.path     = url;
    request.priority = priority;
    request.max_body = MAX_BODY;
    request.what     = "sim";

    Answer             answer;
    const net::Fetched got = net::fetch(std::move(request), answer.body);
    answer.status          = got.outcome == net::Outcome::Answered ? got.status : 0;
    return answer;
}
}  // namespace live
