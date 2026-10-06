#include "kit.h"
#include "kit_fonts.h"

#include <cstring>
#include <iterator>

// The Pages sheet: a Home of widgets and three plain pages the dock's items
// choose between, with every way of changing between them to try on the panel.
namespace ui::lab {
namespace {

using namespace ui::kit;
namespace ic = ui::kit::glyph;

enum : int { HOME, AGENDA, RADAR, CHANGES };

const Zone CO2_ZONES[] = {{400, 800, Ink::Green}, {800, 1200, Ink::Amber}, {1200, 2000, Ink::Red}};

struct Option {
    const char *name;
    Change      how;
    const char *about;
};
constexpr Option OPTIONS[] = {
    {"Instant", Change::Instant, "At once: the new page in one frame, the mark sliding over in the dock."},
    {"Wipe", Change::Wipe, "From the dock side in eight bands, a band a frame, each an eighth of the page."},
    {"Spread", Change::Spread, "Out from the chosen item, up and down, a band a frame."},
    {"Stagger", Change::Stagger, "The page clears, then its tiles come a frame apart, those nearest the item first."},
    {"Slide", Change::Slide, "The pages slide across: all of the screen drawn every frame, to compare."},
    {"Fade", Change::Fade, "One page fades into the other: all of the screen drawn every frame, to compare."},
};

void (*s_back)() = nullptr;
lv_obj_t *s_about = nullptr, *s_last = nullptr;

lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow, std::int32_t gap)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_flex_flow(obj, flow);
    lv_obj_set_style_pad_row(obj, gap, 0);
    lv_obj_set_style_pad_column(obj, gap, 0);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return obj;
}

lv_obj_t *wrapped(lv_obj_t *parent, Role role, const char *value, Ink ink)
{
    lv_obj_t *t = text(parent, role, value, ink);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    return t;
}

void readout(lv_obj_t *body, const char *value, const char *note, Ink ink)
{
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    text(body, Role::ReadL, value);
    text(body, Role::Small, note, ink);
}

// --- Home ----------------------------------------------------------------

