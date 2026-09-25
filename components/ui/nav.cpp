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
    [FOCUS_PAGE]    = {"", "Focus", true, &icons::timer_icon},
    [SETUP_PAGE]    = {LV_SYMBOL_SETTINGS, "Setup", false},
};

// Presets 5 and 6 are for whoever uses the desk while the phone is away.
constexpr int GUEST_PRESETS[] = {4, 5};

constexpr std::int32_t TAB_H          = NAV_H - 2 * (PANEL_PAD / 2);
constexpr std::int32_t TAB_ICON_DY    = -12;
constexpr std::int32_t TAB_CAPTION_DY = 16;
constexpr std::int32_t PAGE_PAD       = 20;

/** The buttons showing share the drawer's height, up to the rail's own size. */
void fit_drawer_buttons()
{
    const std::uint32_t buttons = lv_obj_get_child_count(s_drawer);
    int                 showing = 0;
    for (std::uint32_t i = 0; i < buttons; ++i) {
        showing += lv_obj_is_hidden(lv_obj_get_child(s_drawer, i)) ? 0 : 1;
    }
    const std::int32_t inner  = layout().screen_h - 2 * GAP - 2 * PANEL_PAD;
    const std::int32_t height = std::min<std::int32_t>(
        RAIL_BTN_H, (inner - (showing - 1) * BUTTON_GAP) / showing);
    for (std::uint32_t i = 0; i < buttons; ++i) {
        lv_obj_set_height(lv_obj_get_child(s_drawer, i), height);
    }
}
}  // namespace

bool s_present        = false;
int  s_page           = HOME_PAGE;

std::atomic<bool> s_setup_visible{false};

bool s_presence_gate = true;

void show_guest_presets()
{
    if (s_drawer == nullptr || s_preset_buttons[GUEST_PRESETS[0]] == nullptr) {
        return;
    }
    const bool shown = !s_presence_gate || !s_present;
    for (const int index : GUEST_PRESETS) {
        lv_obj_set_hidden(s_preset_buttons[index], !shown);
    }
    fit_drawer_buttons();
}
namespace {
bool page_available(int index)
{
    return !NAV_ITEMS[index].needs_presence || !s_presence_gate || s_present;
}

void paint_tab(int index, bool active)
{
    lv_obj_t *tab = s_nav_tabs[index];
    lv_obj_set_state(tab, LV_STATE_CHECKED, active);
    const std::uint32_t ink  = active ? theme::text : theme::secondary;
    lv_obj_t           *icon = lv_obj_get_child(tab, 0);
    if (NAV_ITEMS[index].image != nullptr) {
        lv_obj_set_style_image_recolor(icon, lv_color_hex(ink), 0);
    } else {
        theme::set_text_color(icon, ink);
    }
    theme::set_text_color(lv_obj_get_child(tab, 1), ink);
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
        s_handlers.radar(index == RADAR_PAGE, page_available(RADAR_PAGE));
    }
}
}  // namespace

void select_page(int index)
{
    if (!page_available(index)) {
        index = HOME_PAGE;
    }
    s_page = index;
    s_setup_visible.store(index == SETUP_PAGE, std::memory_order_relaxed);

    tell_page_opened(index);
    show_guest_presets();
    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_set_hidden(s_nav_tabs[i], !page_available(i));
        lv_obj_set_hidden(s_pages[i], i != index);
        paint_tab(i, i == index);
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
lv_obj_t *make_bare_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_scrollable(box, false);
    return box;
}

lv_obj_t *make_nav_bar(lv_obj_t *area, std::int32_t width)
{
    lv_obj_t *nav = make_bare_box(area);
    lv_obj_set_size(nav, width, NAV_H);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, BUTTON_GAP, 0);
    return nav;
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

lv_obj_t *make_nav_tab(lv_obj_t *nav, int index, std::int32_t width)
{
    lv_obj_t *tab = lv_button_create(nav);
    lv_obj_set_size(tab, width, TAB_H);
    theme::style_button(tab, theme::panel_light);
    theme::fill_accent(tab, LV_STATE_CHECKED);
    lv_obj_add_event_cb(tab, nav_event_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));

    const NavItem &item = NAV_ITEMS[index];
    lv_obj_align(make_tab_icon(tab, item), LV_ALIGN_CENTER, 0, TAB_ICON_DY);
    lv_obj_t *caption = theme::make_label(tab, item.caption, theme::secondary, fonts::size_16());
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, TAB_CAPTION_DY);
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
    lv_obj_set_pos(area, l.content_x, GAP);
    lv_obj_set_size(area, l.content_w, l.screen_h - GAP - EDGE_GAP);

    lv_obj_t          *nav   = make_nav_bar(area, l.content_w);
    const std::int32_t tab_w = (l.content_w - (PAGE_COUNT - 1) * BUTTON_GAP) / PAGE_COUNT;
    for (int i = 0; i < PAGE_COUNT; ++i) {
        s_nav_tabs[i] = make_nav_tab(nav, i, tab_w);
        s_pages[i]    = make_page(area, l);
    }

    build_home_page(s_pages[HOME_PAGE]);
    build_padded_page(RADAR_PAGE, build_radar_page, l);
    build_padded_page(CALENDAR_PAGE, build_calendar_page, l);
    build_padded_page(FOCUS_PAGE, build_focus_page, l);
    build_settings_page(s_pages[SETUP_PAGE]);

    select_page(HOME_PAGE);
}

}  // namespace ui::detail
