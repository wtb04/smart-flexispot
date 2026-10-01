#pragma once

#include "ui.h"

// What plays, played here: Home Assistant's speaker and a Jellyfin session are
// not reached from the simulator, and the media card, its panel, the
// favourites and cinema mode have to be seen all the same.
namespace media_stub {
// Followed: an episode on a player that takes no commands, as Streamyfin is.
enum class Scene { Idle, Music, Episode, Followed };

/** After ui::init. */
void start();

/** Once per frame: position reports, and an episode running out. */
void tick();

void show(Scene scene);
void next_scene();  // idle, music, an episode, one that is only followed, and round again

void on_media(ui::MediaAction action);
void on_seek(int position_s);
void on_volume(int percent);
void on_pick(int index);
}  // namespace media_stub
