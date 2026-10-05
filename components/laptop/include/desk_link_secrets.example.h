#pragma once

// Copy to desk_link_secrets.h. The key Desk Link and the panel seal every
// message with, each way: 32 bytes, as 64 hex digits, made with
//     openssl rand -hex 32
// and given to Desk Link in its menu. Empty refuses every laptop.
#define DESK_LINK_KEY ""
