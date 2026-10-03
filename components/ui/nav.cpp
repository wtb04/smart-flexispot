#include "ui_internal.h"

#include "popout.h"
#include "settings_model.h"
#include "status_model.h"
#include "topics.h"

#include "esp_timer.h"

namespace ui::detail {
namespace {
lv_obj_t *s_pages[PAGE_COUNT]    = {};
lv_obj_t *s_nav_tabs[PAGE_COUNT] = {};

struct NavItem {
    const char           *icon;
    bool                  needs_presence;
    const lv_image_dsc_t *image = nullptr;  // drawn here, where the fonts have no glyph
};
// In the pages' order; Setup has no tab, the status opening it.
constexpr NavItem NAV_ITEMS[PAGE_COUNT] = {
    {LV_SYMBOL_HOME, false},
    {"", true, &icons::plane_icon},
    {"", true, &icons::calendar_icon},
    {"", false},
};
static_assert(HOME_PAGE == 0 && RADAR_PAGE == 1 && CALENDAR_PAGE == 2 && SETUP_PAGE == 3);

constexpr std::int32_t TAB_H    = 72;
constexpr std::int32_t PAGE_PAD = 20;

}  // namespace

int  s_page           = HOME_PAGE;
namespace {
int s_before_setup = HOME_PAGE;  // where Setup goes back to
}  // namespace

std::atomic<bool> s_setup_visible{false};

bool s_presence_gate = true;

bool owner_away()
{
    return s_presence_gate && !status_state().present;
}

namespace {
bool page_available(int index)
{
    return !NAV_ITEMS[index].needs_presence || !owner_away();
}

void paint_tab(int index, bool active)
{
    lv_obj_t *tab = s_nav_tabs[index];
    if (tab == nullptr) {
        return;  // Setup has none: the control bar's status opens it
    }
    lv_obj_set_state(tab, LV_STATE_CHECKED, active);
    const std::uint32_t ink  = active ? theme::text : theme::secondary;
    lv_obj_t           *icon = lv_obj_get_child(tab, 0);
    if (NAV_ITEMS[index].image != nullptr) {
        lv_obj_set_style_image_recolor(icon, lv_color_hex(ink), 0);
    } else {
        theme::set_text_color(icon, ink);
    }
}

void tell_page_opened(int index)
{
    if (index == SETUP_PAGE && s_handlers.diagnostics != nullptr) {
        s_handlers.diagnostics();
    }
    if (index == RADAR_PAGE || index == HOME_PAGE) {
        radar_page_opened();
    }
    if (index == CALENDAR_PAGE) {
        show_calendar();
    }
    if (s_handlers.radar != nullptr) {
        // Home's radar cards follow the sky as closely as the page does.
        s_handlers.radar(index == RADAR_PAGE || index == HOME_PAGE, page_available(RADAR_PAGE));
    }
}
}  // namespace

void select_page(int index)
{
    if (!page_available(index)) {
        index = HOME_PAGE;
    }
    s_page = index;
    if (index != SETUP_PAGE) {
        s_before_setup = index;
    }
    s_setup_visible.store(index == SETUP_PAGE, std::memory_order_relaxed);

    tell_page_opened(index);
    show_guest_presets();
    for (int i = 0; i < PAGE_COUNT; ++i) {
        if (s_nav_tabs[i] != nullptr) {
            lv_obj_set_hidden(s_nav_tabs[i], !page_available(i));
        }
        lv_obj_set_hidden(s_pages[i], i != index);
        paint_tab(i, i == index);
    }
    close_popout();
    publish(Topic::Page);
}
void toggle_setup()
{
    select_page(s_page == SETUP_PAGE ? s_before_setup : SETUP_PAGE);
}

namespace {
void nav_event_cb(lv_event_t *e)
{
    select_page(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))));
}
}  // namespace

namespace {
lv_obj_t *make_bare_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_scrollable(box, false);
    return box;
}

lv_obj_t *make_tab_icon(lv_obj_t *tab, const NavItem &item)
{
    if (item.image == nullptr) {
        return theme::make_label(tab, item.icon, theme::secondary, fonts::size_28());
    }
    return theme::make_icon(tab, item.image, theme::secondary);
}

lv_obj_t *make_nav_tab(lv_obj_t *dock, int index)
{
    lv_obj_t *tab = lv_button_create(dock);
    lv_obj_set_size(tab, lv_pct(100), TAB_H);
    theme::style_button(tab, theme::panel_light);
    theme::fill_accent(tab, LV_STATE_CHECKED);
    // As the finger lands rather than as it lifts: the page is there a tap's length sooner.
    lv_obj_add_event_cb(tab, nav_event_cb, LV_EVENT_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));

    lv_obj_center(make_tab_icon(tab, NAV_ITEMS[index]));
    return tab;
}

lv_obj_t *make_page(lv_obj_t *area, const Layout &l)
{
    lv_obj_t *page = lv_obj_create(area);
    lv_obj_set_size(page, l.content_w, l.content_h);
    lv_obj_set_align(page, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(page, 0, 0);
    theme::style_panel(page);
    lv_obj_set_style_pad_all(page, PAGE_PAD, 0);
    return page;
}

using PageBuilder = void (*)(lv_obj_t *page, std::int32_t width, std::int32_t height);

void build_padded_page(int index, PageBuilder build, const Layout &l)
{
    lv_obj_set_style_pad_all(s_pages[index], PANEL_PAD, 0);
    build(s_pages[index], l.content_w - 2 * PANEL_PAD, l.content_h - 2 * PANEL_PAD);
}
}  // namespace

