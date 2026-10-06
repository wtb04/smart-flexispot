#include "lab_lists.h"

#include "kit.h"
#include "kit_fonts.h"
#include "lvgl_private.h"  // for the study, the areas each frame redraws

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace ui::lab::lists {
namespace {

using namespace ui::kit;
namespace ic = ui::kit::glyph;

constexpr std::int32_t COL_A = 352, COL_B = 360, COL_C = 378, SHEET_H = 524;
constexpr std::int32_t SECTION_H = 31;
constexpr std::uint32_t ACCENTS[] = {0, 0x5aa9ff, 0x7ccf6a, 0xb48cff};
const char *const ACCENT_NAMES[] = {"Orange", "Blue", "Green", "Violet"};

lv_obj_t *s_root = nullptr, *s_cal = nullptr, *s_cal_tile = nullptr, *s_cal_empty = nullptr;
lv_obj_t *s_desk = nullptr, *s_about = nullptr, *s_log = nullptr, *s_accent_row = nullptr, *s_live = nullptr;
lv_obj_t *s_sheet = nullptr;  // the scrolling body of the open sheet
Readout  *s_co2 = nullptr, *s_temp = nullptr;
Stepper  *s_target = nullptr;
int       s_accent = 0;

template <typename T> void own(lv_obj_t *owner, T *thing)
{
    lv_obj_add_event_cb(owner, [](lv_event_t *e) { delete static_cast<T *>(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, thing);
}

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

// --- what the lab shows ----------------------------------------------------

struct Event {
    const char *time, *title, *place;
    int         feed;
};
constexpr std::uint32_t FEEDS[] = {0xff8a3d, 0x6fb6ff, 0x86d27c, 0xb48cff};
const char *const DAYS[][2] = {{"TODAY", "6 OCT"}, {"TOMORROW", "7 OCT"}, {"THURSDAY", "8 OCT"},
                               {"FRIDAY", "9 OCT"}, {"MONDAY", "12 OCT"}};
const Event EVENTS[] = {
    {"08:45", "Computing and Communications", "Lecture, Hall 2", 0},
    {"10:30", "Signals and Systems", "Tutorial, Room 2042", 0},
    {"12:00", "Lunch with Sam", "Canteen", 2},
    {"13:45", "Project meeting", "Online", 1},
    {"15:30", "Linear Algebra", "Lecture, Hall 4", 0},
    {"17:00", "Gym", "Sports centre", 2},
    {"19:30", "Band practice", "The Basement", 3},
    {"21:00", "Call home", "", 2},
    {"09:00", "Embedded Systems", "Lab, Room 1120", 0},
    {"11:00", "Office hours", "Room 3012", 1},
    {"12:30", "Stand-up", "Online", 1},
    {"14:00", "Probability", "Lecture, Hall 1", 0},
    {"15:45", "Study group", "Library, 2nd floor", 3},
    {"17:15", "Dentist", "Main Street 12", 2},
    {"18:30", "Dinner", "At home", 2},
    {"20:00", "Film night", "Cinema", 3},
    {"08:30", "Networks", "Lecture, Hall 2", 0},
    {"10:15", "Networks", "Tutorial, Room 2042", 0},
    {"12:00", "Seminar", "Room 4001", 1},
    {"13:30", "Reading", "Library", 3},
    {"15:00", "Lab report due", "Online", 0},
    {"16:30", "Coffee with Alex", "Café", 2},
    {"18:00", "Groceries", "Market", 2},
    {"20:30", "Board games", "At Robin's", 3},
    {"09:30", "Databases", "Lecture, Hall 3", 0},
    {"11:15", "Databases", "Lab, Room 1120", 0},
    {"13:00", "Team lunch", "Canteen", 1},
    {"14:30", "Thesis meeting", "Room 5010", 1},
    {"16:00", "Run", "Park", 2},
    {"18:15", "Cooking class", "Community centre", 3},
    {"20:00", "Concert", "Music hall", 3},
    {"22:00", "Pack for the weekend", "", 2},
    {"08:45", "Operating Systems", "Lecture, Hall 2", 0},
    {"10:30", "Operating Systems", "Tutorial, Room 2042", 0},
    {"12:15", "Lunch", "Canteen", 2},
    {"13:45", "Sprint review", "Online", 1},
    {"15:30", "Statistics", "Lecture, Hall 4", 0},
    {"17:00", "Swim", "Pool", 2},
    {"19:00", "Choir", "Church hall", 3},
    {"21:00", "Plan the week", "", 2},
};

const char *const LOG[] = {
    "I (1204) boot: firmware 2.4.0, built 6 Oct 2026",
    "I (1388) wifi: connecting to Home",
    "I (2911) wifi: connected, -52 dBm, 192.168.1.40",
    "I (3020) ota: up to date",
    "I (3101) desk: link up, 74.0 cm",
    "I (3480) hass: connected, 31 entities",
    "W (4120) ical: feed 2 answered 503, trying again in 60 s",
    "I (4300) radar: 14 aircraft in range",
    "I (5012) media: Office speaker playing",
    "I (9001) focus: plan 25, 5, 15 min, 4 rounds",
    "W (12040) hass: sensor.office_co2 unavailable",
    "I (12988) hass: sensor.office_co2 back, 640 ppm",
    "I (20211) desk: moved to 112.0 cm",
    "I (24502) desk: moved to 74.0 cm",
    "E (31877) travel: no answer from the planner",
    "I (31990) travel: using the last plan, 9 min old",
    "I (40203) screen: dimmer on battery",
    "I (61004) ical: feed 2 back, 12 events",
    "I (90233) wifi: roamed, -48 dBm",
    "I (120440) heap: 212 KB internal, 28.1 MB PSRAM free",
};

void open_log(lv_obj_t *source)
{
    lv_obj_t *body = s_sheet = sheet(source, "Log", 840, 600);
    for (int round = 0; round < 3; ++round) {
        for (const char *line : LOG) {
            const Ink ink = line[0] == 'E' ? Ink::Red : line[0] == 'W' ? Ink::Amber : Ink::Secondary;
            lv_obj_t *t = text(body, Role::Small, line, ink);
            lv_obj_set_width(t, LV_PCT(100));
            lv_obj_set_style_pad_hor(t, space::m, 0);
            lv_obj_set_style_pad_ver(t, 5, 0);
            lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
        }
    }
}

void open_about(lv_obj_t *source)
{
    lv_obj_t *body = s_sheet = sheet(source, "About this panel", 640, 560);
    struct L { const char *label, *value; };
    for (const L &l : {L{"Firmware", "2.4.0"}, L{"Built", "6 Oct 2026, 19:26"}, L{"Uptime", "3 h 12 min"},
                       L{"Wi-Fi", "Home, -52 dBm"}, L{"Address", "192.168.1.40"}, L{"Desk", "Linked, 74.0 cm"},
                       L{"Battery", "86%, charging"}, L{"Screen", "1280 x 720, 80%"},
                       L{"Memory", "212 KB internal, 28.1 MB PSRAM"}, L{"Home Assistant", "Connected, 31 entities"},
                       L{"Calendar", "2 feeds, 52 events"}, L{"Radar", "14 aircraft"}, L{"LVGL", "9.6.0"},
                       L{"Touch", "GT911, 800 a second"}}) {
        sheet_row(body, l.label, l.value);
    }
}

void open_event(lv_obj_t *source)
{
    const auto  i  = static_cast<std::size_t>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(source)));
    const Event &ev = EVENTS[i];
    lv_obj_t *body = s_sheet = sheet(source, ev.title, 560, 330);
    const char *const names[] = {"Today", "Tomorrow", "Thursday", "Friday", "Monday"};
    char when[48];
    std::snprintf(when, sizeof when, "%s, %s, for 1 h 30 min", names[i / 8], ev.time);
    sheet_row(body, "When", when);
    sheet_row(body, "Where", ev.place[0] != '\0' ? ev.place : "Nowhere given");
    sheet_row(body, "Calendar", ev.feed == 0 ? "Timetable" : ev.feed == 1 ? "Work" : "Home");
}

void build_settings(lv_obj_t *parent)
{
    lv_obj_t *col = box(parent, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(col, COL_A);
    section(col, "PANEL");
    lv_obj_t *g = group(col, COL_A);
    RowSpec r;
    r.glyph = ic::desk, r.title = "Desk link", r.trail = Trail::Toggle, r.on = true;
    s_desk = row(g, r);
    r = {};
    r.glyph = ic::palette, r.tint = Ink::Amber, r.title = "Accent colour", r.trail = Trail::Chevron;
    r.value = ACCENT_NAMES[0], r.value_w = widest(Role::Body, {"Orange", "Blue", "Green", "Violet"});
    s_accent_row = row(g, r);
    lv_obj_add_event_cb(
        s_accent_row,
        [](lv_event_t *) {
            s_accent = (s_accent + 1) % static_cast<int>(std::size(ACCENTS));
            set_accent(ACCENTS[s_accent]);
            set_row_value(s_accent_row, ACCENT_NAMES[s_accent]);
        },
        LV_EVENT_CLICKED, nullptr);
    r = {};
    r.glyph = ic::wifi, r.tint = Ink::Green, r.title = "Wi-Fi", r.detail = "Home, -52 dBm", r.trail = Trail::Value;
    r.value = "Online", r.dot = true, r.ink = Ink::Green;
    row(g, r);
    r = {};
    r.glyph = ic::monitor_heart, r.tint = Ink::Red, r.title = "Log", r.trail = Trail::Badge, r.value = "3 warnings";
    r.ink = Ink::Amber, r.pressable = true;
    s_log = row(g, r);
    lv_obj_add_event_cb(s_log, [](lv_event_t *e) { open_log(lv_event_get_current_target_obj(e)); }, LV_EVENT_CLICKED,
                        nullptr);
    popover_source(s_log);
    r = {};
    r.glyph = ic::restart_alt, r.tint = Ink::Red, r.title = "Restart";
    r.action = true;
    set_usable(row(g, r), false);
    r = {};
    r.glyph = ic::info, r.tint = Ink::Secondary, r.title = "About this panel", r.trail = Trail::Chevron;
    s_about = row(g, r);
    lv_obj_add_event_cb(s_about, [](lv_event_t *e) { open_about(lv_event_get_current_target_obj(e)); },
                        LV_EVENT_CLICKED, nullptr);
    popover_source(s_about);

    section(col, "FOCUS");
    lv_obj_t *f = group(col, COL_A);
    r = {};
    r.title = "Focus";
    lv_obj_t *fr = row(f, r);
    StepperSpec ss;
    ss.min = 5, ss.max = 120, ss.step = 5, ss.value = 25, ss.unit = "min";
    own(fr, new Stepper(fr, ss));
}

void show_feed(bool working)
{
    const std::uint32_t n = lv_obj_get_child_count(s_cal);
    for (std::uint32_t i = 0; i < n; ++i) {
        lv_obj_t *c = lv_obj_get_child(s_cal, static_cast<std::int32_t>(i));
        lv_obj_set_hidden(c, c == s_cal_empty ? working : !working);
    }
    lv_obj_scroll_to_y(s_cal, 0, LV_ANIM_OFF);
}

void fill_calendar(lv_obj_t *col)
{
    const std::int32_t lead_w = widest(Role::Body, {"00:00"});
    for (std::size_t i = 0; i < std::size(EVENTS); ++i) {
        if (i % 8 == 0) {
            section(col, DAYS[i / 8][0], DAYS[i / 8][1]);
        }
        const Event &ev = EVENTS[i];
        RowSpec r;
        r.lead = ev.time, r.lead_w = lead_w, r.mark = FEEDS[ev.feed], r.title = ev.title;
        r.detail = ev.place[0] != '\0' ? ev.place : nullptr, r.pressable = true;
        lv_obj_t *e = row(col, r);
        lv_obj_set_user_data(e, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        lv_obj_add_event_cb(e, [](lv_event_t *ev2) { open_event(lv_event_get_current_target_obj(ev2)); },
                            LV_EVENT_CLICKED, nullptr);
        popover_source(e);
    }
}

void build_calendar(lv_obj_t *parent)
{
    lv_obj_t *col = box(parent, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_width(col, COL_B);
    section(col, "COMING UP", "40 EVENTS, 5 DAYS");
    s_cal = list(col, COL_B, SHEET_H - SECTION_H - 60 - space::m);
    s_cal_tile = lv_obj_get_parent(s_cal);
    fill_calendar(s_cal);
    s_cal_empty = empty(s_cal, ic::event, "The calendar did not answer. Its events show again once it does.");
    lv_obj_set_hidden(s_cal_empty, true);
    lv_obj_t *feed = segmented(col, {"Feed working", "Feed failing"}, 0, COL_B);
    lv_obj_set_style_margin_top(feed, space::m, 0);
    lv_obj_add_event_cb(
        feed, [](lv_event_t *e) { show_feed(segmented_chosen(lv_event_get_target_obj(e)) == 0); },
        LV_EVENT_VALUE_CHANGED, nullptr);
}

lv_obj_t *caption_tile(lv_obj_t *parent, const char *title)
{
    lv_obj_t *t = tile(parent, Lift::Low);
    lv_obj_set_size(t, COL_C, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, space::m, 0);
    text(t, Role::Label, title, Ink::Secondary);
    return t;
}

void build_readings(lv_obj_t *parent)
{
    lv_obj_t *col = box(parent, LV_FLEX_FLOW_COLUMN, space::l);
    lv_obj_set_width(col, COL_C);

    lv_obj_t *reads = caption_tile(col, "READOUTS, LIVE");
    lv_obj_t *pair  = box(reads, LV_FLEX_FLOW_ROW, space::xl);
    ReadoutSpec co2;
    co2.role = Role::ReadL, co2.unit = "PPM", co2.caption = "CO2, COUNTING", co2.min = 400, co2.max = 2000;
    s_co2 = new Readout(pair, co2);
    own(s_co2->obj(), s_co2);
    s_co2->set_value(640);
    ReadoutSpec temp;
    temp.role = Role::ReadL, temp.unit = "°C", temp.caption = "ROOM", temp.min = 0, temp.max = 399, temp.decimals = 1;
    s_temp = new Readout(pair, temp);
    own(s_temp->obj(), s_temp);
    s_temp->set_value(215);
    lv_obj_t *set = box(reads, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_width(set, LV_PCT(100));
    lv_obj_set_flex_align(set, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(set, Role::Body, "Target");
    StepperSpec ss;
    ss.min = 150, ss.max = 250, ss.step = 5, ss.value = 210, ss.decimals = 1, ss.unit = "°C";
    s_target = new Stepper(set, ss);
    own(s_target->obj(), s_target);

    lv_obj_t *badges = caption_tile(col, "BADGES, IN EVERY INK");
    lv_obj_t *wrap   = box(badges, LV_FLEX_FLOW_ROW_WRAP, space::s);
    lv_obj_set_width(wrap, LV_PCT(100));
    badge(wrap, "Live", Ink::OnAccent);
    badge(wrap, "Heating", Ink::Accent, nullptr, 0, true);
    badge(wrap, "Good", Ink::Green, ic::check_circle);
    badge(wrap, "Fair", Ink::Amber, ic::warning);
    badge(wrap, "Poor", Ink::Red, ic::error);
    badge(wrap, "Paused", Ink::Secondary);
    badge(wrap, "Off", Ink::Tertiary, nullptr, 0, true);
    s_live = badge(wrap, "In 45 min", Ink::Secondary, nullptr, widest(Role::Small, {"In 45 min", "In 5 min", "Now"}) + 24);

    lv_obj_t *sheets = tile(col, Lift::Low);
    lv_obj_set_size(sheets, COL_C, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sheets, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(sheets, space::m, 0);
    lv_obj_t *b = button(sheets, Button::Raised, ic::monitor_heart, "The log");
    lv_obj_add_event_cb(b, [](lv_event_t *e) { open_log(lv_event_get_current_target_obj(e)); }, LV_EVENT_CLICKED, nullptr);
    popover_source(b);
    lv_obj_t *a = button(sheets, Button::Raised, ic::info, "Details");
    lv_obj_add_event_cb(a, [](lv_event_t *e) { open_about(lv_event_get_current_target_obj(e)); }, LV_EVENT_CLICKED,
                        nullptr);
    popover_source(a);

    lv_timer_create(
        [](lv_timer_t *) {
            if (s_root == nullptr || lv_obj_is_hidden(s_root)) {
                return;
            }
            static int tick = 0;
            ++tick;
            s_co2->set_value(450 + std::rand() % 1400, true);
            s_temp->set_value(195 + std::rand() % 40);
            const char *const soon[] = {"In 45 min", "In 5 min", "Now"};
            set_badge(s_live, soon[tick % 3], tick % 3 == 0 ? Ink::Secondary : tick % 3 == 1 ? Ink::Amber : Ink::Accent);
        },
        2200, nullptr);
}

// --- the study -------------------------------------------------------------

// The same forty-event list built each way a page might, in the calendar's
// place, then scrolled through and timed.
enum Variant : std::uint8_t {
    KIT, TILE_SCROLLS, ROWS_OPAQUE, ROW_SHADOWS, HAIR_OBJECTS, FADE, ON_GLASS, IN_WELL_ON_GLASS, WHOLE_PAGE,
    SHEET_FLAT, SHEET, VARIANTS
};
const char *const VARIANT_NAMES[] = {
    "kit list: bare rows, inset scroller",
    "the rounded tile scrolls itself",
    "every row opaque",
    "every row its own shadow",
    "hairlines as objects",
    "a gradient fade over the foot",
    "rows straight on glass",
    "rows on a flat area on glass",
    "the whole page scrolls instead",
    "a sheet, its body on its glass",
    "kit sheet: a picture of its glass under",
};

constexpr int ROUNDS = 3;  // each variant in turn, three times over
constexpr int STUDY_STEPS = 60;

struct Study {
    int           variant = -1, round = 0;
    std::uint32_t began = 0;
    lv_obj_t     *holder = nullptr, *scroller = nullptr;
    float        *each = nullptr;  // [VARIANTS][ROUNDS * STUDY_STEPS], ms
    std::uint64_t px[VARIANTS]{};
    std::uint32_t frames[VARIANTS]{};
    bool          counting = false;
} s_study;
char s_report[1600] = "";
std::uint64_t s_flush_from = 0, s_flushing_us = 0;

// What a frame spends handing pixels over, flushing or waiting, is left out:
// in the simulator it is mostly waiting for the screen's refresh.
void on_flush(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FLUSH_START || code == LV_EVENT_FLUSH_WAIT_START) {
        s_flush_from = perf::now_us();
    } else if (s_flush_from != 0) {
        s_flushing_us += perf::now_us() - s_flush_from;
        s_flush_from = 0;
    }
}

void on_render(lv_event_t *e)
{
    if (!s_study.counting) {
        return;
    }
    const auto *disp = static_cast<const lv_display_t *>(lv_event_get_current_target(e));
    for (std::uint32_t i = 0; i < disp->inv_p; ++i) {
        if (!disp->inv_area_joined[i]) {
            s_study.px[s_study.variant] += lv_area_get_size(&disp->inv_areas[i]);
        }
    }
    ++s_study.frames[s_study.variant];
}

lv_obj_t *variant_row(lv_obj_t *col, std::size_t i, int v)
{
    RowSpec r;
    r.lead = EVENTS[i].time, r.lead_w = widest(Role::Body, {"00:00"}), r.mark = FEEDS[EVENTS[i].feed];
    r.title = EVENTS[i].title, r.detail = EVENTS[i].place[0] != '\0' ? EVENTS[i].place : nullptr, r.pressable = true;
    lv_obj_t *row_obj = row(col, r);
    if (v == ROWS_OPAQUE) {
        lv_obj_set_style_bg_color(row_obj, lv_color_hex(colours().surface), 0);
        lv_obj_set_style_bg_opa(row_obj, LV_OPA_COVER, 0);
    } else if (v == ROW_SHADOWS) {
        lv_obj_set_style_shadow_width(row_obj, 16, 0);
        lv_obj_set_style_shadow_offset_y(row_obj, 4, 0);
        lv_obj_set_style_shadow_opa(row_obj, 60, 0);
        lv_obj_set_style_shadow_color(row_obj, lv_color_hex(colours().shadow), 0);
    }
    return row_obj;
}

void fill_variant(lv_obj_t *col, int v)
{
    for (std::size_t i = 0; i < std::size(EVENTS); ++i) {
        if (i % 8 == 0) {
            section(col, DAYS[i / 8][0], DAYS[i / 8][1]);
        }
        if (v == HAIR_OBJECTS && i % 8 != 0) {
            lv_obj_t *h = hairline(col, false, COL_B - 2 * space::s - 80);
            lv_obj_set_style_margin_left(h, 80, 0);
        }
        lv_obj_t *r = variant_row(col, i, v);
        if (v == HAIR_OBJECTS) {
            lv_obj_set_style_pad_top(r, 9, 0);  // the drawn line is a pixel of its own row
        }
    }
}

lv_obj_t *scroller_in(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    return col;
}

void build_variant(int v)
{
    if (v == SHEET) {
        s_study.scroller = sheet(s_log, "Log", 840, 600);
        fill_variant(s_study.scroller, v);
        return;
    }
    if (v == SHEET_FLAT) {
        lv_obj_t *body = popover(s_log, 840, 600, From::Centre);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(body, space::l, 0);
        text(body, Role::H2, "Log");
        lv_obj_t *flat = well(body);
        lv_obj_set_width(flat, LV_PCT(100));
        lv_obj_set_flex_grow(flat, 1);
        lv_obj_set_style_pad_all(flat, space::s, 0);
        s_study.scroller = scroller_in(flat);
        fill_variant(s_study.scroller, v);
        return;
    }
    lv_area_t at;
    lv_obj_get_coords(s_cal_tile, &at);
    const std::int32_t w = lv_area_get_width(&at), h = lv_area_get_height(&at);
    lv_obj_t *content = layer(Layer::Content);
    lv_obj_set_hidden(s_cal_tile, true);
    s_study.holder = box(content, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_ignore_layout(s_study.holder, true);
    lv_obj_set_pos(s_study.holder, at.x1, at.y1);
    lv_obj_set_size(s_study.holder, w, h);

    switch (v) {
    case TILE_SCROLLS: {
        lv_obj_t *t = tile(s_study.holder, Lift::Low);
        lv_obj_set_size(t, w, h);
        lv_obj_set_style_pad_all(t, space::s, 0);
        lv_obj_set_scrollable(t, true);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(t, LV_SCROLLBAR_MODE_OFF);
        s_study.scroller = t;
        break;
    }
    case ON_GLASS:
    case IN_WELL_ON_GLASS: {
        lv_obj_t *g = glass(s_study.holder, Glass::Large);
        lv_obj_set_style_radius(g, radius::pop, 0);
        lv_obj_set_size(g, w, h);
        lv_obj_set_style_pad_all(g, space::s, 0);
        lv_obj_t *in = g;
        if (v == IN_WELL_ON_GLASS) {
            in = well(g);
            lv_obj_set_size(in, LV_PCT(100), LV_PCT(100));
            lv_obj_set_style_pad_all(in, space::s, 0);
        }
        s_study.scroller = scroller_in(in);
        break;
    }
    case WHOLE_PAGE: {
        // a tile as tall as its rows, and the content plane scrolling it under the chrome
        lv_obj_t *t = tile(s_study.holder, Lift::Low);
        lv_obj_set_size(t, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(t, space::s, 0);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_height(s_study.holder, LV_SIZE_CONTENT);
        s_study.scroller = content;
        fill_variant(t, v);
        return;
    }
    default: {
        lv_obj_t *col = list(s_study.holder, w, h);
        lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
        s_study.scroller = col;
        if (v == FADE) {
            lv_obj_t *t    = lv_obj_get_parent(col);
            lv_obj_t *fade = lv_obj_create(t);
            lv_obj_remove_style_all(fade);
            lv_obj_set_ignore_layout(fade, true);
            lv_obj_set_clickable(fade, false);
            lv_obj_set_size(fade, w - 2 * space::s, 40);
            lv_obj_align(fade, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_style_bg_color(fade, lv_color_hex(colours().surface), 0);
            lv_obj_set_style_bg_opa(fade, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_main_opa(fade, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_grad_color(fade, lv_color_hex(colours().surface), 0);
            lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
        }
        break;
    }
    }
    fill_variant(s_study.scroller, v);
}

void drop_variant()
{
    if (popover_open()) {
        close_popover(false);
    }
    if (s_study.holder != nullptr) {
        lv_obj_delete(s_study.holder);
        s_study.holder = s_study.scroller = nullptr;
    }
    lv_obj_scroll_to_y(layer(Layer::Content), 0, LV_ANIM_OFF);
    lv_obj_set_hidden(s_cal_tile, false);
}

constexpr std::uint32_t STUDY_SETTLE_MS = 500;  // for the tile's shadow to be baked in

// Drawn here frame by frame, so nothing else the lab does lands between them,
// each timed for its drawing alone.
void scroll_through(lv_obj_t *sc, float *each)
{
    const std::int32_t span = std::min<std::int32_t>(lv_obj_get_scroll_bottom(sc) + lv_obj_get_scroll_y(sc), 900);
    for (int k = 1; k <= STUDY_STEPS; ++k) {
        const int t = k <= STUDY_STEPS / 2 ? k : STUDY_STEPS - k;
        lv_obj_scroll_to_y(sc, span * t * 2 / STUDY_STEPS, LV_ANIM_OFF);
        s_flushing_us = 0;
        const std::uint64_t t0 = perf::now_us();
        lv_refr_now(nullptr);
        each[k - 1] = static_cast<float>(perf::now_us() - t0 - s_flushing_us) / 1000.0f;
    }
}

void study_step(lv_timer_t *timer)
{
    if (lv_tick_elaps(s_study.began) < (s_study.variant >= SHEET_FLAT ? 1200u : STUDY_SETTLE_MS)) {
        return;
    }
    s_study.counting = true;
    scroll_through(s_study.scroller, s_study.each + (s_study.variant * ROUNDS + s_study.round) * STUDY_STEPS);
    s_study.counting = false;
    drop_variant();
    if (s_study.variant + 1 == VARIANTS && ++s_study.round < ROUNDS) {
        s_study.variant = -1;
    }
    if (++s_study.variant < VARIANTS) {
        build_variant(s_study.variant);
        s_study.began = lv_tick_get();
        return;
    }
    lv_timer_delete(timer);
    int n = std::snprintf(s_report, sizeof s_report,
                          "list study, %s, 40 events and 5 days, %d frames of scrolling each, %d times, drawing alone\n",
                          look() == Look::Dark ? "dark" : "light", STUDY_STEPS, ROUNDS);
    // the middle frame and the one nine tenths of the way up: what a frame
    // costs, and what the slow ones do, without the odd stall
    constexpr int N = ROUNDS * STUDY_STEPS;
    float mid[VARIANTS], high[VARIANTS];
    for (int i = 0; i < VARIANTS; ++i) {
        float *f = s_study.each + i * N;
        std::sort(f, f + N);
        mid[i]  = f[N / 2];
        high[i] = f[N * 9 / 10];
    }
    for (int i = 0; i < VARIANTS && n < static_cast<int>(sizeof s_report); ++i) {
        n += std::snprintf(s_report + n, sizeof s_report - n,
                           "  %-40s middle %6.2f ms  x%5.2f  p90 %6.2f ms  %4u kpx a frame\n", VARIANT_NAMES[i],
                           static_cast<double>(mid[i]), static_cast<double>(mid[i] / std::max(mid[KIT], 0.001f)),
                           static_cast<double>(high[i]),
                           static_cast<unsigned>(s_study.frames[i] ? s_study.px[i] / s_study.frames[i] / 1000 : 0));
    }
    std::printf("%s", s_report);
    lv_free(s_study.each);
    s_study.each    = nullptr;
    s_study.variant = -1;
}

// --- the bench -------------------------------------------------------------

std::int32_t triangle(lv_obj_t *sc, std::uint32_t at, std::uint32_t ms)
{
    const std::int32_t span = lv_obj_get_scroll_bottom(sc) + lv_obj_get_scroll_y(sc);
    const auto         half = static_cast<std::int32_t>(ms / 2), t = static_cast<std::int32_t>(at);
    return std::min<std::int32_t>(span, 900) * (t < half ? t : 2 * half - t) / half;
}

}  // namespace

void build(lv_obj_t *sheet_box)
{
    s_root = sheet_box;
    lv_obj_set_flex_align(sheet_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    build_settings(sheet_box);
    build_calendar(sheet_box);
    build_readings(sheet_box);
}

void scroll_tick(std::uint32_t at, std::uint32_t)
{
    lv_obj_scroll_to_y(s_cal, triangle(s_cal, at, 1600), LV_ANIM_OFF);
}

void press_tick(std::uint32_t at, std::uint32_t)
{
    if (at % 200 >= 16) {
        return;
    }
    if (lv_obj_has_state(s_about, LV_STATE_PRESSED)) {
        lv_obj_remove_state(s_about, LV_STATE_PRESSED);
    } else {
        lv_obj_add_state(s_about, LV_STATE_PRESSED);
        lv_obj_send_event(s_about, LV_EVENT_PRESSED, nullptr);
    }
}

void readouts_tick(std::uint32_t at, std::uint32_t)
{
    if (at % 500 < 16) {
        s_co2->set_value(450 + std::rand() % 1400, true);
        s_temp->set_value(195 + std::rand() % 40);
        s_target->set_value(150 + std::rand() % 21 * 5);
    }
}

void sheet_open_tick(std::uint32_t at, std::uint32_t frame)
{
    if (frame == 0) {
        prepare_popover();
    } else if (at >= 120 && at < 136 && !popover_open()) {
        open_log(s_log);
    }
}

void sheet_scroll_tick(std::uint32_t at, std::uint32_t)
{
    if (s_sheet != nullptr && popover_open()) {
        lv_obj_scroll_to_y(s_sheet, triangle(s_sheet, at, 1400), LV_ANIM_OFF);
    }
}

void sheet_close_tick(std::uint32_t, std::uint32_t frame)
{
    if (frame == 0) {
        close_popover(true);
        s_sheet = nullptr;
    }
}

void study()
{
    if (s_study.variant >= 0 || s_cal_tile == nullptr) {
        return;
    }
    static bool watching = false;
    if (!watching) {
        watching = true;
        lv_display_t *disp = lv_display_get_default();
        lv_display_add_event_cb(disp, on_render, LV_EVENT_RENDER_START, nullptr);
        for (lv_event_code_t code : {LV_EVENT_FLUSH_START, LV_EVENT_FLUSH_FINISH, LV_EVENT_FLUSH_WAIT_START,
                                     LV_EVENT_FLUSH_WAIT_FINISH}) {
            lv_display_add_event_cb(disp, on_flush, code, nullptr);
        }
    }
    s_study = Study{};
    s_study.each = static_cast<float *>(lv_malloc(sizeof(float) * VARIANTS * ROUNDS * STUDY_STEPS));
    if (s_study.each == nullptr) {
        return;
    }
    s_study.variant = 0;
    s_study.began   = lv_tick_get();
    build_variant(0);
    lv_timer_create(study_step, 16, nullptr);
}

void show_press()
{
    for (lv_obj_t *r : {s_desk, s_about}) {
        lv_obj_set_state(r, LV_STATE_USER_1, !lv_obj_has_state(r, LV_STATE_USER_1));  // as while its sheet is open
    }
}

void nudge()
{
    lv_obj_t *sc = popover_open() && s_sheet != nullptr ? s_sheet : s_cal;
    lv_obj_scroll_by(sc, 0, -240, LV_ANIM_ON);
}

const char *report() { return s_report; }

}  // namespace ui::lab::lists