void build_home(lv_obj_t *page)
{
    lv_obj_t *grid = widget_grid(page);

    lv_obj_t *air = widget(grid, Span::Big, "AIR");
    lv_obj_set_flex_flow(air, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(air, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DialSpec co2;
    co2.size = 290, co2.min = 400, co2.max = 2000, co2.ticks = 31, co2.major = 5, co2.zones = CO2_ZONES;
    co2.zone_count = 3, co2.ends = true, co2.unit = "PPM CO2", co2.read = Role::ReadL;
    auto *dial = new Dial(air, co2);
    lv_obj_add_event_cb(dial->obj(), [](lv_event_t *e) { delete static_cast<Dial *>(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, dial);
    dial->set_value(640, false);

    readout(widget(grid, Span::One, "HUMIDITY"), "48%", "Good", Ink::Green);
    readout(widget(grid, Span::One, "OUTSIDE"), "13°", "Rain from 16:00", Ink::Secondary);

    lv_obj_t *sky = widget(grid, Span::Tall, "OVERHEAD");
    lv_obj_set_flex_flow(sky, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sky, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *middle = box(sky, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_width(middle, LV_PCT(100));
    lv_obj_set_flex_align(middle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *scope = well(middle);
    lv_obj_set_size(scope, 190, 190);
    lv_obj_set_style_radius(scope, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *ring = well(scope);
    lv_obj_set_size(ring, 96, 96);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(ring);
    lv_obj_align(icon(scope, ic::flight, 28, true, Ink::Accent), LV_ALIGN_CENTER, 34, -44);
    lv_obj_t *col = box(sky, LV_FLEX_FLOW_COLUMN, 2);
    text(col, Role::H3, "Boeing 747");
    text(col, Role::Small, "37,000 ft, 65 km south", Ink::Secondary);
    widget_opens(sky, RADAR);

    lv_obj_t *next = widget(grid, Span::Wide, "NEXT");
    lv_obj_set_flex_flow(next, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(next, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(next, space::xl, 0);
    text(next, Role::ReadL, "10:00");
    lv_obj_t *what = box(next, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_flex_grow(what, 1);
    lv_obj_set_style_pad_bottom(what, 6, 0);
    wrapped(what, Role::H3, "Computing and Communications", Ink::Primary);
    text(what, Role::Small, "Lecture, tomorrow, Hall 2", Ink::Secondary);

    readout(widget(grid, Span::One, "DESK"), "74.0", "CM", Ink::Tertiary);
    readout(widget(grid, Span::One, "FOCUS"), "24:51", "Until 10:06", Ink::Secondary);
}

// --- Agenda: a list that runs past the screen -----------------------------

struct Row {
    const char *at, *what, *where;
};

void group(lv_obj_t *page, const char *title, std::initializer_list<Row> rows)
{
    text(page, Role::Label, title, Ink::Secondary);
    lv_obj_t *t = tile(page, Lift::Low);
    lv_obj_set_size(t, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(t, space::s, 0);
    bool first = true;
    for (const Row &r : rows) {
        if (!first) {
            lv_obj_set_width(hairline(t, false, 0), LV_PCT(100));
        }
        first = false;
        lv_obj_t *row = box(t, LV_FLEX_FLOW_ROW, space::xl);
        lv_obj_set_size(row, LV_PCT(100), 76);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_width(text(row, Role::ReadS, r.at), 110);
        lv_obj_t *col = box(row, LV_FLEX_FLOW_COLUMN, 2);
        text(col, Role::Body, r.what);
        text(col, Role::Small, r.where, Ink::Secondary);
    }
}

void build_agenda(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, space::m, 0);
    group(page, "TODAY", {{"09:30", "Computing and Communications", "Lecture, Hall 2"},
                          {"12:15", "Lunch with Sam", "Canteen"},
                          {"14:00", "Lab session", "Room 3.14"},
                          {"16:30", "Train home", "Platform 5"}});
    group(page, "TOMORROW", {{"08:45", "Bike to the station", "About 20 minutes"},
                             {"10:00", "Computing and Communications", "Lecture, Hall 2"},
                             {"13:00", "Reading group", "Library"},
                             {"15:30", "Dentist", "Main street"},
                             {"19:00", "Dinner", "At home"}});
}

// --- Radar: one big tile --------------------------------------------------

void build_radar(lv_obj_t *page)
{
    lv_obj_t *t = tile(page, Lift::Low);
    lv_obj_set_size(t, inset::width, inset::height);
    text(t, Role::Label, "RADAR", Ink::Secondary);
    for (std::int32_t d : {500, 340, 180}) {
        lv_obj_t *ring = well(t);
        lv_obj_set_size(ring, d, d);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, 10);
    }
    lv_obj_align(icon(t, ic::flight, 34, true, Ink::Accent), LV_ALIGN_CENTER, 120, -110);
    lv_obj_t *col = box(t, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_align(col, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    text(col, Role::H2, "Boeing 747");
    text(col, Role::Small, "37,000 ft, 65 km south", Ink::Secondary);
}

// --- Changes: how pages give way ------------------------------------------

void show_option(int i)
{
    set_change(OPTIONS[i].how);
    if (std::strcmp(lv_label_get_text(s_about), OPTIONS[i].about) != 0) {
        lv_label_set_text(s_about, OPTIONS[i].about);
    }
}

void build_changes(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, space::l, 0);
    lv_obj_t *t = tile(page, Lift::Low);
    lv_obj_set_size(t, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, space::m, 0);
    text(t, Role::Label, "PAGE CHANGE", Ink::Secondary);
    int chosen = 0;
    for (int i = 0; i < static_cast<int>(std::size(OPTIONS)); ++i) {
        chosen = OPTIONS[i].how == change() ? i : chosen;
    }
    lv_obj_t *seg = segmented(t, {"Instant", "Wipe", "Spread", "Stagger", "Slide", "Fade"}, chosen, inset::width - 2 * space::xl);
    lv_obj_add_event_cb(seg, [](lv_event_t *e) { show_option(segmented_chosen(lv_event_get_target_obj(e))); },
                        LV_EVENT_VALUE_CHANGED, nullptr);
    s_about = wrapped(t, Role::Body, "", Ink::Secondary);
    lv_obj_set_height(s_about, 64);
    show_option(chosen);
    text(t, Role::Label, "LAST CHANGE", Ink::Secondary);
    s_last = wrapped(t, Role::Small, change_report(), Ink::Tertiary);
    lv_obj_set_height(s_last, 44);
    lv_timer_create(
        [](lv_timer_t *) {
            if (std::strcmp(lv_label_get_text(s_last), change_report()) != 0) {
                lv_label_set_text(s_last, change_report());
            }
        },
        300, nullptr);

    lv_obj_t *back = button(page, Button::Raised, ic::chevron_left, "The lab's sheets");
    lv_obj_add_event_cb(back, [](lv_event_t *) { s_back(); }, LV_EVENT_CLICKED, nullptr);
}

}  // namespace

void build_pages(lv_obj_t *const items[4], void (*shown)(int page), void (*back)())
{
    s_back = back;
    on_page_shown(shown);
    add_page(items[0], build_home);
    add_page(items[1], build_agenda);
    add_page(items[2], build_radar);
    add_page(items[3], build_changes);
}

}  // namespace ui::lab