void create_content(lv_obj_t *parent)
{
    const Layout l = layout();

    lv_obj_t *area = make_bare_box(parent);
    s_content      = area;
    lv_obj_set_pos(area, l.content_x, l.content_y);
    lv_obj_set_size(area, l.content_w, l.content_h);

    for (int i = 0; i < PAGE_COUNT; ++i) {
        if (i != SETUP_PAGE) {
            s_nav_tabs[i] = make_nav_tab(dock_tabs(), i);
        }
        s_pages[i] = make_page(area, l);
    }
    dock_tabs_done(s_nav_tabs[HOME_PAGE]);
}

bool build_next_page()
{
    static int  next = 0;
    const Layout l   = layout();
    switch (next++) {
        case 0: build_home_page(s_pages[HOME_PAGE]); return true;
        case 1:
            // The map is the page's panel itself, to its rounded edge on the screen's black.
            lv_obj_set_style_bg_opa(s_pages[RADAR_PAGE], LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_all(s_pages[RADAR_PAGE], 0, 0);
            build_radar_page(s_pages[RADAR_PAGE], l.content_w, l.content_h);
            return true;
        case 2: build_padded_page(CALENDAR_PAGE, build_calendar_page, l); return true;
        case 3:
            build_settings_page(s_pages[SETUP_PAGE]);
            select_page(HOME_PAGE);
            return false;
        default: return false;
    }
}


void follow_pages()
{
    const auto pages = [] {
        const bool gate = settings_state().on[static_cast<int>(Setting::PresenceGate)];
        static bool s_present_shown = false;
        const bool  moved = std::exchange(s_present_shown, status_state().present) != status_state().present;
        if (std::exchange(s_presence_gate, gate) != gate || moved) {
            select_page(s_page);
        }
    };
    subscribe(Topic::Status, kNoView, pages);
    subscribe(Topic::Settings, kNoView, pages);
    // Only once all that arrives is in and checked, not while any still comes.
    subscribe(Topic::Update, kNoView, [] {
        const UpdateState &update = settings_state().update;
        paint_setup_dot(settings_state().update_known && (update.panel_ready || update.companion_ready) &&
                        update.busy == UpdateTarget::None);
    });
}
}  // namespace ui::detail

namespace ui {
namespace {
constexpr std::int64_t SETTLE_US = 700'000;  // what follows a switch: the tab easing in, a feed's answer

std::int64_t s_render_began = 0;
std::int64_t s_render_us    = 0;  // of a frame, the drawing; the rest is turning it onto the panel

void render_timed(lv_event_t *e)
{
    const std::int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
        s_render_began = now;
    } else {
        s_render_us += now - s_render_began;
    }
}

struct Switch {
    std::int64_t select_us = 0;  // select_page() itself, before anything is drawn
    std::int64_t first_us  = 0;  // the frame that shows the page
    std::int64_t drawn_us  = 0;  // and of it, the drawing
    std::int64_t busy_us   = 0;  // the LVGL work while it settles
    std::int64_t worst_us  = 0;
    int          calls     = 0;  // of those taking a millisecond or more
};

Switch time_switch(int page)
{
    Switch t;
    std::int64_t at = esp_timer_get_time();
    detail::select_page(page);
    t.select_us = esp_timer_get_time() - at;
    at          = esp_timer_get_time();
    s_render_us = 0;
    lv_refr_now(nullptr);
    t.first_us              = esp_timer_get_time() - at;
    t.drawn_us              = s_render_us;
    const std::int64_t till = esp_timer_get_time() + SETTLE_US;
    while (esp_timer_get_time() < till) {
        at = esp_timer_get_time();
        lv_timer_handler();
        const std::int64_t took = esp_timer_get_time() - at;
        if (took >= 1000) {
            t.busy_us += took;
            t.worst_us = std::max(t.worst_us, took);
            ++t.calls;
        }
    }
    return t;
}
}  // namespace

int bench_pages(char *out, std::size_t size)
{
    using namespace detail;
    const int  page = s_page;
    const bool gate = s_presence_gate;
    s_presence_gate = false;  // every page, the phone there or not
    lv_display_t *display = lv_display_get_default();
    lv_display_add_event_cb(display, render_timed, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(display, render_timed, LV_EVENT_RENDER_READY, nullptr);
    time_switch(HOME_PAGE);
    static constexpr const char *NAMES[PAGE_COUNT] = {"home", "radar", "calendar", "setup"};
    static constexpr int         ORDER[]           = {RADAR_PAGE, CALENDAR_PAGE, SETUP_PAGE, HOME_PAGE,
                                                      CALENDAR_PAGE, HOME_PAGE, RADAR_PAGE, HOME_PAGE};
    int n = std::snprintf(out, size, "%-9s %7s %7s %7s %13s %7s\n", "to", "select", "frame", "drawn", "then (calls)",
                          "worst");
    for (const int to : ORDER) {
        const Switch t = time_switch(to);
        n += std::snprintf(out + n, size - n, "%-9s %5.1fms %5.1fms %5.1fms %7.1fms (%2d) %5.1fms\n", NAMES[to],
                           t.select_us / 1000.0, t.first_us / 1000.0, t.drawn_us / 1000.0, t.busy_us / 1000.0,
                           t.calls, t.worst_us / 1000.0);
    }
    lv_display_remove_event_cb_with_user_data(display, render_timed, nullptr);
    s_presence_gate = gate;
    select_page(page);
    return n;
}
}  // namespace ui
