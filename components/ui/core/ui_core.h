#pragma once

// What the models and the plumbing under the screen share: nothing in here draws.

#include "ui.h"

#include "esp_log.h"
#include "lvgl.h"

#include <atomic>
#include <cstdint>
#include <ctime>

namespace ui::detail {
constexpr char TAG[] = "ui";

constexpr std::uint32_t LOCK_TIMEOUT_MS = 500;

extern Handlers    s_handlers;
extern Orientation s_orientation;
extern int         s_initial_brightness;

constexpr int          NOTIFY_QUEUE_LEN  = 4;

struct Notice {
    char source[24];
    char title[64];
    char message[192];
    Level level;
    int   timeout_ms;
    char  key[48];  // what withdraw() takes it back by; empty for none
};

// What the screen calls each preset. Everything off the screen says Preset 1 to 6.
// components/ui/ui_presets.h, which git ignores, can name them otherwise.
#if __has_include("ui_presets.h")
#include "ui_presets.h"
#else
constexpr const char *PRESET_NAMES[kPresetCount] = {
    "Preset 1", "Ultra low", "Stand", "Sit", "Sit 2", "Stand 2",
};
#endif

constexpr std::uint32_t PAUSE_SETTLE_MS = 1500;

// The pages, in the order the dock's tabs are made.
constexpr int HOME_PAGE     = 0;
constexpr int RADAR_PAGE    = 1;
constexpr int CALENDAR_PAGE = 2;
constexpr int SETUP_PAGE    = 3;  // opened from the bar's status
constexpr int PAGE_COUNT    = 4;


constexpr int SETTING_COUNT = static_cast<int>(Setting::Count);

// The focus plan the panel shows until the timer reports one, and the limits
// the Setup steppers keep it to, which the fullscreen timer is sized for.
constexpr int FOCUS_WORK_MIN_DEFAULT       = 25;
constexpr int FOCUS_BREAK_MIN_DEFAULT      = 5;
constexpr int FOCUS_LONG_BREAK_MIN_DEFAULT = 20;
constexpr int FOCUS_ROUNDS_DEFAULT         = 4;
constexpr int FOCUS_WORK_MIN_MAX           = 90;
constexpr int FOCUS_ROUNDS_MAX             = 8;

// The presets the dock's Stand and Sit send the desk to.
constexpr int STAND_PRESET     = 2;
constexpr int SIT_PRESET       = 3;
constexpr int ULTRA_LOW_PRESET = 1;  // Preset 2, as the cinema view sends the desk down

// The diagnostics cards set_cards() was given, before init.
extern const Card *s_cards;
extern int         s_card_count;

/** The page on show: HOME_PAGE to SETUP_PAGE. */
extern int               s_page;
extern std::atomic<bool> s_setup_visible;
/** A page on show, or Home in its place while it is away; each front's own. */
void select_page(int index);

/** The setting that takes the owner's pages away with their phone. */
extern bool s_presence_gate;

/** The screen lit or dark, as the schedule or a touch decides. */
void set_screen_state(bool on);

/** The time the screen shows: the real one, or running on from the clock
 *  pinned by pin_clock(), so what it says agrees with the top bar. */
std::time_t wall_now();

/** The clock pin_clock() set, or null: what the screen shows in place of the time. */
const char *pinned_clock();

/** The screen going dark by itself, and lighting again: see screen_rules.h. */
void start_screen_schedule();

// What the screen schedule asks of the views on show (screen_rules.h).
bool notice_on_show();

/** How far start-up has said it is, for the start-up screen: steps done, and
 *  whether all is up. */
int  splash_steps_wanted();
bool splash_done_wanted();
bool splash_showing();  // the front's, still over the screen
/** Takes back the notices sent with `key`, waiting or on show. */
void withdraw_notice(const char *key);
/** How long the open film leaves the screen lit untouched: 0 with none open,
 *  screen_rules::kNever while it keeps it lit. */
std::int64_t cinema_dark_after();
/** An intro or credits to skip on show: the screen lights for it. */
bool cinema_offers_skip();

}  // namespace ui::detail

// What the screen built on this core provides, in kit_front.cpp.
namespace ui::front {
/** In init, with the lock held: the look, and what shows while the rest is made. */
void init(std::uint32_t accent);
/** One part of the screen a call, behind what init put up; false after the last. */
bool build_next_part();
}  // namespace ui::front
