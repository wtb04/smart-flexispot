// The screen as the design kit draws it, on the shared core in ui.cpp.

#include "kit_front.h"

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#endif

#include "screenshot.h"
#include "settings_model.h"
#include "status_model.h"
#include "topics.h"

#include <iterator>
#include <utility>

#ifndef SHOT_ENABLED
#define SHOT_ENABLED 0
#endif

namespace ui::kitfront {
namespace {
// The core's numbering of the pages, HOME_PAGE to SETUP_PAGE, against the dock's.
constexpr int KIT_OF[PAGE_COUNT]  = {HOME, RADAR, CALENDAR, SETUP};  // by detail::HOME_PAGE...
constexpr int CORE_OF[PAGE_COUNT] = {detail::HOME_PAGE, detail::CALENDAR_PAGE, detail::RADAR_PAGE, detail::SETUP_PAGE};

DockItems s_items;
int       s_before_setup = detail::HOME_PAGE;

bool needs_presence(int core) { return core == detail::RADAR_PAGE || core == detail::CALENDAR_PAGE; }

// What a page coming to show sets going, however it was chosen: the dock's
// item, a widget, Setup from the bar.
void shown(int kit)
{
    using namespace detail;
    const int core = CORE_OF[kit];
    if (core == SETUP_PAGE && s_page != SETUP_PAGE) {
        s_before_setup = s_page;
    }
    s_page = core;
    s_setup_visible.store(core == SETUP_PAGE, std::memory_order_relaxed);
    if (core == SETUP_PAGE && s_handlers.diagnostics != nullptr) {
        s_handlers.diagnostics();
    }
    if (s_handlers.radar != nullptr) {
        s_handlers.radar(core == RADAR_PAGE || core == HOME_PAGE, !owner_away());
    }
    publish(Topic::Page);
}

void follow_away()
{
    const auto away = [] {
        const bool gate = detail::settings_state().on[static_cast<int>(Setting::PresenceGate)];
        static bool present = false;
        const bool  moved   = std::exchange(present, detail::status_state().present) != detail::status_state().present;
        if (std::exchange(detail::s_presence_gate, gate) == gate && !moved) {
            return;
        }
        for (lv_obj_t *item : {s_items.calendar, s_items.radar}) {
            if (item != nullptr) {
                lv_obj_set_hidden(item, owner_away());
            }
        }
        detail::select_page(detail::s_page);  // away from a page that went
    };
    subscribe(Topic::Status, detail::kNoView, away);
    subscribe(Topic::Settings, detail::kNoView, away);
}

// Development: a picture of each page wanted to the console for
// tools/screenshot.py, a little after boot. Each holds the LVGL lock for
// several seconds, so it is off unless built with SHOT_ENABLED. One page a
// tick, so each is drawn before it is taken; the presence gate is lifted
// meanwhile, as the pages it hides are the ones most often worth a look.
constexpr std::uint32_t SHOT_START_MS = 25000;
constexpr std::uint32_t SHOT_PAGE_MS  = 2000;

void take_screenshots(lv_timer_t *timer)
{
    static const int PAGES[] = {detail::CALENDAR_PAGE};
    static int       step    = -1;
    static bool      gated   = false;
    if (step >= 0) {
        screenshot();
    } else {
        gated                   = detail::s_presence_gate;
        detail::s_presence_gate = false;
    }
    if (++step < static_cast<int>(std::size(PAGES))) {
        detail::select_page(PAGES[step]);
        lv_timer_set_period(timer, SHOT_PAGE_MS);
        return;
    }
    detail::s_presence_gate = gated;
    detail::select_page(detail::HOME_PAGE);
    lv_timer_delete(timer);
}
}  // namespace

bool owner_away() { return detail::s_presence_gate && !detail::status_state().present; }

void toggle_setup()
{
    detail::select_page(detail::s_page == detail::SETUP_PAGE ? s_before_setup : detail::SETUP_PAGE);
}
}  // namespace ui::kitfront

namespace ui::front {
using namespace kitfront;

void init(std::uint32_t accent)
{
    kit::init(kit::Look::Dark);
    if (accent != 0) {
        kit::set_accent(accent);
    }
    kit::screen(lv_screen_active());
    // Asked for a frame twice a refresh rather than about once: at 16 ms the
    // timer drifted against the panel's 60 Hz, and frames came one refresh
    // apart, then two, so a moving mark stuttered. Each still waits for the
    // panel to take the last, and asking costs nothing when nothing changed.
    lv_timer_set_period(lv_display_get_refr_timer(lv_display_get_default()), 8);
    // frames counted: a popover's picture made ahead is only current while none has been drawn since
#if defined(ESP_PLATFORM)
    kit::perf::start([] { return static_cast<std::uint64_t>(esp_timer_get_time()); });
#else
    kit::perf::start();
#endif
}

// One part a call, so the pages are made between frames rather than in one long stall.
bool build_next_part()
{
    static int next = 0;
    switch (next++) {
        case 0:
            s_items = build_dock(kit::layer(kit::Layer::Chrome));
            build_bar(kit::layer(kit::Layer::Chrome));
            return true;
        case 1:
            kit::add_page(s_items.home, build_home);
            kit::add_page(s_items.calendar, build_calendar);
            kit::add_page(s_items.radar, build_radar);
            kit::add_page(nullptr, build_setup);
            kit::on_page_shown(shown);
            // at once: on the panel a slide could not be drawn smoothly, the page
            // to come taking 400 ms to draw and each frame of it 45
            kit::set_change(kit::Change::Instant);
            kit::show_page(HOME, false);
            return true;
        case 2:
            build_views(lv_screen_active());
            follow_away();
            subscribe(Topic::Settings, detail::kNoView, [] {
                const bool light = detail::settings_state().on[static_cast<int>(Setting::Light)];
                kit::set_look(light ? kit::Look::Light : kit::Look::Dark);
            });
            detail::start_screen_schedule();
            if (SHOT_ENABLED) {
                lv_timer_create(take_screenshots, SHOT_START_MS, nullptr);
            }
            return true;
        default:
            return false;
    }
}
}  // namespace ui::front

namespace ui::detail {
bool s_presence_gate = true;  // the gate, as the radar's bench lifts it

void select_page(int index)
{
    if (index < 0 || index >= PAGE_COUNT || (kitfront::needs_presence(index) && kitfront::owner_away())) {
        index = HOME_PAGE;
    }
    kit::show_page(kitfront::KIT_OF[index], true);
}
}  // namespace ui::detail
