#pragma once

#include "esp_err.h"

// Named desklink rather than link: POSIX has a link() and unistd.h is pulled
// in by more headers than you would think.
namespace desklink {

/** Advertises and echoes whatever is written back to the writer, so the panel
 *  can time the round trip. Nothing here can reach the desk. */
esp_err_t start();

/** The desk driver reports heights; the link passes them on. */
void note_height(int height_mm);

}  // namespace desklink
