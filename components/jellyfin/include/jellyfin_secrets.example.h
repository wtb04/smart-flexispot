#pragma once

// Copy to jellyfin_secrets.h, which git ignores, and fill in. The key is made in
// Jellyfin under Dashboard, API Keys; the panel only reads an episode's media
// segments with it, to offer skipping the intro and going on after the credits.
#define JELLYFIN_URL "https://jellyfin.example"
#define JELLYFIN_KEY ""
