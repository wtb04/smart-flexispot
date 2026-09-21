#pragma once

#include "esp_err.h"

#include <cstddef>

namespace logbuf {

/** Starts capturing everything written through the logging system. The console
 *  keeps receiving it too. Call early: what happens before this is not kept. */
esp_err_t start();

/** Joins the most recent lines written under any of `tags`, oldest first, into
 *  `out`. Returns the number of lines written. */
int recent(const char *const *tags, int tag_count, char *out, std::size_t out_size, int max_lines);

}  // namespace logbuf
