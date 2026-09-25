#include "ui_internal.h"

namespace ui::detail {
namespace {
lv_obj_t *s_pages[PAGE_COUNT]    = {};
lv_obj_t *s_nav_tabs[PAGE_COUNT] = {};

struct NavItem {
    const char           *icon;
    const char           *caption;
    bool                  needs_presence;
    const lv_image_dsc_t *image = nullptr;  // drawn here, where the fonts have no glyph
};
constexpr NavItem NAV_ITEMS[PAGE_COUNT] = {
    [HOME_PAGE]     = {LV_SYMBOL_HOME, "Home", false},
    [RADAR_PAGE]    = {"", "Radar", true, &icons::plane_icon},
    [CALENDAR_PAGE] = {"", "Calendar", true, &icons::calendar_icon},
    [ALERTS_PAGE]   = {LV_SYMBOL_BELL, "Alerts", true},
    [SETUP_PAGE]    = {LV_SYMBOL_SETTINGS, "Setup", false},
};
}  // namespace

bool s_present        = false;
int  s_page           = HOME_PAGE;

std::atomic<bool> s_setup_visible{false};

bool s_presence_gate = true;

// Presets 5 and 6 are for whoever uses the desk while the phone is away.
void show_guest_presets()
{
    if (s_drawer == nullptr || s_preset_buttons[4] == nullptr) {
        return;
    }
    const bool shown = !s_presence_gate || !s_present;
    lv_obj_set_hidden(s_preset_buttons[4], !shown);
    lv_obj_set_hidden(s_preset_buttons[5], !shown);
    const Layout       l      = layout();
    const int          count  = shown ? 6 : 4;
    const std::int32_t inner  = l.screen_h - 2 * GAP - 2 * PANEL_PAD;
    const std::int32_t height = std::min<std::int32_t>(RAIL_BTN_H, (inner - (count - 1) * BUTTON_GAP) / count);
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(s_drawer); ++i) {
        lv_obj_set_height(lv_obj_get_child(s_drawer, i), height);
    }
}
namespace {
bool page_available(int index)
{
    return !NAV_ITEMS[index].needs_presence || !s_presence_gate || s_present;
}
}  // namespace

void select_page(int index)
{
    if (!page_available(index)) {
        index = HOME_PAGE;
    }
    s_page = index;
    s_setup_visible.store(index == SETUP_PAGE, std::memory_order_relaxed);

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
        s_handlers.radar(index == RADAR_PAGE, page_available(RADAR_PAGE));
    }
    show_guest_presets();
    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_set_hidden(s_nav_tabs[i], !page_available(i));
        lv_obj_set_hidden(s_pages[i], i != index);
        const bool active = (i == index);
        lv_obj_set_state(s_nav_tabs[i], LV_STATE_CHECKED, active);
        const std::uint32_t ink = active ? theme::text : theme::secondary;
        lv_obj_t *icon = lv_obj_get_child(s_nav_tabs[i], 0);
        if (NAV_ITEMS[i].image != nullptr) {
            lv_obj_set_style_image_recolor(icon, lv_color_hex(ink), 0);
        } else {
            theme::set_text_color(icon, ink);
        }
        theme::set_text_color(lv_obj_get_child(s_nav_tabs[i], 1), ink);
    }
}
namespace {
void nav_event_cb(lv_event_t *e)
{
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
void build_placeholder_page(lv_obj_t *page, const char *title, const char *blurb)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, 14, 0);
    theme::make_label(page, title, theme::text, fonts::size_32());
    theme::make_label(page, blurb, theme::secondary, fonts::size_20());
}
}  // namespace

void create_content(lv_obj_t *parent)
{
    const Layout l = layout();

    lv_obj_t *area = lv_obj_create(parent);
    s_content      = area;
    lv_obj_set_pos(area, l.content_x, GAP);
    lv_obj_set_size(area, l.content_w, l.screen_h - GAP - EDGE_GAP);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_set_scrollable(area, false);

    lv_obj_t *nav = lv_obj_create(area);
    lv_obj_set_size(nav, l.content_w, NAV_H);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_pad_all(nav, 0, 0);
    lv_obj_set_scrollable(nav, false);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, BUTTON_GAP, 0);

    const std::int32_t tab_w =
        (l.content_w - (PAGE_COUNT - 1) * BUTTON_GAP) / PAGE_COUNT;

    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_t *tab = lv_button_create(nav);
        lv_obj_set_size(tab, tab_w, NAV_H - 2 * (PANEL_PAD / 2));
        theme::style_button(tab, theme::panel_light);
        theme::fill_accent(tab, LV_STATE_CHECKED);
        lv_obj_add_event_cb(tab, nav_event_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));

        lv_obj_t *icon = nullptr;
        if (NAV_ITEMS[i].image != nullptr) {
            icon = lv_image_create(tab);
            lv_image_set_src(icon, NAV_ITEMS[i].image);
            lv_obj_set_style_image_recolor(icon, lv_color_hex(theme::secondary), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        } else {
            icon = theme::make_label(tab, NAV_ITEMS[i].icon, theme::secondary, fonts::size_28());
        }
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -12);
        lv_obj_t *caption = theme::make_label(tab, NAV_ITEMS[i].caption, theme::secondary,
                                              fonts::size_16());
        lv_obj_align(caption, LV_ALIGN_CENTER, 0, 16);
        s_nav_tabs[i] = tab;

        lv_obj_t *page = lv_obj_create(area);
        lv_obj_set_size(page, l.content_w, l.content_h);
        lv_obj_set_align(page, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(page, 0, 0);
        theme::style_panel(page);
        lv_obj_set_style_pad_all(page, 20, 0);
        s_pages[i] = page;
    }

    build_home_page(s_pages[HOME_PAGE]);

    lv_obj_set_style_pad_all(s_pages[RADAR_PAGE], PANEL_PAD, 0);
    build_radar_page(s_pages[RADAR_PAGE], l.content_w - 2 * PANEL_PAD, l.content_h - 2 * PANEL_PAD);
    lv_obj_set_style_pad_all(s_pages[CALENDAR_PAGE], PANEL_PAD, 0);
    build_calendar_page(s_pages[CALENDAR_PAGE], l.content_w - 2 * PANEL_PAD,
                        l.content_h - 2 * PANEL_PAD);
    build_placeholder_page(s_pages[ALERTS_PAGE], "Alerts",
                           "Reminders to stand, and whatever Home Assistant sends.");
    build_settings_page(s_pages[SETUP_PAGE]);

    select_page(HOME_PAGE);
}

}  // namespace ui::detail
