#pragma once

#include "esp_err.h"
#include "travel_parse.h"

#include <cstdint>

namespace travel {
/** Called on the travel task once an answer has come back. */
using UpdateHandler = void (*)();

esp_err_t start(UpdateHandler on_update);

/** Where a journey ends: the timetable's buildings, or work. */
enum class Place : std::uint8_t { Study, Work };

/** Asks what it takes to be at `place` by `arrive_by`, in unix seconds. Asking
 *  the same again is free; the answer is refreshed on its own while it
 *  matters. Zero forgets the question. */
void want(std::int64_t arrive_by, Place place = Place::Study);

/** The journeys last answered with, best first: the latest departure that still
 *  arrives in time, then earlier ones. Thread-safe. */
int options(Option *out, int capacity);

/** Whether the last request came back with something. */
bool ok();

}  // namespace travel
