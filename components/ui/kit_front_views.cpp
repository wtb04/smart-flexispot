// The fullscreen views, music, cinema and the focus timer, and what they all
// have over them; the favourites; and the notices. The views themselves are in
// kit_front_views_media.cpp and kit_front_views_focus.cpp.

#include "kit_front_views.h"

#include "focus_model.h"
#include "kit_fonts.h"
#include "media.h"
#include "media_model.h"
#include "notices_model.h"
#include "room_model.h"
#include "status_model.h"
#include "topics.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace ui::kitfront::views {
namespace {
namespace ic = kit::glyph;
using namespace kit;

constexpr std::int32_t EDGE = 32, TOP = 24, ROW_H = size::button;

std::intptr_t id_of(lv_event_t *e) { return reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)); }
void         *as_data(std::intptr_t v) { return reinterpret_cast<void *>(v); }

// The preset the shortcut goes to: sitting while standing, standing otherwise.
int shortcut_target()
{
    return detail::desk_state().preset_active[detail::STAND_PRESET] ? detail::SIT_PRESET : detail::STAND_PRESET;
}

struct Over : InPsram {
    lv_obj_t *shortcut = nullptr, *arrow = nullptr;
    lv_obj_t *right = nullptr, *power = nullptr, *cell = nullptr, *percent = nullptr, *clock = nullptr;
    lv_obj_t *badge = nullptr;
};

void paint_desk(const Over &o)
{
    const detail::DeskState &desk = detail::desk_state();
    set_icon(o.arrow,
             desk.moving > 0                                 ? ic::arrow_upward
             : desk.moving < 0                               ? ic::arrow_downward
             : shortcut_target() == detail::STAND_PRESET     ? ic::arrow_upward
                                                             : ic::arrow_downward,
             false);
    set_usable(o.shortcut, desk.available);
}

void paint_status(const Over &o)
{
    const detail::StatusState &st = detail::status_state();
    set_text_if(o.clock, detail::pinned_clock() != nullptr ? detail::pinned_clock() : st.time);
    lv_obj_set_hidden(o.power, !st.on_battery);
    if (st.on_battery) {
        const int pct = std::clamp(st.battery_percent, 0, 100);
        set_icon(o.cell, pct > 80 ? ic::battery_full : pct > 20 ? ic::battery_5_bar : ic::battery_2_bar, false);
        set_ink(o.cell, pct <= 20 ? Ink::Amber : Ink::Primary);
        char buf[8];
        std::snprintf(buf, sizeof buf, "%d%%", pct);
        set_text_if(o.percent, buf);
    }
    if (o.badge != nullptr) {
        // left of what stays put while the rest fades, so it never moves as they do
        lv_obj_update_layout(o.right);
        lv_obj_align_to(o.badge, o.right, LV_ALIGN_OUT_LEFT_MID, -space::m, 0);
    }
}

void paint_focus(const Over &o)
{
    const Focus &focus = detail::focus_state();
    const bool   idle  = detail::focus_idle(focus);
    lv_obj_set_hidden(o.badge, idle);
    if (idle) {
        return;
    }
    char buf[16];
    detail::focus_clock_text(focus, buf, sizeof buf);
    set_badge(o.badge, buf,
              detail::focus_paused(focus) ? Ink::Secondary : detail::focus_resting(focus) ? Ink::Green : Ink::Accent);
}

lv_obj_t *row_of(lv_obj_t *parent)
{
    lv_obj_t *row = flow(parent, LV_FLEX_FLOW_ROW, space::s);
    lv_obj_set_height(row, ROW_H);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}
}  // namespace

void set_text_if(lv_obj_t *label, const char *value)
{
    if (std::strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}

void write_clock(lv_obj_t *label, int seconds)
{
    seconds = std::max(seconds, 0);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", seconds / 60, seconds % 60);
    set_text_if(label, buf);
}

lv_obj_t *figures(lv_obj_t *parent, const char *widest_text, lv_text_align_t align)
{
    lv_obj_t *label = text(parent, Role::Small, "", Ink::Secondary);
    lv_obj_set_width(label, widest(Role::Small, {widest_text}));
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

lv_obj_t *fullscreen_root()
{
    lv_obj_t *root = lv_obj_create(layer(Layer::Chrome));
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, size::screen_w, size::screen_h);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);  // what is under it takes no taps
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    const auto paint = [](void *obj) { lv_obj_set_style_bg_color(static_cast<lv_obj_t *>(obj), lv_color_hex(colours().ground), 0); };
    paint(root);
    on_restyle(root, paint, root);
    return root;
}

