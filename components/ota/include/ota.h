#pragma once

#include "esp_err.h"

#include <cstddef>
#include <cstdint>

// Firmware over the air, for the panel and, through it, the desk companion:
// POST /update/panel or /update/companion with the image as the body and the
// key as X-Update-Key; GET /version says what runs. tools/ota.sh does both.
namespace ota {
struct Hooks {
    /** True while the desk moves; an update is refused then. */
    bool (*busy)();
    /** Said on the screen. */
    void (*notice)(const char *message);
    /** Settles what has to be written and restarts into the new panel firmware. */
    void (*restart)();
    /** Passes a companion image over the link; percent goes to progress. */
    esp_err_t (*relay)(const std::uint8_t *image, std::size_t size, std::uint32_t crc,
                       void (*progress)(int percent));
};

esp_err_t start(const Hooks &hooks);

/** A firmware booted from an update has a few minutes to reach the network,
 *  where the next update would come from; not doing so restarts, and the
 *  bootloader returns to the one before. Call once, early. */
void watch();

/** It got there: the firmware keeps its place. */
void confirm();

}  // namespace ota
