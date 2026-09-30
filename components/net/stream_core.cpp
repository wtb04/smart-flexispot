#include "stream_core.h"

#include <algorithm>
#include <limits>

namespace net {
namespace {
constexpr std::int64_t US_PER_MS = 1000;

bool live(StreamState state)
{
    return state == StreamState::Connecting || state == StreamState::Open || state == StreamState::Ready;
}
}  // namespace

void StreamCore::enter(StreamState state, std::int64_t now)
{
    state_    = state;
    since_us_ = now;
}

void StreamCore::drop(std::int64_t now, const char *why, int retry_ms)
{
    ++drops_;
    last_error_ = why != nullptr ? why : "";
    std::int64_t wait_ms = retry_ms;
    if (wait_ms <= 0) {
        delay_ms_ = delay_ms_ == 0 ? policy_.reconnect.first_ms
                                   : std::min<std::int64_t>(delay_ms_ * policy_.reconnect.backoff_percent / 100,
                                                            policy_.reconnect.max_ms);
        wait_ms   = delay_ms_;
    }
    enter(StreamState::Waiting, now);
    due_us_ = now + wait_ms * US_PER_MS;
}

void StreamCore::network(bool up, std::int64_t now)
{
    if (!up && state_ != StreamState::Offline) {
        stop_wanted_ = stop_wanted_ || running_;
        enter(StreamState::Offline, now);
    } else if (up && state_ == StreamState::Offline) {
        delay_ms_ = 0;
        enter(StreamState::Waiting, now);
        due_us_ = now;
    }
}

void StreamCore::opened(std::int64_t now)
{
    if (state_ == StreamState::Connecting) {
        enter(StreamState::Open, now);
    }
}

void StreamCore::closed(std::int64_t now, const char *why)
{
    // One it was told to stop, or that went while offline, is not a drop.
    if (live(state_)) {
        running_ = false;
        drop(now, why, 0);
    }
}

void StreamCore::ready(std::int64_t now)
{
    if (state_ == StreamState::Connecting || state_ == StreamState::Open) {
        enter(StreamState::Ready, now);
        ++readies_;
        delay_ms_      = 0;
        keep_alive_us_ = now + static_cast<std::int64_t>(policy_.keep_alive_ms) * US_PER_MS;
    }
}

void StreamCore::fail(std::int64_t now, const char *why, int retry_ms)
{
    if (live(state_)) {
        stop_wanted_ = stop_wanted_ || running_;
        drop(now, why, retry_ms);
    }
}

void StreamCore::restart(std::int64_t now)
{
    if (state_ == StreamState::Offline) {
        return;
    }
    stop_wanted_ = stop_wanted_ || running_;
    delay_ms_    = 0;
    enter(StreamState::Waiting, now);
    due_us_ = now;
}

StreamAction StreamCore::next(std::int64_t now)
{
    if (stop_wanted_) {
        stop_wanted_ = false;
        running_     = false;
        return StreamAction::Stop;
    }
    if (state_ == StreamState::Waiting && now >= due_us_) {
        enter(StreamState::Connecting, now);
        attempt_us_ = now;
        running_    = true;
        return StreamAction::Start;
    }
    if ((state_ == StreamState::Connecting || state_ == StreamState::Open) &&
        now - attempt_us_ >= static_cast<std::int64_t>(policy_.ready_within_ms) * US_PER_MS) {
        drop(now, state_ == StreamState::Connecting ? "did not open in time" : "not ready in time", 0);
        running_ = false;
        return StreamAction::Stop;
    }
    if (state_ == StreamState::Ready && policy_.keep_alive_ms > 0 && now >= keep_alive_us_) {
        keep_alive_us_ = now + static_cast<std::int64_t>(policy_.keep_alive_ms) * US_PER_MS;
        return StreamAction::KeepAlive;
    }
    return StreamAction::None;
}

std::int64_t StreamCore::due(std::int64_t now) const
{
    if (stop_wanted_) {
        return now;
    }
    switch (state_) {
        case StreamState::Waiting: return due_us_;
        case StreamState::Connecting:
        case StreamState::Open:
            return attempt_us_ + static_cast<std::int64_t>(policy_.ready_within_ms) * US_PER_MS;
        case StreamState::Ready:
            if (policy_.keep_alive_ms > 0) {
                return keep_alive_us_;
            }
            break;
        default: break;
    }
    return std::numeric_limits<std::int64_t>::max();
}

StreamStatus StreamCore::status(std::int64_t now) const
{
    StreamStatus out;
    out.state      = state_;
    out.for_ms     = static_cast<int>((now - since_us_) / US_PER_MS);
    out.next_in_ms = state_ == StreamState::Waiting ? static_cast<int>(std::max<std::int64_t>(due_us_ - now, 0) / US_PER_MS) : 0;
    out.readies    = readies_;
    out.drops      = drops_;
    out.last_error = last_error_;
    return out;
}

}  // namespace net
