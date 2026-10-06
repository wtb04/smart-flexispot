// The desk, folded out of the dock's height: its height over its travel,
// every preset, held to store, and up and down for as long as they are held.

#include "kit_front.h"

#include "kit_fonts.h"
#include "room_model.h"
#include "settings_model.h"

#include <cstdio>
#include <cstring>

#ifndef CONFIG_LOCTEK_MIN_HEIGHT_MM  // the simulator's desk
#define CONFIG_LOCTEK_MIN_HEIGHT_MM 660
#define CONFIG_LOCTEK_MAX_HEIGHT_MM 1310
#endif

namespace ui::kitfront {
namespace {
namespace ic = kit::glyph;
using namespace kit;
using detail::desk_state;
using detail::DeskState;

constexpr int TRAVEL_MIN = CONFIG_LOCTEK_MIN_HEIGHT_MM, TRAVEL_MAX = CONFIG_LOCTEK_MAX_HEIGHT_MM;

// Stand and Sit first, as the dock has them as one button; 5 and 6 are for
// whoever uses the desk while the owner's phone is away.
constexpr int PRESETS[] = {detail::STAND_PRESET, detail::SIT_PRESET, detail::ULTRA_LOW_PRESET, 0, 4, 5};
constexpr int GUESTS[]  = {4, 5};

constexpr std::int32_t COL_W = 160, MOVE_H = 88, PRESET_H = 104, GAP = space::m;
constexpr std::int32_t RAIL_W = 46;  // a rail with ticks, across
constexpr std::int32_t GRID_W = 2 * COL_W + GAP;
constexpr std::int32_t CARD_W = 2 * space::xxl + RAIL_W + space::l + GRID_W;
constexpr std::uint32_t IDLE_MS = 20 * 1000;

enum class Tile : std::uint8_t { Plain, At, Going };

struct Card : InPsram {
    lv_obj_t   *where = nullptr;
    Rail       *rail  = nullptr;
    lv_obj_t   *moves[2]{};
    lv_obj_t   *tiles[kPresetCount]{}, *names[kPresetCount]{}, *details[kPresetCount]{};
    Tile        shown[kPresetCount]{};
    bool        guests = false;
    Move        held   = Move::Stop;
    lv_timer_t *idle   = nullptr;
};

Card *s_card = nullptr;

bool guests_shown()
{
    return owner_away() || !detail::settings_state().on[static_cast<int>(Setting::PresenceGate)];
}

bool is_guest(int index) { return index == GUESTS[0] || index == GUESTS[1]; }

void tenths(char *out, std::size_t size, int mm)
{
    std::snprintf(out, size, "%d.%d cm", (mm / 10) % 10000, mm % 10);  // the desk is never 10 m tall: the bound is for the compiler
}

void set_text(lv_obj_t *label, const char *value)
{
    if (std::strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}

void say_where(char *out, std::size_t size)
{
    const DeskState &desk = desk_state();
    if (!desk.available) {
        std::snprintf(out, size, "Not answering");
        return;
    }
    if (desk.height_mm < 0) {
        if (desk.travelling >= 0) {
            std::snprintf(out, size, "Going to %s", preset_name(desk.travelling));
        } else {
            std::snprintf(out, size, "Height not known yet");
        }
        return;
    }
    char height[24];
    tenths(height, sizeof height, desk.height_mm);
    if (desk.travelling >= 0) {
        std::snprintf(out, size, "Going to %s, %s", preset_name(desk.travelling), height);
        return;
    }
    for (const int index : PRESETS) {
        if (desk.preset_active[index]) {
            std::snprintf(out, size, "At %s, %s", preset_name(index), height);
            return;
        }
    }
    std::snprintf(out, size, "Between presets, %s", height);
}

void paint_tile(Card &card, int index)
{
    const DeskState &desk = desk_state();
    const Tile look = desk.travelling == index ? Tile::Going : desk.preset_active[index] ? Tile::At : Tile::Plain;
    char detail[24] = "";
    if (look == Tile::Going) {
        std::snprintf(detail, sizeof detail, "On its way");
    } else if (desk_state().preset_mm[index] >= 0) {
        tenths(detail, sizeof detail, desk_state().preset_mm[index]);
    }
    set_text(card.details[index], detail);
    if (card.shown[index] == look) {
        return;
    }
    card.shown[index] = look;
    lv_obj_set_state(card.tiles[index], LV_STATE_CHECKED, look == Tile::At);
    set_soft(card.tiles[index], look == Tile::Going);
    set_ink(card.names[index], look == Tile::At ? Ink::OnAccent : look == Tile::Going ? Ink::Accent : Ink::Primary);
    set_ink(card.details[index], look == Tile::At ? Ink::OnAccent : look == Tile::Going ? Ink::Accent : Ink::Secondary);
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
    if (desk.height_mm >= 0 && card.rail->value() != desk.height_mm) {
        // never on the spring: a Rail freed as the card closes would leave its animation running on it
        card.rail->set_value(desk.height_mm, false);
    }
    const lv_opa_t rail_opa = desk.height_mm < 0 ? LV_OPA_TRANSP : desk.available ? LV_OPA_COVER : LV_OPA_40;
    if (lv_obj_get_style_opa(card.rail->obj(), LV_PART_MAIN) != rail_opa) {
        lv_obj_set_style_opa(card.rail->obj(), rail_opa, 0);
    }
    for (lv_obj_t *move : card.moves) {
        set_usable(move, desk.available);
    }
    for (const int index : PRESETS) {
        if (card.tiles[index] != nullptr) {
            set_usable(card.tiles[index], desk.available);
            paint_tile(card, index);
        }
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
    lv_obj_t  *btn = button(grid, Button::Raised, up ? ic::keyboard_arrow_up : ic::keyboard_arrow_down, up ? "Up" : "Down");
    lv_obj_set_size(btn, COL_W, MOVE_H);
    lv_obj_set_style_pad_column(btn, space::xs, 0);
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
    lv_obj_set_style_pad_all(tile, space::l, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    card.names[index]   = text(tile, Role::H3, preset_name(index));
    card.details[index] = text(tile, Role::Small, "", Ink::Secondary);
    lv_obj_set_width(card.details[index], COL_W - 2 * space::l);
    auto *data = reinterpret_cast<void *>(static_cast<std::intptr_t>(index));
    lv_obj_add_event_cb(tile, on_preset, LV_EVENT_SHORT_CLICKED, data);
    lv_obj_add_event_cb(tile, on_preset, LV_EVENT_LONG_PRESSED, data);
    card.tiles[index] = tile;
}

// The heading and the line under the grid, as tall as their fonts make them.
void measure(std::int32_t &head, std::int32_t &foot)
{
    static std::int32_t s_head = 0, s_foot = 0;
    if (s_head == 0) {
        lv_obj_t *probe = flow(layer(Layer::Overlay), LV_FLEX_FLOW_COLUMN, 0);
        lv_obj_t *h     = heading(probe, "Desk", "At");
        lv_obj_t *f     = text(probe, Role::Small, "Hold");
        lv_obj_update_layout(probe);
        s_head = lv_obj_get_height(h);
        s_foot = lv_obj_get_height(f);
        lv_obj_delete(probe);
    }
    head = s_head;
    foot = s_foot;
}
}  // namespace

void open_desk(lv_obj_t *source)
{
    const bool guests = guests_shown();
    int        shown  = 0;
    for (const int index : PRESETS) {
        shown += guests || !is_guest(index) ? 1 : 0;
    }
    const int          rows   = 1 + (shown + 1) / 2;
    const std::int32_t grid_h = MOVE_H + (rows - 1) * PRESET_H + (rows - 1) * GAP;
    std::int32_t       head = 0, foot = 0;
    measure(head, foot);
    const std::int32_t card_h = 2 * space::xxl + head + space::l + grid_h + space::l + foot;

    lv_obj_t *body = popover(source, CARD_W, card_h, From::Beside);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, space::l, 0);
    auto *card   = new Card;
    card->guests = guests;
    lv_obj_t *head_row = heading(body, "Desk", "");
    card->where = lv_obj_get_child(lv_obj_get_child(head_row, 0), 1);
    lv_obj_set_width(card->where, CARD_W - 2 * space::xxl);

    lv_obj_t *row = flow(body, LV_FLEX_FLOW_ROW, space::l);
    std::int32_t notches[kPresetCount];
    int          notch_count = 0;
    for (const int index : PRESETS) {
        if ((guests || !is_guest(index)) && desk_state().preset_mm[index] >= 0) {
            notches[notch_count++] = desk_state().preset_mm[index];
        }
    }
    RailSpec rs;
    rs.length = grid_h, rs.vertical = true, rs.min = TRAVEL_MIN, rs.max = TRAVEL_MAX, rs.ticks = 14, rs.major = 2;
    rs.notches = notches, rs.notch_count = notch_count;
    card->rail = new Rail(row, rs);
    if (desk_state().height_mm >= 0) {
        card->rail->set_value(desk_state().height_mm, false);
    }
    lv_obj_t *grid = flow(row, LV_FLEX_FLOW_ROW_WRAP, GAP);
    lv_obj_set_width(grid, GRID_W);
    add_move(*card, grid, Move::Up);
    add_move(*card, grid, Move::Down);
    for (const int index : PRESETS) {
        if (guests || !is_guest(index)) {
            add_preset(*card, grid, index);
        }
    }
    text(body, Role::Small, "Hold a preset to save the height it goes to", Ink::Tertiary);

    card->idle = lv_timer_create(
        [](lv_timer_t *) {
            if (lv_display_get_inactive_time(nullptr) >= IDLE_MS) {
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
            delete gone->rail;
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