Chrome chrome(detail::ViewId view, lv_obj_t *root, bool focus_badge)
{
    auto *o = new Over;

    lv_obj_t *left = row_of(root);
    lv_obj_set_pos(left, EDGE, TOP);
    o->shortcut = button(left, Button::Flat, nullptr, nullptr);
    o->arrow    = icon(o->shortcut, ic::arrow_upward, 24);
    icon(o->shortcut, ic::desk, 28);
    lv_obj_add_event_cb(o->shortcut, [](lv_event_t *) { detail::desk_go_to(shortcut_target()); }, LV_EVENT_CLICKED,
                        nullptr);

    o->right = row_of(root);
    lv_obj_set_style_pad_column(o->right, space::m, 0);
    lv_obj_align(o->right, LV_ALIGN_TOP_RIGHT, -EDGE, TOP);
    o->power = flow(o->right, LV_FLEX_FLOW_ROW, space::xs);
    lv_obj_set_flex_align(o->power, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    o->cell    = icon(o->power, ic::battery_full, 24);
    o->percent = figures(o->power, "100%", LV_TEXT_ALIGN_LEFT);
    lv_obj_set_hidden(o->power, true);
    o->clock = text(o->right, Role::H3, "");
    lv_obj_set_width(o->clock, widest(Role::H3, {"00:00"}));
    lv_obj_set_style_text_align(o->clock, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *close = button(o->right, Button::Flat, ic::keyboard_arrow_down, nullptr, true);
    lv_obj_set_size(close, size::button, size::button);
    lv_obj_add_event_cb(close, [](lv_event_t *e) { detail::close_view(static_cast<detail::ViewId>(id_of(e))); },
                        LV_EVENT_CLICKED, as_data(view));

    if (focus_badge) {
        const bool digits_wider = widest(Role::Small, {"00:00"}) >= widest(Role::Small, {"Ready"});
        o->badge = badge(root, digits_wider ? "00:00" : "Ready", Ink::Accent, ic::timer);
        lv_obj_add_flag(o->badge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(o->badge, space::m);
        lv_obj_add_event_cb(o->badge, [](lv_event_t *) { open_focus_view(); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_set_hidden(o->badge, true);
        detail::subscribe(Topic::Focus, view, [o] { paint_focus(*o); });
        detail::subscribe(Topic::Second, view, [o] { paint_focus(*o); });
    }
    detail::fade_when_idle(view, left);
    detail::fade_when_idle(view, o->right);
    detail::subscribe(Topic::Desk, view, [o] { paint_desk(*o); });
    detail::subscribe(Topic::Status, view, [o] { paint_status(*o); });
    detail::subscribe(Topic::Second, view, [o] {
        if (detail::pinned_clock() == nullptr) {
            set_text_if(o->clock, detail::status_state().time);
        }
    });
    return {left};
}

lv_obj_t *quick_action(const Chrome &chrome, const char *glyph, const char *label)
{
    return button(chrome.left, Button::Flat, glyph, label);
}

// Lit quietly, its glyph filled in the accent, as what it does reports back
// rather than as it is tapped: a filled button would shout over the picture.
void light(lv_obj_t *b, bool on)
{
    lv_obj_t *glyph = lv_obj_get_child(b, 0);
    set_icon(glyph, lv_label_get_text(glyph), on);
    if (lv_obj_has_state(b, LV_STATE_CHECKED) != on) {
        lv_obj_set_state(b, LV_STATE_CHECKED, on);
        set_ink(glyph, on ? Ink::Accent : Ink::Primary);
    }
}
}  // namespace ui::kitfront::views

// --- the favourites --------------------------------------------------------

namespace ui::kitfront {
namespace {
using namespace kit;

constexpr int          COLUMNS = 4;
constexpr std::int32_t ART = media::kPickArtSize, GAP = space::l, NAME_GAP = space::s;

struct PickTile {
    Cover        *cover = nullptr;
    std::uint32_t arts  = 0;
};
PickTile  s_tiles[media::kPickCount];
lv_obj_t *s_picks = nullptr;  // the sheet's body while it is open

void show_pick_art(int i, bool animate)
{
    PickTile           &tile = s_tiles[i];
    const detail::Pick &pick = detail::media_pick(i);
    if (tile.cover == nullptr || tile.arts == pick.arts) {
        return;
    }
    tile.arts = pick.arts;
    if (pick.art != nullptr) {
        tile.cover->show(Picture{pick.art, ART, ART, 0}, animate);
    } else {
        tile.cover->clear(animate);
    }
}

void pick_tapped(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    close_popover(true);
    if (detail::s_handlers.pick != nullptr) {
        detail::s_handlers.pick(index);
    }
}

void build_pick(lv_obj_t *grid, int i)
{
    lv_obj_t *b = button(grid, Button::Flat, nullptr, nullptr);
    lv_obj_set_size(b, ART, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_pad_row(b, NAME_GAP, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_STATE_PRESSED);  // it gives under the finger, without a fill round it
    lv_obj_add_event_cb(b, pick_tapped, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));

    auto *cover = new Cover(b, ART, ART, Shape::Rounded, radius::inner);
    s_tiles[i]  = {cover, UINT32_MAX};
    lv_obj_add_event_cb(
        cover->obj(),
        [](lv_event_t *e) {
            auto *gone = static_cast<Cover *>(lv_event_get_user_data(e));
            for (PickTile &tile : s_tiles) {
                if (tile.cover == gone) {
                    tile.cover = nullptr;
                }
            }
            delete gone;
        },
        LV_EVENT_DELETE, cover);
    lv_obj_t *name = text(b, Role::Small, detail::media_pick(i).name);
    lv_obj_set_width(name, ART);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    show_pick_art(i, false);
}

// Without a source, what the finger is on, or the middle of the screen.
lv_obj_t *source_or_finger(lv_obj_t *source)
{
    if (source == nullptr && lv_indev_active() != nullptr) {
        source = lv_indev_get_active_obj();
    }
    static lv_obj_t *middle = nullptr;
    if (source == nullptr) {
        if (middle == nullptr) {
            middle = lv_obj_create(layer(Layer::Overlay));
            lv_obj_remove_style_all(middle);
            lv_obj_set_size(middle, 1, 1);
            lv_obj_center(middle);
            lv_obj_remove_flag(middle, LV_OBJ_FLAG_CLICKABLE);
        }
        source = middle;
    }
    return source;
}
}  // namespace

void open_favourites(lv_obj_t *source)
{
    const int count = detail::media_pick_count();
    if (count == 0) {
        return;
    }
    const int          columns = std::min(count, COLUMNS);
    const int          rows    = (count + COLUMNS - 1) / COLUMNS;
    const std::int32_t name_h  = lv_font_get_line_height(fonts::small());
    // the sheet's padding, its head, the well round the grid and its scrolling column's foot
    const std::int32_t w = std::max<std::int32_t>(columns * ART + (columns - 1) * GAP + 2 * (space::xxl + space::s), 400);
    const std::int32_t h = rows * (ART + NAME_GAP + name_h) + (rows - 1) * GAP + 2 * (space::xxl + space::s) +
                           size::button + space::l + space::s;
    s_picks = sheet(source_or_finger(source), "Favourites", w, std::min<std::int32_t>(h, size::screen_h - 2 * space::l));
    lv_obj_set_flex_flow(s_picks, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_picks, GAP, 0);
    lv_obj_set_style_pad_row(s_picks, GAP, 0);
    lv_obj_add_event_cb(s_picks, [](lv_event_t *) { s_picks = nullptr; }, LV_EVENT_DELETE, nullptr);
    for (int i = 0; i < media::kPickCount; ++i) {
        if (detail::media_pick(i).name[0] != '\0') {
            build_pick(s_picks, i);
        }
    }
}

void open_favourites() { open_favourites(nullptr); }

void open_now_playing()
{
    const detail::MediaState &media = detail::media_state();
    if (!media.has_track) {
        return;
    }
    close_popover(false);
    if (detail::media_is_video()) {
        views::open_cinema();
    } else {
        views::open_music();
    }
}

void open_now_playing(lv_obj_t *) { open_now_playing(); }
}  // namespace ui::kitfront

// --- the notices -------------------------------------------------------------

namespace ui::kitfront {
namespace {
using namespace kit;

constexpr int    DEFAULT_MS = 8000;
constexpr time_t CLOCK_SET  = 1'700'000'000;  // any earlier and the clock is not set yet

lv_obj_t *s_notice    = nullptr;
bool      s_on_show   = false;
char      s_came[8]   = "";  // when the one on show came

Ink ink_of(Level level)
{
    switch (level) {
        case Level::Bad:  return Ink::Red;
        case Level::Warn: return Ink::Amber;
        case Level::Good: return Ink::Green;
        default:          return Ink::Accent;
    }
}

void aside(char *out, std::size_t size)
{
    const int waiting = detail::notices_state().count;
    if (waiting > 0) {
        std::snprintf(out, size, "%d MORE%s%s", waiting, s_came[0] != '\0' ? ", " : "", s_came);
    } else {
        std::snprintf(out, size, "%s", s_came);
    }
}

void show_next_notice();
void notice_done(lv_obj_t *) { show_next_notice(); }

void show_next_notice()
{
    detail::Notice notice{};
    if (!detail::notices_take_next(notice)) {
        s_notice  = nullptr;
        s_on_show = false;
        dismiss_notice();
        return;
    }
    char source[sizeof notice.source];
    const char *from = notice.source[0] != '\0' ? notice.source : "Notice";
    std::size_t i    = 0;
    for (; from[i] != '\0' && i + 1 < sizeof source; ++i) {
        source[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(from[i])));
    }
    source[i] = '\0';

    const time_t now = detail::wall_now();
    s_came[0]        = '\0';
    if (now >= CLOCK_SET) {
        std::tm local{};
        localtime_r(&now, &local);
        std::strftime(s_came, sizeof s_came, "%H:%M", &local);
    }
    char side[32];
    aside(side, sizeof side);

    // the title is the headline; without one, the message is
    const bool titled = notice.title[0] != '\0';
    NoticeSpec spec;
    spec.source     = source;
    spec.title      = titled ? notice.title : notice.message;
    spec.message    = titled ? notice.message : nullptr;
    spec.aside      = side;
    spec.ink        = ink_of(notice.level);
    spec.timeout_ms = notice.timeout_ms < 0 ? 0 : notice.timeout_ms > 0 ? notice.timeout_ms : DEFAULT_MS;
    spec.done       = notice_done;
    spec.solid      = detail::fullscreen_open();
    s_on_show       = true;
    s_notice        = kit::notice(spec);
}

// The next as it comes, once the start-up is out of the way; while one shows,
// how many wait behind it.
void follow_notices()
{
    if (!detail::status_state().splash_gone) {
        return;
    }
    if (s_on_show) {
        char side[32];
        aside(side, sizeof side);
        set_notice_aside(s_notice, side);
    } else if (detail::notices_state().count > 0) {
        show_next_notice();
    }
}
}  // namespace

void build_views(lv_obj_t *screen)
{
    views::build_media(screen);
    views::build_focus(screen);
    subscribe(Topic::Picks, detail::kNoView, [] {
        for (int i = 0; s_picks != nullptr && i < media::kPickCount; ++i) {
            show_pick_art(i, true);
        }
    });
    subscribe(Topic::Notices, detail::kNoView, follow_notices);
    subscribe(Topic::Status, detail::kNoView, follow_notices);
}
}  // namespace ui::kitfront

namespace ui::detail {
bool notice_on_show() { return kitfront::s_on_show; }
}  // namespace ui::detail
