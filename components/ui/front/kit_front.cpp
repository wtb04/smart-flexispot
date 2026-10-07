// The screen as the design kit draws it, on the shared core in ui.cpp: being
// built beside classic_front.cpp, which the panel still runs, until it does
// all that one does.

#include "kit_front.h"

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#endif

#include "settings_model.h"
#include "theme.h"  // the radar keeps the old one's map and planes, and their styles
#include "units_font.h"
#include "status_model.h"
#include "topics.h"

#include <cstdio>
#include <utility>

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
    fonts::init();
    theme::init_accents();
    if (accent != 0) {
        theme::set_primary(accent);
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
            detail::status_splash_gone();  // no start-up screen yet: what waits for it may show
            return true;
        default:
            return false;
    }
}

void apply() {}
}  // namespace ui::front

namespace ui::detail {
bool s_presence_gate = true;  // the gate, as the radar's bench lifts it
bool owner_away() { return kitfront::owner_away(); }  // the radar's, which the old screen's nav.cpp has

void select_page(int index)
{
    if (index < 0 || index >= PAGE_COUNT || (kitfront::needs_presence(index) && kitfront::owner_away())) {
        index = HOME_PAGE;
    }
    kit::show_page(kitfront::KIT_OF[index], true);
}
}  // namespace ui::detail

namespace ui {
esp_err_t splash_step(const char *) { return ESP_OK; }  // the start-up screen comes later
esp_err_t splash_done() { return ESP_OK; }

// The benches of the old screen's dock and pages; the kit's own is the lab's.
namespace {
int not_here(char *out, std::size_t size)
{
    return std::snprintf(out, size, "not in the design kit's screen: its bench is /bench?lab\n");
}
}  // namespace
int bench_desk_card(char *out, std::size_t size) { return not_here(out, size); }
int bench_pages(char *out, std::size_t size) { return not_here(out, size); }
}  // namespace ui
