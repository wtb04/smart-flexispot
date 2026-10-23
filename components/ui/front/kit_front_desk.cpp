// The desk, folded out of the dock's height: up and down for as long as they
// are held, every preset with its height, held to store, and a Stop while it moves.

#include "kit_front.h"

#include "kit_fonts.h"
#include "room_model.h"
#include "settings_model.h"

#include <cstdio>
#include <cstring>

namespace ui::kitfront {
namespace {
namespace ic = kit::glyph;
using namespace kit;
using detail::desk_state;
using detail::DeskState;

// Stand and Sit first, as the dock has them as one button; 5 and 6 are for
// whoever uses the desk while the owner's phone is away.
constexpr int PRESETS[] = {detail::STAND_PRESET, detail::SIT_PRESET, detail::ULTRA_LOW_PRESET, 0, 4, 5};
constexpr int GUESTS[]  = {4, 5};

constexpr std::int32_t COL_W = 176, MOVE_H = 80, PRESET_H = 120, GAP = space::m, TILE_R = 20, FOOT_H = 72;
constexpr std::int32_t INNER  = 2 * COL_W + GAP;
constexpr std::int32_t CARD_W = INNER + 2 * space::xxl;
constexpr std::uint32_t IDLE_MS = 20 * 1000;

enum class Tile : std::uint8_t { Plain, At, Going };

struct Card : InPsram {
    lv_obj_t   *where = nullptr;
    lv_obj_t   *moves[2]{};
    lv_obj_t   *tiles[kPresetCount]{}, *heights[kPresetCount]{}, *units[kPresetCount]{}, *arrows[kPresetCount]{};
    Tile        shown[kPresetCount]{};
    lv_obj_t   *hint = nullptr, *stop = nullptr;
    bool        guests = false;
    Move        held   = Move::Stop;
    lv_timer_t *idle   = nullptr;
};

Card *s_card = nullptr;

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    return obj;
}

// A preset's height and its unit on one baseline.
std::int32_t unit_y()
{
    const lv_font_t *big = fonts::read_m(), *small = fonts::small();
    return (big->line_height - big->base_line) - (small->line_height - small->base_line);
}

bool guests_shown()
{
    return owner_away() || !detail::settings_state().on[static_cast<int>(Setting::PresenceGate)];
}

bool is_guest(int index) { return index == GUESTS[0] || index == GUESTS[1]; }

void set_text(lv_obj_t *label, const char *value)
{
    kit::set_words(label, value);
}

// What the desk does, beside the card's name: where it is, or where it goes.
void say_where(char *out, std::size_t size)
{
    const DeskState &desk = desk_state();
    if (!desk.available) {
        std::snprintf(out, size, "Not answering");
    } else if (desk.travelling >= 0) {
        std::snprintf(out, size, "Going to %s", preset_name(desk.travelling));
    } else if (desk.moving != 0) {
        std::snprintf(out, size, "%s", desk.moving > 0 ? "Moving up" : "Moving down");
    } else {
        out[0] = '\0';
        for (const int index : PRESETS) {
            if (desk.preset_active[index]) {
                std::snprintf(out, size, "At %s", preset_name(index));
                break;
            }
        }
    }
}

bool under_way()
{
    const DeskState &desk = desk_state();
    return desk.available && (desk.travelling >= 0 || desk.moving != 0);
}

// The one it stands at filled with the accent, the one it goes to ringed in
// it with the way it goes, the rest plain; each with the height it holds.
void paint_tile(Card &card, int index)
{
    const DeskState &desk = desk_state();
    const Tile look = desk.travelling == index ? Tile::Going : desk.preset_active[index] ? Tile::At : Tile::Plain;
    char height[16] = "--";
    if (desk.preset_mm[index] >= 0) {
        std::snprintf(height, sizeof height, "%d.%d", (desk.preset_mm[index] / 10) % 1000, desk.preset_mm[index] % 10);
    }
    set_text(card.heights[index], height);
    lv_obj_set_x(card.units[index], widest(Role::ReadM, {height}) + space::xs);
    if (look == Tile::Going && desk.height_mm >= 0 && desk.preset_mm[index] >= 0) {
        set_icon(card.arrows[index], desk.preset_mm[index] > desk.height_mm ? ic::arrow_upward : ic::arrow_downward, false);
    }
    if (card.shown[index] == look) {
        return;
    }
    card.shown[index] = look;
    lv_obj_set_state(card.tiles[index], LV_STATE_CHECKED, look == Tile::At);
    lv_obj_set_style_border_width(card.tiles[index], look == Tile::Going ? 3 : 0, 0);
    lv_obj_set_hidden(card.arrows[index], look != Tile::Going);
    const Ink on = look == Tile::At ? Ink::OnAccent : Ink::Primary;
    set_ink(lv_obj_get_child(card.tiles[index], 0), on);
    set_ink(card.heights[index], on);
    set_ink(card.units[index], look == Tile::At ? Ink::OnAccent : Ink::Secondary);
}

void paint()
{
    if (s_card == nullptr) {
        return;
    }
    Card &card = *s_card;
    if (card.guests != guests_shown()) {
        close_popover(true);  // its size was for the presets it had
        return;
    }
    const DeskState &desk = desk_state();
    char where[48];
    say_where(where, sizeof where);
    set_text(card.where, where);
    for (lv_obj_t *move : card.moves) {
        set_usable(move, desk.available);
    }
    for (const int index : PRESETS) {
        if (card.tiles[index] != nullptr) {
            set_usable(card.tiles[index], desk.available);
            paint_tile(card, index);
        }
    }
    // stopping one tap anywhere along the foot while it moves; the hint while it stands
    const bool going = under_way();
    if (lv_obj_is_hidden(card.stop) == going) {
        lv_obj_set_hidden(card.stop, !going);
        lv_obj_set_hidden(card.hint, going);
    }
}

void on_stop(lv_event_t *)
{
    const DeskState &desk = desk_state();
    if (desk.travelling >= 0) {
        detail::desk_go_to(desk.travelling);  // a tap on the preset it goes to stops it, as the box's keys do
    } else if (detail::s_handlers.move != nullptr) {
        detail::s_handlers.move(Move::Stop);
    }
}

void on_move(lv_event_t *e)
{
    const auto way = static_cast<Move>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    const Move now = lv_event_get_code(e) == LV_EVENT_PRESSED ? way : Move::Stop;
    if (s_card != nullptr) {
        s_card->held = now;
    }
    if (detail::s_handlers.move != nullptr) {
        detail::s_handlers.move(now);
    }
}

void on_preset(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) {
        detail::desk_go_to(index);
    } else if (detail::s_handlers.preset != nullptr) {
        detail::s_handlers.preset(index, true);  // held: the height it is at, stored there
    }
}

