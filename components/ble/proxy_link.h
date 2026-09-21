#pragma once

#include "ble.h"
#include "deskproto.h"
#include "esp_err.h"
#include "host/ble_gap.h"

#include <cstdint>

// The link to the box that holds the desk's wire. Lives beside the presence
// scanner because NimBLE has one host and one set of GAP events, and they have
// to be shared rather than started twice.
namespace ble::proxy {

/** Called for every advertisement the scanner sees. Returns true once it has
 *  decided to connect, so the scanner knows it has been stopped. */
bool consider(const ble_gap_disc_desc &advert);

/** Connection, discovery and notification events. Returns true when the event
 *  belonged to this link and the scanner should leave it alone. */
bool handle(ble_gap_event *event);

/** Scanning is stopped while connecting and has to be put back afterwards. */
void set_rescan(void (*rescan)());

/** Starts the task that times the round trip. */
esp_err_t start();

bool connected();

void collect(LinkStats &out);

void set_status_handler(desk::StatusHandler handler);
void set_hold(deskproto::Motion direction);
void send_command(deskproto::Op op, std::uint8_t preset);
bool last_status(int &height_mm, bool &box_linked, deskproto::Motion &motion);

}  // namespace ble::proxy
