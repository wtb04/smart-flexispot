#pragma once

#include "esp_err.h"
#include "focus_plan.h"

namespace focus {
enum class Action : std::uint8_t { Toggle, Skip, Reset };

/** Told of every change; finished when a part ran out on its own rather than
 *  by a tap. Called from the timer's task or the caller's, so it must return
 *  at once. */
using ChangeHandler = void (*)(const State &state, bool finished);

esp_err_t start(ChangeHandler on_change);

/** From any task. */
void act(Action action);

State state();
Plan  plan();

/** Takes effect from the next part; the one under way keeps its length. */
void set_plan(const Plan &plan);

/** The clock State's times are on: milliseconds since boot. */
std::int64_t now_ms();

}  // namespace focus