void add_move(Card &card, lv_obj_t *grid, Move way)
{
    const bool up  = way == Move::Up;
    lv_obj_t  *btn = button(grid, Button::Raised, nullptr, nullptr);
    lv_obj_set_size(btn, COL_W, MOVE_H);
    lv_obj_set_style_radius(btn, TILE_R, 0);
    lv_obj_set_style_pad_column(btn, space::xs, 0);
    icon(btn, up ? ic::keyboard_arrow_up : ic::keyboard_arrow_down, 34, false);
    text(btn, Role::Small, "Hold", Ink::Secondary);
    auto *data = reinterpret_cast<void *>(static_cast<std::intptr_t>(way));
    for (const lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(btn, on_move, code, data);
    }
    card.moves[up ? 0 : 1] = btn;
}

void add_preset(Card &card, lv_obj_t *grid, int index)
{
    lv_obj_t *tile = button(grid, Button::Raised, nullptr, nullptr, false, true);
    lv_obj_set_checkable(tile, false);  // checked where the desk stands, not by a tap
    lv_obj_set_size(tile, COL_W, PRESET_H);
    lv_obj_set_style_radius(tile, TILE_R, 0);
    lv_obj_set_style_pad_all(tile, space::l, 0);
    lv_obj_set_layout(tile, LV_LAYOUT_NONE);
    lv_obj_set_style_border_color(tile, colour(Ink::Accent), 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_t *name = text(tile, Role::H3, preset_name(index));
    lv_obj_set_pos(name, 0, 0);
    const std::int32_t inner_h = PRESET_H - 2 * space::l;
    const std::int32_t figure_y = inner_h - fonts::read_m()->line_height;
    card.heights[index] = text(tile, Role::ReadM, "--");
    lv_obj_set_pos(card.heights[index], 0, figure_y);
    card.units[index] = text(tile, Role::Small, "cm", Ink::Secondary);
    lv_obj_set_y(card.units[index], figure_y + unit_y());
    card.arrows[index] = icon(tile, ic::arrow_upward, 28, false, Ink::Accent);
    lv_obj_align(card.arrows[index], LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_hidden(card.arrows[index], true);
    auto *data = reinterpret_cast<void *>(static_cast<std::intptr_t>(index));
    lv_obj_add_event_cb(tile, on_preset, LV_EVENT_SHORT_CLICKED, data);
    lv_obj_add_event_cb(tile, on_preset, LV_EVENT_LONG_PRESSED, data);
    card.tiles[index] = tile;
}
}  // namespace

void open_desk(lv_obj_t *source)
{
    const bool guests = guests_shown();
    int        shown  = 0;
    for (const int index : PRESETS) {
        shown += guests || !is_guest(index) ? 1 : 0;
    }
    const int          rows   = (shown + 1) / 2;
    const std::int32_t grid_h = MOVE_H + rows * (PRESET_H + GAP);
    const std::int32_t head_h = fonts::h3()->line_height;
    const std::int32_t card_h = 2 * space::xxl + head_h + space::l + grid_h + space::l + FOOT_H;

    lv_obj_t *body = popover(source, CARD_W, card_h, From::Beside);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, space::l, 0);
    auto *card   = new Card;
    card->guests = guests;

    lv_obj_t *head = flow(body, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_width(head, INNER);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(head, Role::H3, "Desk");
    filler(head);
    card->where = text(head, Role::Small, "", Ink::Secondary);
    lv_obj_set_width(card->where, widest(Role::Small, {"Going to Ultra low"}));
    lv_obj_set_style_text_align(card->where, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *grid = flow(body, LV_FLEX_FLOW_ROW_WRAP, GAP);
    lv_obj_set_width(grid, INNER);
    lv_obj_set_style_pad_row(grid, GAP, 0);
    add_move(*card, grid, Move::Up);
    add_move(*card, grid, Move::Down);
    for (const int index : PRESETS) {
        if (guests || !is_guest(index)) {
            add_preset(*card, grid, index);
        }
    }

    // the hint while it stands, and in its place a wide Stop while it moves
    lv_obj_t *foot = plain(body);
    lv_obj_set_size(foot, INNER, FOOT_H);
    card->hint = flow(foot, LV_FLEX_FLOW_ROW, space::s);
    lv_obj_set_flex_align(card->hint, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(card->hint, LV_ALIGN_LEFT_MID, 0, 0);
    icon(card->hint, ic::info, 24, false, Ink::Tertiary);
    text(card->hint, Role::Small, "Hold a preset to save this height", Ink::Tertiary);
    card->stop = button(foot, Button::Raised, ic::stop, "Stop");
    lv_obj_set_size(card->stop, INNER, FOOT_H);
    lv_obj_set_style_bg_color(card->stop, colour(Ink::Red), 0);
    lv_obj_set_style_bg_opa(card->stop, 52, 0);
    lv_obj_set_style_text_color(card->stop, colour(Ink::Red), 0);
    lv_obj_set_hidden(card->stop, true);
    lv_obj_add_event_cb(card->stop, on_stop, LV_EVENT_CLICKED, nullptr);

    card->idle = lv_timer_create(
        [](lv_timer_t *) {
            if (lv_display_get_inactive_time(nullptr) >= IDLE_MS && !under_way()) {
                close_popover(true);
            }
        },
        1000, nullptr);
    lv_obj_add_event_cb(
        body,
        [](lv_event_t *) {
            Card *gone = s_card;
            s_card     = nullptr;
            lv_timer_delete(gone->idle);
            if (gone->held != Move::Stop && detail::s_handlers.move != nullptr) {
                detail::s_handlers.move(Move::Stop);  // its button went from under the finger
            }
            delete gone;
        },
        LV_EVENT_DELETE, nullptr);
    s_card = card;
    paint();
}

void follow_desk()
{
    subscribe(Topic::Desk, detail::kNoView, paint);
    subscribe(Topic::Status, detail::kNoView, paint);
    subscribe(Topic::Settings, detail::kNoView, paint);
}
}  // namespace ui::kitfront
