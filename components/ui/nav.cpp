#include "ui_internal.h"

#include "settings_model.h"
#include "status_model.h"
#include "topics.h"

namespace ui::detail {
namespace {
lv_obj_t *s_pages[PAGE_COUNT]    = {};
lv_obj_t *s_nav_tabs[PAGE_COUNT] = {};

struct NavItem {
    const char           *icon;
    const char           *name;
    bool                  needs_presence;
    const lv_image_dsc_t *image = nullptr;  // drawn here, where the fonts have no glyph
};
constexpr NavItem NAV_ITEMS[PAGE_COUNT] = {
    [HOME_PAGE]     = {LV_SYMBOL_HOME, "Home", false},
    [RADAR_PAGE]    = {"", "Radar", true, &icons::plane_icon},
    [CALENDAR_PAGE] = {"", "Calendar", true, &icons::calendar_icon},
    [SETUP_PAGE]    = {LV_SYMBOL_SETTINGS, "Setup", false},
};

constexpr std::int32_t TAB_H    = 72;
constexpr std::int32_t PAGE_PAD = 20;

}  // namespace

int  s_page           = HOME_PAGE;
namespace {
int s_before_setup = HOME_PAGE;  // where Setup goes back to
}  // namespace

std::atomic<bool> s_setup_visible{false};

bool s_presence_gate = true;

namespace {
bool page_available(int index)
{
    return !NAV_ITEMS[index].needs_presence || !s_presence_gate || status_state().present;
}

void paint_tab(int index, bool active)
{
    lv_obj_t *tab = s_nav_tabs[index];
    if (tab == nullptr) {
        return;  // Setup has none: the top row's status opens it
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
    if (index == RADAR_PAGE) {
        radar_page_opened();
    }
    if (index == CALENDAR_PAGE) {
        show_calendar();
    }
    if (s_handlers.radar != nullptr) {
        // Home's tile follows the sky as closely as the page does.
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
    publish(Topic::Page);
}
void toggle_setup()
{
    open_desk_sheet(false);
    select_page(s_page == SETUP_PAGE ? s_before_setup : SETUP_PAGE);
}

namespace {
void nav_event_cb(lv_event_t *e)
{
    open_desk_sheet(false);
    select_page(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))));
}
}  // namespace

lv_obj_t *s_brightness_value = nullptr;

void brightness_event_cb(lv_event_t *e)
{
    auto      *slider  = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const int  percent = static_cast<int>(lv_slider_get_value(slider));

    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", percent);
    theme::set_text(s_brightness_value, text);

    if (s_handlers.brightness != nullptr) {
        s_handlers.brightness(percent);
    }
}
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
    lv_obj_t *icon = lv_image_create(tab);
    lv_image_set_src(icon, item.image);
    lv_obj_set_style_image_recolor(icon, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    return icon;
}

lv_obj_t *make_nav_tab(lv_obj_t *dock, int index)
{
    lv_obj_t *tab = lv_button_create(dock);
    lv_obj_set_size(tab, lv_pct(100), TAB_H);
    theme::style_button(tab, theme::panel_light);
    theme::fill_accent(tab, LV_STATE_CHECKED);
    lv_obj_add_event_cb(tab, nav_event_cb, LV_EVENT_CLICKED,
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
}

bool build_next_page()
{
    static int  next = 0;
    const Layout l   = layout();
    switch (next++) {
        case 0: build_home_page(s_pages[HOME_PAGE]); return true;
        case 1: build_padded_page(RADAR_PAGE, build_radar_page, l); return true;
        case 2: build_padded_page(CALENDAR_PAGE, build_calendar_page, l); return true;
        case 3:
            build_settings_page(s_pages[SETUP_PAGE]);
            select_page(HOME_PAGE);
            return false;
        default: return false;
    }
}


}  // namespace ui::detail

namespace ui::detail {
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
