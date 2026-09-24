#pragma once

#include "esp_err.h"

namespace battery {
esp_err_t start();

/** Looks at the pack and the charging setting now rather than on the next poll.
 *  Returns at once; from any task. */
void refresh();

}  // namespace battery
