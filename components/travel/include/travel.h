#pragma once

#include "esp_err.h"
#include "travel_parse.h"

#include <cstdint>

namespace travel {
/** Called on the travel task once an answer has come back. */
using UpdateHandler = void (*)();

esp_err_t start(UpdateHandler on_update);

/** Asks what it takes to be somewhere by `arrive_by`, in unix seconds. Asking
 *  about the same time again is free; the answer is refreshed on its own while
 *  it matters. Zero forgets the question. */
void want(std::int64_t arrive_by);

/** The journeys last answered with, soonest departure first. Thread-safe. */
int options(Option *out, int capacity);

/** Whether the last request came back with something. */
bool ok();

}  // namespace travel
