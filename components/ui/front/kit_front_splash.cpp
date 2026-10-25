// The start-up screen is the frame building itself: lit first, the dock and
// the bar come as they are made and fill as what they show arrives, each
// page's icon in the dock lights up as that page is built, and where the pages
// will be, the name and what start-up is doing. Once Home Assistant is up the
// home page is there under the dock that already was, with no cut.

#include "kit_front.h"

#include "esp_log.h"
#include "kit_fonts.h"
#include "topics.h"
#include "ui_core.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

#if defined(ESP_PLATFORM)
#include "esp_app_desc.h"
#endif

namespace ui::kitfront {
namespace {
constexpr char TAG[] = "splash";

using kit::Ink;
using kit::Role;

// Driven by the clock alone, so a slow frame does not slow it: the rail fills
// in eight seconds, and what is left of it in a moment once all is up.
constexpr std::uint32_t TIMELINE_MS = 8000;
constexpr float         RUSH_MS     = 600.0f;
constexpr std::uint32_t FRAME_MS    = 16;
constexpr std::uint32_t GUARD_MS    = 15000;  // gone by then, up or not
constexpr std::int32_t  RAIL_STEPS  = 1000;
constexpr std::int32_t  RAIL_W      = 420;

// The pages after Home, built one at a time as the rail passes these shares
// of it, or at once when all is up; the dock's icon of each lights as it is.
struct Ahead {
    int   page;
    float at;
};
constexpr Ahead AHEAD[] = {{CALENDAR, 0.25f}, {RADAR, 0.5f}, {SETUP, 0.75f}};
constexpr int   AHEAD_COUNT = static_cast<int>(std::size(AHEAD));

// What start-up is doing, by the steps it has said it took.
constexpr const char *DOING[] = {"Starting the desk", "Joining Wi-Fi", "Reaching Home Assistant"};

lv_obj_t     *s_words = nullptr, *s_status = nullptr, *s_about = nullptr, *s_shield = nullptr;
kit::Rail    *s_rail  = nullptr;
lv_obj_t     *s_icons[PAGE_COUNT] = {};  // the dock's, by page; Setup has none
lv_timer_t   *s_tick = nullptr;
std::uint32_t s_started = 0, s_ticked = 0;
float         s_at = 0.0f, s_rush = 0.0f;
bool          s_ready = false, s_framed = false;
int           s_built = 0;  // of AHEAD

lv_obj_t *bare(lv_obj_t *parent)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

void hide_behind(bool hidden)
{
    for (kit::Layer l : {kit::Layer::Content, kit::Layer::Overlay, kit::Layer::Alert}) {
        lv_obj_set_hidden(kit::layer(l), hidden);
    }
}

void build_ahead()
{
    const int page = AHEAD[s_built++].page;
    kit::build_page(page);
    if (s_icons[page] != nullptr) {
        kit::set_usable(s_icons[page], true);
    }
}

// Not a fade of the whole screen, which takes a quarter of a second a frame
// here: the words go, and the pages are there under the dock.
void leave()
{
    ESP_LOGI(TAG, "gone after %u ms", static_cast<unsigned>(lv_tick_elaps(s_started)));
    lv_timer_delete(s_tick);
    s_tick = nullptr;
    delete s_rail;
    s_rail = nullptr;
    lv_obj_delete(s_words);
    lv_obj_delete(s_shield);
    s_words = s_shield = nullptr;
    hide_behind(false);
    detail::publish(Topic::Notices);  // the ones that came meanwhile, now there is somewhere to see them
}

void tick(lv_timer_t *)
{
    if (!s_ready && (detail::splash_done_wanted() || lv_tick_elaps(s_started) >= GUARD_MS)) {
        s_ready = true;
        ESP_LOGI(TAG, "ready after %u ms", static_cast<unsigned>(lv_tick_elaps(s_started)));
    }
    const int steps = std::min(detail::splash_steps_wanted(), static_cast<int>(std::size(DOING)) - 1);
    kit::set_words(s_status, s_ready ? "Ready" : DOING[steps]);

    const auto real = static_cast<float>(lv_tick_elaps(s_ticked));
    s_ticked        = lv_tick_get();
    const auto end  = static_cast<float>(TIMELINE_MS);
    if (s_ready && s_rush == 0.0f) {
        s_rush = std::max(1.0f, (end - s_at) / RUSH_MS);
    }
    s_at = std::min(s_at + real * (s_ready ? s_rush : 1.0f), s_ready ? end : end * 0.95f);
    s_rail->set_value(static_cast<std::int32_t>(s_at / end * RAIL_STEPS), false);

    // a page a frame at most, as each takes a while to build
    if (s_framed && s_built < AHEAD_COUNT && (s_ready || s_at >= AHEAD[s_built].at * end)) {
        build_ahead();
        return;
    }
    if (s_ready && s_at >= end && s_built == AHEAD_COUNT) {
        leave();
    }
}
}  // namespace

void build_splash()
{
    // Over everything, taking the taps: the dock and the bar are there to be
    // seen before what they open is. A tap shows the build.
    s_shield = bare(lv_layer_top());
    lv_obj_set_size(s_shield, kit::size::screen_w, kit::size::screen_h);
    lv_obj_add_flag(s_shield, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_shield, [](lv_event_t *) { lv_obj_set_hidden(s_about, !lv_obj_is_hidden(s_about)); },
                        LV_EVENT_CLICKED, nullptr);

    s_words = bare(kit::layer(kit::Layer::Chrome));
    lv_obj_set_size(s_words, RAIL_W + 2 * kit::space::xxl, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_words, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    kit::text(s_words, Role::H1, "Smart Flexispot");
    kit::text(s_words, Role::Body, "by Wouter ten Brinke", Ink::Secondary);
    lv_obj_t *gap = bare(s_words);
    lv_obj_set_size(gap, 1, kit::space::xxl * 2);
    kit::RailSpec spec;
    spec.length = RAIL_W;
    spec.max    = RAIL_STEPS;
    spec.quiet  = true;
    s_rail      = new kit::Rail(s_words, spec);
    s_status    = kit::text(s_words, Role::Small, DOING[0], Ink::Secondary);
    lv_obj_set_style_margin_top(s_status, kit::space::m, 0);
    // the rail's width, so the line under it never shifts
    lv_obj_set_width(s_status, RAIL_W);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);

    char about[64];
#if defined(ESP_PLATFORM)
    std::snprintf(about, sizeof about, "Smart Flexispot, build %s", esp_app_get_description()->version);
#else
    std::snprintf(about, sizeof about, "Smart Flexispot, simulator");
#endif
    s_about = kit::text(s_words, Role::Label, about, Ink::Tertiary);
    lv_obj_set_style_margin_top(s_about, kit::space::xl, 0);
    lv_obj_set_hidden(s_about, true);
    lv_obj_align(s_words, LV_ALIGN_CENTER, 0, 0);

    hide_behind(true);
    s_started = s_ticked = lv_tick_get();
    s_tick               = lv_timer_create(tick, FRAME_MS, nullptr);
}

void splash_frame_built(const DockItems &items)
{
    s_icons[HOME]     = items.home;
    s_icons[CALENDAR] = items.calendar;
    s_icons[RADAR]    = items.radar;
    for (int page : {CALENDAR, RADAR}) {
        if (s_icons[page] != nullptr) {
            kit::set_usable(s_icons[page], false);  // until its page is built
        }
    }
    // the words in the middle of where the pages will be, right of the dock, under the bar
    lv_area_t dock{};
    lv_obj_get_coords(lv_obj_get_parent(items.home), &dock);
    const std::int32_t left = dock.x2 + 1, top = kit::size::bar + kit::space::l;
    lv_obj_align(s_words, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_update_layout(s_words);
    lv_obj_set_pos(s_words, left + (kit::size::screen_w - left - lv_obj_get_width(s_words)) / 2,
                   top + (kit::size::screen_h - top - lv_obj_get_height(s_words)) / 2);
    s_framed = true;
}

bool splash_showing() { return s_shield != nullptr; }
}  // namespace ui::kitfront

namespace ui::detail {
bool splash_showing() { return kitfront::splash_showing(); }
}  // namespace ui::detail
