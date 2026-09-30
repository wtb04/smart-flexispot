#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#include <cstddef>
#include <cstdint>

// Firmware over the air, for the panel and, through it, the desk companion:
// POST /update/panel or /update/companion with the image as the body and the
// key as X-Update-Key. It is kept ready until install() is asked for, unless
// the request says ?install=now. GET /version says what runs. tools/ota.sh.
namespace ota {
enum class Target : std::uint8_t { None, Panel, Companion };
enum class Phase : std::uint8_t { Receiving, Installing };

struct Status {
    Target busy            = Target::None;
    Phase  phase           = Phase::Receiving;  // the image arriving here, or going on to the companion
    int    percent         = 0;
    int    seconds_left    = -1;  // at the rate so far; negative until there is one
    bool   immediate       = false;  // installed as soon as it is in, rather than kept
    bool   panel_ready     = false;
    bool   companion_ready = false;
};

struct Hooks {
    /** True while the desk moves; an install waits for it to stand still. */
    bool (*busy)();
    /** Every change of Status, from the update tasks; must return at once. */
    void (*status)(const Status &status);
    /** Settles what has to be written before the panel restarts into its update. */
    void (*restart)();
    /** Passes a companion image over the link; percent goes to progress. */
    esp_err_t (*relay)(const std::uint8_t *image, std::size_t size, std::uint32_t crc,
                       void (*progress)(int percent));
};

esp_err_t start(const Hooks &hooks);

/** The server the updates come in on, for other pages to be added to, once
 *  started; and whether a request carries the update key, which they ask for too. */
httpd_handle_t server();
bool           authorised(httpd_req_t *req);

/** Installs whatever is ready, the companion's first; returns at once. */
void install();

/** A firmware booted from an update has a few minutes to reach the network,
 *  where the next update would come from; not doing so restarts, and the
 *  bootloader returns to the one before. Call once, early. */
void watch();

/** It got there: the firmware keeps its place. */
void confirm();

/** An update the bootloader turned back from, the panel on the firmware before
 *  it: it did not reach the network in time, or did not start. Stays so
 *  until the next update takes. */
struct RolledBack {
    bool happened = false;
    bool new_now  = false;  // not told before: once for each image
    char version[32] = "";  // what it would have been
    char image[9]    = "";  // its ELF's sha256, the first eight of it in hex
};
RolledBack rolled_back();

}  // namespace ota
