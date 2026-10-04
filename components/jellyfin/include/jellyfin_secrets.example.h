#pragma once

// Copy to jellyfin_secrets.h, which git ignores, and fill in: the server, as
// "https://jellyfin.example", and a key made in Jellyfin under Dashboard, API
// Keys. The panel only reads an episode's media segments with it, to offer
// skipping the intro and going on after the credits. Empty leaves it out.
#define JELLYFIN_URL ""
#define JELLYFIN_KEY ""
