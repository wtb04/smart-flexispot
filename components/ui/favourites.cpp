#include "ui_internal.h"

#include "media_model.h"
#include "topics.h"

#include "esp_heap_caps.h"

// The favourites to start while nothing plays, a cover each with its name, in
// a popup over the page or the music view.
namespace ui::detail {
namespace {
// Favourites, from the bar's play while nothing plays: a cover each with its
// name, as many columns as there are favourites up to a row's worth.
constexpr std::int32_t PICK_COLUMNS  = 4;
constexpr std::int32_t PICKER_PAD      = theme::space::l;
constexpr std::int32_t PICKER_HEADER_H = 52;
constexpr std::int32_t PICK_NAME_GAP   = 8;
constexpr std::int32_t PICK_ART_RADIUS = 14;
constexpr std::int32_t PICK_MIN_W      = 320;  // room for the title and the close button

struct PickView {
    lv_obj_t      *root;
    lv_obj_t      *art;
    lv_obj_t      *name;
    lv_image_dsc_t dsc;
    std::uint16_t *rounded;  // the cover with its corners already in the card's colour
    bool           named;
    std::uint32_t  arts_shown;  // the model's count of its covers when last drawn
};
PickView                    s_pick_views[media::kPickCount]{};
std::optional<ModalOverlay> s_pick_picker;

std::int32_t pick_height()
{
    return media::kPickArtSize + PICK_NAME_GAP + lv_font_get_line_height(fonts::size_20());
}

void pick_clicked_cb(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    s_pick_picker->close();
    if (s_handlers.pick != nullptr) {
        s_handlers.pick(index);
    }
}

void build_pick(lv_obj_t *grid, int index)
{
    const std::int32_t side = media::kPickArtSize;
    PickView          &view = s_pick_views[index];

    view.root = lv_obj_create(grid);
    lv_obj_set_size(view.root, side, pick_height());
    lv_obj_set_style_bg_opa(view.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view.root, 0, 0);
    lv_obj_set_style_pad_all(view.root, 0, 0);
    lv_obj_set_scrollable(view.root, false);
    lv_obj_set_style_opa(view.root, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(view.root, pick_clicked_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
    lv_obj_set_hidden(view.root, true);

    view.art = lv_image_create(view.root);
    lv_obj_align(view.art, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_clickable(view.art, false);
    lv_obj_set_hidden(view.art, true);

    view.name = theme::make_line(view.root, theme::text, fonts::size_20(), side);
    lv_obj_align(view.name, LV_ALIGN_TOP_LEFT, 0, side + PICK_NAME_GAP);
}

std::uint16_t rgb565_of(std::uint32_t colour)
{
    return static_cast<std::uint16_t>(((colour >> 8) & 0xf800) | ((colour >> 5) & 0x07e0) |
                                      ((colour >> 3) & 0x001f));
}

std::uint16_t blend565(std::uint16_t a, std::uint16_t b, int b_share, int whole)
{
    const auto channel = [&](int shift, int mask) {
        const int ca = (a >> shift) & mask;
        const int cb = (b >> shift) & mask;
        return ((ca * (whole - b_share) + cb * b_share) / whole) << shift;
    };
    constexpr int RED = 11, GREEN = 5, FIVE_BITS = 0x1f, SIX_BITS = 0x3f;
    return static_cast<std::uint16_t>(channel(RED, FIVE_BITS) | channel(GREEN, SIX_BITS) |
                                      channel(0, FIVE_BITS));
}

/** A square picture copied with its corners rounded over `background`, their
 *  edge smoothed by sampling each corner pixel several times. */
void round_corners(const std::uint16_t *from, std::uint16_t *to, std::int32_t side,
                   std::int32_t radius, std::uint32_t background)
{
    constexpr int SAMPLES = 4;  // a side, per pixel
    constexpr int WHOLE   = SAMPLES * SAMPLES;
    const std::uint16_t fill = rgb565_of(background);
    std::copy(from, from + side * side, to);
    for (std::int32_t y = 0; y < radius; ++y) {
        for (std::int32_t x = 0; x < radius; ++x) {
            int outside = 0;
            for (int sy = 0; sy < SAMPLES; ++sy) {
                for (int sx = 0; sx < SAMPLES; ++sx) {
                    const float dx = static_cast<float>(radius) - (x + (sx + 0.5f) / SAMPLES);
                    const float dy = static_cast<float>(radius) - (y + (sy + 0.5f) / SAMPLES);
                    outside += dx * dx + dy * dy > static_cast<float>(radius * radius) ? 1 : 0;
                }
            }
            if (outside == 0) {
                continue;
            }
            // The same share at each of the four corners, mirrored.
            for (const auto &[px, py] : {std::pair{x, y}, std::pair{side - 1 - x, y},
                                        std::pair{x, side - 1 - y},
                                        std::pair{side - 1 - x, side - 1 - y}}) {
                std::uint16_t &pixel = to[py * side + px];
                pixel                = blend565(pixel, fill, outside, WHOLE);
            }
        }
    }
}

/** Sized for the favourites there are, since that changes between openings. */
void fit_pick_picker(int count)
{
    const std::int32_t columns = std::min<std::int32_t>(count, PICK_COLUMNS);
    const std::int32_t rows    = (count + PICK_COLUMNS - 1) / PICK_COLUMNS;
    const std::int32_t grid_w  = columns * media::kPickArtSize + (columns - 1) * BUTTON_GAP;
    const std::int32_t grid_h  = rows * pick_height() + (rows - 1) * BUTTON_GAP;
    s_pick_picker->resize(std::max(grid_w, PICK_MIN_W) + 2 * PICKER_PAD,
                          grid_h + PICKER_HEADER_H + 2 * PICKER_PAD);
}

void paint_picks();

void build_pick_picker(lv_obj_t *parent)
{
    s_pick_picker.emplace(parent, PICK_MIN_W, PICKER_HEADER_H);
    lv_obj_t *card = s_pick_picker->content();
    lv_obj_set_style_pad_all(card, PICKER_PAD, 0);

    lv_obj_t *title = theme::make_accent_label(card, "FAVOURITES", fonts::size_22());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_pos(grid, 0, PICKER_HEADER_H);
    lv_obj_set_size(grid, PICK_COLUMNS * (media::kPickArtSize + BUTTON_GAP), LV_SIZE_CONTENT);
    theme::style_panel(grid, theme::panel, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(grid, false);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, BUTTON_GAP, 0);
    lv_obj_set_style_pad_column(grid, BUTTON_GAP, 0);

    for (int i = 0; i < media::kPickCount; ++i) {
        build_pick(grid, i);
        s_pick_views[i].arts_shown = UINT32_MAX;
    }
    s_pick_picker->add_close_button();
    subscribe(Topic::Picks, kNoView, paint_picks);
}

// The favourites as the media model has them: those named, with their covers.
void paint_picks()
{
    for (int index = 0; index < media::kPickCount; ++index) {
        PickView   &view = s_pick_views[index];
        const Pick &pick = media_pick(index);
        if (view.root == nullptr) {
            continue;
        }
        view.named = pick.name[0] != '\0';
        theme::set_text(view.name, pick.name);
        lv_obj_set_hidden(view.root, !view.named);
        if (pick.arts == view.arts_shown) {
            continue;
        }
        view.arts_shown = pick.arts;
        if (pick.art != nullptr && view.rounded == nullptr) {
            view.rounded = static_cast<std::uint16_t *>(heap_caps_malloc(
                media::kPickArtSize * media::kPickArtSize * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        lv_obj_set_hidden(view.art, pick.art == nullptr || view.rounded == nullptr);
        if (pick.art == nullptr || view.rounded == nullptr) {
            continue;
        }
        // Rounded once here rather than clipped on every frame, which in software
        // costs more than the rest of the popup.
        round_corners(static_cast<const std::uint16_t *>(pick.art), view.rounded, media::kPickArtSize,
                      PICK_ART_RADIUS, theme::panel);
        const std::uint32_t bytes = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
        view.dsc.header.magic     = LV_IMAGE_HEADER_MAGIC;
        view.dsc.header.cf        = LV_COLOR_FORMAT_RGB565;
        view.dsc.header.w         = media::kPickArtSize;
        view.dsc.header.h         = media::kPickArtSize;
        view.dsc.header.stride    = media::kPickArtSize * bytes;
        view.dsc.data_size        = media::kPickArtSize * media::kPickArtSize * bytes;
        view.dsc.data             = reinterpret_cast<const std::uint8_t *>(view.rounded);
        lv_image_set_src(view.art, &view.dsc);
        lv_obj_invalidate(view.art);
    }
}

void open_pick_picker()
{
    const int count = media_pick_count();
    if (count == 0 || !s_pick_picker.has_value()) {
        return;
    }
    fit_pick_picker(count);
    s_pick_picker->open();
}
}  // namespace


void open_favourites()
{
    open_pick_picker();
}

void build_favourites(lv_obj_t *screen)
{
    build_pick_picker(screen);
}

}  // namespace ui::detail
