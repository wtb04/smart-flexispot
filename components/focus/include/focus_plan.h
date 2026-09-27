#pragma once

#include "units.h"

#include <cstdint>

namespace focus {
enum class Phase : std::uint8_t { Idle, Work, Break, LongBreak };

struct Plan {
    int work_min       = 25;
    int break_min      = 5;
    int rounds         = 4;
    int long_break_min = 20;
};

/** All anyone needs to show the timer, here or on another machine: the part,
 *  the round and when the part ends. It is never counted down, only compared
 *  with the clock, so two places showing it cannot drift apart. Milliseconds. */
struct State {
    Phase        phase   = Phase::Idle;
    int          round   = 0;  // the work round this is, or the one before a break
    bool         running = false;
    std::int64_t ends_at = 0;  // while running
    std::int32_t left    = 0;  // while paused
    std::int32_t length  = 0;  // the whole part
};

inline std::int32_t minutes(int count)
{
    return count * units::kMsPerMinute;
}

inline std::int32_t left_of(const State &state, std::int64_t now)
{
    if (state.phase == Phase::Idle) {
        return 0;
    }
    if (!state.running) {
        return state.left;
    }
    const std::int64_t left = state.ends_at - now;
    return left > 0 ? static_cast<std::int32_t>(left) : 0;
}

inline std::int32_t length_of(Phase phase, const Plan &plan)
{
    switch (phase) {
        case Phase::Work:      return minutes(plan.work_min);
        case Phase::Break:     return minutes(plan.break_min);
        case Phase::LongBreak: return minutes(plan.long_break_min);
        case Phase::Idle:      break;
    }
    return 0;
}

inline State part(Phase phase, int round, const Plan &plan, std::int64_t now)
{
    State state;
    state.phase   = phase;
    state.round   = round;
    state.running = true;
    state.length  = length_of(phase, plan);
    state.left    = state.length;
    state.ends_at = now + state.length;
    return state;
}

/** What follows a part that was skipped, started at once. After the long
 *  break the next set begins, until the timer is reset. */
inline State after(const State &state, const Plan &plan, std::int64_t now)
{
    switch (state.phase) {
        case Phase::Work:
            return part(state.round >= plan.rounds ? Phase::LongBreak : Phase::Break, state.round,
                        plan, now);
        case Phase::Break:
            return part(Phase::Work, state.round + 1, plan, now);
        case Phase::LongBreak:
            return part(Phase::Work, 1, plan, now);
        case Phase::Idle:
            break;
    }
    return State{};
}

/** A part set up and waiting for the button, its whole length to go. */
inline State ready(State state)
{
    state.running = false;
    state.left    = state.length;
    return state;
}

inline bool waiting(const State &state)
{
    return state.phase != Phase::Idle && !state.running && state.left == state.length;
}

/** What follows a part that ran out on its own, waiting for the button:
 *  nothing starts by itself. */
inline State finished(const State &state, const Plan &plan, std::int64_t now)
{
    return ready(after(state, plan, now));
}

/** The one button: start a set, pause, or carry on. */
inline State toggled(const State &state, const Plan &plan, std::int64_t now)
{
    if (state.phase == Phase::Idle) {
        return part(Phase::Work, 1, plan, now);
    }
    State next = state;
    if (state.running) {
        next.left    = left_of(state, now);
        next.running = false;
    } else {
        next.ends_at = now + state.left;
        next.running = true;
    }
    return next;
}

}  // namespace focus
