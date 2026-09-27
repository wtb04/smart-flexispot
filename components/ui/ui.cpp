#include "ui_internal.h"
#include "focus_page.h"

#ifndef SHOT_DRAWER
#define SHOT_DRAWER 0
#endif
#ifndef SHOT_ENABLED
#define SHOT_ENABLED 0
#endif

namespace ui::detail {
namespace {
constexpr std::uint32_t APPLY_PERIOD_MS = 20;

constexpr std::int32_t NAV_GAP    = GAP;

constexpr int DEFAULT_BRIGHTNESS_PERCENT = 80;

constexpr std::uint32_t SHOT_START_MS = 25000;
constexpr std::uint32_t SHOT_PAGE_MS  = 2000;
}  // namespace

bool s_rail_right = false;

Orientation s_orientation = Orientation::Normal;

Layout layout()
{
    lv_display_t      *disp = lv_display_get_default();
    const std::int32_t w    = lv_display_get_horizontal_resolution(disp);
    const std::int32_t h    = lv_display_get_vertical_resolution(disp);
    const std::int32_t x      = s_rail_right ? GAP : RAIL_W + GAP;
    const std::int32_t area_h = h - GAP - EDGE_GAP;
    return Layout{w,
                  h,
                  x,
                  w - RAIL_W - 2 * GAP,
                  area_h - NAV_H - NAV_GAP,
                  s_rail_right ? w - RAIL_W : 0,
                  s_rail_right};
}

Handlers s_handlers{};
namespace {
// The two move buttons and a button per preset, stand and sit among them.
constexpr int MOVE_BUTTON_COUNT = 2;
constexpr int DESK_CONTROL_MAX  = MOVE_BUTTON_COUNT + kPresetCount;
bool          s_desk_available                  = true;

lv_obj_t     *s_desk_controls[DESK_CONTROL_MAX] = {};
int           s_desk_control_count              = 0;
}  // namespace

bool      s_screen_on         = true;

bool s_notice_lit_screen = false;

void set_screen_state(bool on)
{
    if (on == s_screen_on || s_handlers.screen == nullptr) {
        return;
    }
    s_screen_on = on;
    s_handlers.screen(on);
}
namespace {
void wake_on_touch(lv_event_t *)
{
    s_notice_lit_screen = false;
    if (s_screen_on) {
        return;
    }

    // The tap that lights the screen does nothing else: whatever was under the
    // finger was not visible when it landed, and one of those things moves a desk.
    // lv_indev_active(), not lv_event_get_indev() -- the latter returns the event's
    // parameter, which is null for an event sent to the device itself, so both
    // calls below quietly did nothing. stop_processing withholds the press; the
    // reset drops the device's hold so the release cannot arrive as a click.
    lv_indev_t *indev = lv_indev_active();
    lv_indev_stop_processing(indev);
    lv_indev_reset(indev, nullptr);
    set_screen_state(true);
}
}  // namespace

void screen_off_cb(lv_event_t *)
{
    s_notice_lit_screen = false;
    set_screen_state(false);
}

void register_desk_control(lv_obj_t *obj)
{
    if (s_desk_control_count < DESK_CONTROL_MAX) {
        s_desk_controls[s_desk_control_count++] = obj;
    }
}
int               s_initial_brightness = DEFAULT_BRIGHTNESS_PERCENT;

std::optional<SegmentDisplay> s_height;
lv_obj_t *s_rail          = nullptr;
lv_obj_t *s_content       = nullptr;
lv_obj_t *s_clock_box     = nullptr;
lv_obj_t *s_side_buttons[CHOICE_COUNT]      = {};
lv_obj_t *s_flip_buttons[ORIENTATION_COUNT] = {};
lv_obj_t *s_wifi_icon     = nullptr;
lv_obj_t *s_phone_icon    = nullptr;
lv_obj_t *s_clock_hours   = nullptr;
lv_obj_t *s_clock_colon   = nullptr;
lv_obj_t *s_clock_minutes = nullptr;
bool      s_clock_known   = false;
namespace {
// One page per tick rather than all at once: a tab's colour eases in, and a
// picture taken straight after the switch shows the old tab lit. Pages hidden
// while the phone is away are the ones most often worth looking at, so the gate
// is lifted for as long as the pictures take.
void take_screenshots(lv_timer_t *timer)
{
    // Only the pages being worked on: every one adds about a minute.
    static const int PAGES[] = {CALENDAR_PAGE};
    static int       step    = -1;
    static bool      gated   = false;
    if (step >= 0) {
        screenshot();
    } else {
        gated           = s_presence_gate;
        s_presence_gate = false;
    }
    if (++step < static_cast<int>(std::size(PAGES))) {
        select_page(PAGES[step]);
        if (SHOT_DRAWER) {
            place_drawer(DRAWER_W);
        }
        lv_timer_set_period(timer, SHOT_PAGE_MS);
        return;
    }
    s_presence_gate = gated;
    select_page(HOME_PAGE);
    lv_timer_delete(timer);
}

void build_screen()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(theme::background), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollable(scr, false);

    for (lv_indev_t *dev = lv_indev_get_next(nullptr); dev != nullptr;
         dev = lv_indev_get_next(dev)) {
        lv_indev_add_event_cb(dev, wake_on_touch, LV_EVENT_PRESSED, nullptr);
    }

    create_rail(scr);
    create_content(scr);
    // Development: hands a picture of the screen to tools/screenshot.py a little
    // after boot. It holds the LVGL lock for several seconds, so it is off
    // unless wanted.
    if (SHOT_ENABLED) {
        lv_timer_create(take_screenshots, SHOT_START_MS, nullptr);
    }
    create_drawer(scr);  // after the content, so it overlays it when open
    build_cinema(scr);
    build_focus_full(scr);
    lv_obj_move_foreground(s_rail);  // and under the rail, which it slides out from
    create_notice_card();
    build_splash();  // last, so it covers everything until startup finishes

}
}  // namespace

}  // namespace ui::detail

namespace ui {
using namespace detail;

const char *preset_name(int index)
{
    return index >= 0 && index < kPresetCount ? PRESET_NAMES[index] : deskproto::kBetween;
}

namespace {
// Every set_* below runs on some other task. None of them touches LVGL: each
// copies its arguments into a slot holding only the latest value and returns,
// and apply_pending() on the LVGL task draws whatever changed. So no caller
// ever waits on the screen, and no update is ever lost, only superseded.
// Notifications are events rather than state, and queue instead.
portMUX_TYPE      s_pending_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> s_pending{false};

template <typename T>
struct Slot {
    bool dirty = false;
    T    value{};
};

void copy_text(char *dest, std::size_t size, const char *source)
{
    std::size_t i = 0;
    if (source != nullptr) {
        for (; i + 1 < size && source[i] != '\0'; ++i) {
            dest[i] = source[i];
        }
    }
    dest[i] = '\0';
}

/** A string that may be null, kept as text plus whether there was any. */
template <std::size_t N>
struct Text {
    char text[N];
    bool null;

    void set(const char *source)
    {
        null = source == nullptr;
        copy_text(text, N, source);
    }
    const char *get() const { return null ? nullptr : text; }
};

template <typename T>
void put(Slot<T> &slot, const T &value)
{
    portENTER_CRITICAL(&s_pending_lock);
    slot.value = value;
    slot.dirty = true;
    portEXIT_CRITICAL(&s_pending_lock);
    s_pending.store(true, std::memory_order_release);
}

template <typename T>
bool take(Slot<T> &slot, T &out)
{
    portENTER_CRITICAL(&s_pending_lock);
    const bool dirty = slot.dirty;
    if (dirty) {
        out        = slot.value;
        slot.dirty = false;
    }
    portEXIT_CRITICAL(&s_pending_lock);
    return dirty;
}

struct MediaArgs {
    Text<32>  source;
    Text<128> title;
    Text<128> artist;
    Text<24>  state;
    bool      playing;
    bool      controllable;
    bool      art_coming;
};
struct ProgressArgs {
    int  position_s;
    int  duration_s;
    bool playing;
};
struct ArtArgs {
    const void *pixels;
    bool        placeholder;
};
struct PillArgs {
    Text<40> label;
    Text<40> value;
    Level    level;
};
struct LightsArgs {
    Text<40> label;
    Text<32> state;
    bool     on;
};
struct LightArgs {
    Text<48> name;
    Text<32> state;
    bool     on;
};
struct ToggleArgs {
    Text<32> label;
    bool     on;
};
struct RangeArgs {
    float min_c;
    float max_c;
    float step_c;
};
struct ThermostatArgs {
    float    current_c;
    float    target_c;
    Text<24> mode;
    Hvac     state;
};
using TimeText = Text<16>;
struct PresenceArgs {
    bool has_key;
    bool present;
    bool ever_seen;
};
struct InfoArgs {
    Text<64> value;
    Level    level;
};
struct DetailsArgs {
    char            hex[radar::kHexLen];
    radar::Details  details;
};
struct SegmentsArgs {
    MediaSegment items[kMaxSegments];
    int          count;
};
struct PickArgs {
    Text<48> name;
};
struct PhotoArgs {
    char        hex[radar::kHexLen];
    const void *pixels;
    int         width;
    int         height;
};

Slot<bool>           p_preset_active[kPresetCount];
Slot<int>            p_height;
Slot<bool>           p_desk_available;
Slot<MediaArgs>      p_media;
Slot<ProgressArgs>   p_progress;
Slot<int>            p_media_volume;
Slot<ArtArgs>        p_art;
Slot<PickArgs>       p_pick[media::kPickCount];
Slot<const void *>   p_pick_art[media::kPickCount];
Slot<SegmentsArgs>   p_segments;
Slot<bool>           p_media_seeks;
Slot<const void *>   p_still;
Slot<PillArgs>       p_pill[kPillCount];
Slot<LightsArgs>     p_lights;
Slot<LightArgs>      p_light[kLightCount];
Slot<ToggleArgs>     p_toggle[kDialToggleCount];
Slot<RangeArgs>      p_range;
Slot<ThermostatArgs> p_thermostat;
Slot<PresenceArgs>   p_presence;
Slot<TimeText>       p_time;
Slot<bool>           p_wifi;
// Kept in PSRAM: sized for the most any card can have, it is several
// kilobytes, and static data would take them from internal RAM.
Slot<InfoArgs>      *p_rows = nullptr;  // [card * kMaxRows + row]
Slot<InfoArgs>       p_card[kMaxCards];
Slot<Text<24>>       p_glance[kGlanceCount];
Slot<int>            p_media_hold;
Slot<int>            p_notification_volume;
Slot<bool>           p_screen;
Slot<bool>           p_setting[SETTING_COUNT];
Slot<bool>           p_calendar;
Slot<Focus>          p_focus;
Slot<UpdateState>    p_update;
Slot<bool>           p_radar;
Slot<DetailsArgs>    p_details;
Slot<PhotoArgs>      p_photo;

// Notifications in the order they were asked for; the oldest goes when full.
constexpr int NOTICE_INBOX_LEN = 4;
Notice        s_inbox[NOTICE_INBOX_LEN];
int           s_inbox_count = 0;

void drop_oldest(Notice *queue, int &count)
{
    for (int i = 1; i < count; ++i) {
        queue[i - 1] = queue[i];
    }
    --count;
}

// ---- What each update does, on the LVGL task. ----

constexpr std::int32_t MEDIA_TITLE_Y          = 28;  // under the source line
constexpr std::int32_t RGB565_BYTES_PER_PIXEL = 2;

void apply_preset_active(int index, bool active)
{
    if (s_preset_buttons[index] == nullptr || s_preset_active[index] == active) {
        return;
    }
    s_preset_active[index] = active;
    lv_obj_t *button       = s_preset_buttons[index];
    lv_obj_set_state(button, LV_STATE_CHECKED, active);
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(button); ++i) {
        lv_obj_t *child = lv_obj_get_child(button, i);
        theme::set_text_color(child, theme::text);
        for (std::uint32_t j = 0; j < lv_obj_get_child_count(child); ++j) {
            lv_obj_set_state(lv_obj_get_child(child, j), LV_STATE_CHECKED, active);
        }
    }
}

void apply_height(int height_mm)
{
    if (s_height.has_value()) {
        s_height->set_tenths(height_mm);
    }
}

void apply_desk_available(bool available)
{
    s_desk_available = available;
    for (int i = 0; i < s_desk_control_count; ++i) {
        theme::set_usable(s_desk_controls[i], available);
    }
}

// The card's frame holds the cover, a blank where one would not come, or,
// while nothing plays, the speaker.
bool        s_media_idle      = true;
const void *s_art_pixels      = nullptr;
bool        s_art_placeholder = false;

void place_media_text(bool framed);

void frame_media()
{
    if (s_media_art == nullptr) {
        return;
    }
    const bool has_art = s_art_pixels != nullptr && !s_media_idle;
    const bool framed  = has_art || s_art_placeholder || s_media_idle;
    lv_obj_set_hidden(s_media_frame, !framed);
    lv_obj_set_hidden(s_panel_frame, !framed);
    lv_obj_set_hidden(s_media_art, !has_art);
    lv_obj_set_hidden(s_panel_art, !has_art);
    show_speaker_face(s_media_idle);
    place_media_text(framed);
}

void apply_media(const char *source, const char *title, const char *artist, const char *state,
                 bool playing, bool controllable)
{
    if (s_media_card == nullptr) {
        return;
    }
    const bool has_track = title != nullptr && title[0] != '\0';
    s_media_off = !controllable;
    theme::set_text(s_media_source, source != nullptr && source[0] != '\0' ? source : "SPEAKER");
    theme::set_text(s_media_title, has_track ? title : (state != nullptr ? state : "--"));
    theme::set_text(s_media_artist, has_track && artist != nullptr ? artist : "");
    theme::set_text_color(s_media_title, has_track ? theme::text : theme::secondary);

    theme::set_text(s_panel_title, has_track ? title : (state != nullptr ? state : "--"));
    theme::set_text(s_panel_artist, has_track && artist != nullptr ? artist : "");
    layout_media_text();
    // The first time too: a card that starts idle never changes into it.
    static bool s_framed = false;
    if (!std::exchange(s_framed, true) || s_media_idle == has_track) {
        s_media_idle = !has_track;
        frame_media();
    }

    s_has_track_shown = has_track;
    if (playing || !has_track) {
        cancel_pause_settle();
        apply_playing(playing);
    } else if (!s_playing_shown) {
        apply_playing(false);
    } else if (s_pause_timer == nullptr) {
        s_pause_timer = lv_timer_create(pause_settled, PAUSE_SETTLE_MS, nullptr);
    }
}

void apply_media_args(const MediaArgs &media)
{
    apply_media(media.source.get(), media.title.get(), media.artist.get(), media.state.get(),
                media.playing, media.controllable);
}

// A new track's text waits here for its cover, or for this long when that is slow.
constexpr std::uint32_t ART_WAIT_MS = 3 * units::kMsPerSecond;
MediaArgs               s_held_media;
bool                    s_media_held = false;
lv_timer_t             *s_held_timer = nullptr;

void drop_held_media()
{
    s_media_held = false;
    if (s_held_timer != nullptr) {
        lv_timer_delete(s_held_timer);
        s_held_timer = nullptr;
    }
}

void release_held_media()
{
    if (s_media_held) {
        drop_held_media();
        apply_media_args(s_held_media);
    }
}

void held_too_long(lv_timer_t *)
{
    release_held_media();
}

bool same_track(const MediaArgs &a, const MediaArgs &b)
{
    return std::strcmp(a.title.text, b.title.text) == 0 &&
           std::strcmp(a.artist.text, b.artist.text) == 0;
}

void hold_media(const MediaArgs &media)
{
    drop_held_media();
    s_held_media = media;
    s_media_held = true;
    s_held_timer = lv_timer_create(held_too_long, ART_WAIT_MS, nullptr);
    lv_timer_set_repeat_count(s_held_timer, 1);
    lv_timer_set_auto_delete(s_held_timer, false);
}

void apply_media_progress(int position_s, int duration_s, bool playing)
{
    if (s_panel_progress == nullptr) {
        return;
    }
    s_position_s    = position_s;
    s_duration_s    = duration_s;
    s_media_playing = playing;
    s_position_at   = xTaskGetTickCount();

    const bool known = duration_s > 0;
    lv_obj_set_hidden(s_panel_progress, !known);
    lv_obj_set_hidden(s_panel_elapsed, !known);
    lv_obj_set_hidden(s_panel_total, !known);
    if (known) {
        lv_bar_set_range(s_panel_progress, 0, duration_s * PROGRESS_SCALE);
        lv_bar_set_value(s_panel_progress, position_s * PROGRESS_SCALE, LV_ANIM_OFF);
        write_clock(s_panel_elapsed, position_s);
        write_clock(s_panel_total, duration_s);
    }
}

void apply_media_volume(int percent)
{
    if (s_panel_volume_pct == nullptr) {
        return;
    }
    write_percent(s_panel_volume_pct, percent);
}

void place_media_text(bool framed)
{
    const TextBox card  = framed ? s_card_with_art : s_card_bare;
    const TextBox panel = framed ? s_panel_with_art : s_panel_bare;
    // Nothing playing leaves the speaker's name and OFF, a short pair, which
    // sits level with the speaker beside it rather than at the top.
    const std::int32_t top = s_media_idle ? idle_media_text_top() : 0;
    theme::align(s_media_source, LV_ALIGN_TOP_LEFT, card.x, top);
    theme::align(s_media_title, LV_ALIGN_TOP_LEFT, card.x, top + MEDIA_TITLE_Y);
    lv_obj_set_width(s_media_title, card.w);
    lv_obj_set_width(s_media_artist, card.w);

    theme::align(s_panel_title, LV_ALIGN_TOP_LEFT, panel.x, 0);
    s_has_art = framed;
    lv_obj_set_width(s_panel_title, panel.w);
    lv_obj_set_width(s_panel_artist, panel.w);
    lv_obj_set_width(s_panel_progress, panel.w);
    layout_media_text();
}

void show_art_pixels(const void *pixels)
{
    // Two descriptors in turn, so the one on screen is never rewritten under it.
    lv_image_dsc_t &dsc = s_art_dsc[s_art_slot];
    s_art_slot          = 1 - s_art_slot;

    dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc.header.w      = media::kArtSize;
    dsc.header.h      = media::kArtSize;
    dsc.header.stride = media::kArtSize * RGB565_BYTES_PER_PIXEL;
    dsc.data_size     = media::kArtSize * media::kArtSize * RGB565_BYTES_PER_PIXEL;
    dsc.data          = static_cast<const std::uint8_t *>(pixels);

    lv_image_set_src(s_media_art, &dsc);
    lv_image_set_src(s_panel_art, &dsc);
    lv_obj_invalidate(s_media_art);
    lv_obj_invalidate(s_panel_art);
}

void apply_album_art(const void *pixels, bool placeholder)
{
    if (s_media_art == nullptr) {
        return;
    }
    s_art_pixels      = pixels;
    s_art_placeholder = placeholder;
    frame_media();
    if (pixels != nullptr) {
        show_art_pixels(pixels);
    }
}

void apply_pill(int index, const char *label, const char *value, Level level)
{
    Pill &pill = s_pills[index];
    if (pill.root == nullptr) {
        return;
    }
    const bool empty = label == nullptr || label[0] == '\0';
    lv_obj_set_hidden(pill.root, empty);
    if (pill.shown == empty) {
        pill.shown = !empty;
        reflow_pills();
    }
    if (!empty) {
        theme::set_text(pill.label, label);
        theme::set_text(pill.value, value != nullptr ? value : "--");
        theme::set_bg_color(pill.dot, level_ink(level));
    }
}

void apply_lights(const char *label, const char *state, bool on)
{
    if (s_lights_button == nullptr) {
        return;
    }
    theme::set_text(s_lights_name, label != nullptr ? label : "LIGHTS");
    theme::set_text(s_lights_state, state != nullptr ? state : "--");
    paint_light(s_lights_button, s_lights_name, s_lights_state, on);
    s_lights_on = on;
    paint_bulbs();
}

void apply_light(int index, const char *name, const char *state, bool on)
{
    LightButton &light = s_lights[index];
    if (light.root == nullptr) {
        return;
    }
    const bool empty = name == nullptr || name[0] == '\0';
    lv_obj_set_hidden(light.root, empty);
    lv_obj_set_hidden(s_bulbs[index], empty);
    if (!empty) {
        theme::set_text(light.name, name);
        theme::set_text(light.state, state != nullptr ? state : "--");
        paint_light(light.root, light.name, light.state, on);
    }
    s_light_on[index] = !empty && on;
    paint_bulbs();
}

void apply_dial_toggle(int index, const char *label, bool on)
{
    lv_obj_t *chip = s_dial_toggles[index];
    if (chip == nullptr) {
        return;
    }
    const bool empty = label == nullptr || label[0] == '\0';
    lv_obj_set_hidden(chip, empty);
    if (!empty) {
        lv_obj_t *text = lv_obj_get_child(chip, 0);
        theme::set_text(text, label);
        theme::center_ink(text);
        lv_obj_set_state(chip, LV_STATE_CHECKED, on);
        theme::set_text_color(text, on ? theme::text : theme::secondary);
        lv_obj_set_style_text_opa(text, on ? static_cast<lv_opa_t>(LV_OPA_COVER) : theme::mark_opa,
                                  0);
    }
}

void apply_thermostat_range(float min_c, float max_c, float step_c)
{
    if (s_dial == nullptr) {
        return;
    }
    lv_arc_set_range(s_dial, static_cast<int>(min_c * DIAL_SCALE),
                     static_cast<int>(max_c * DIAL_SCALE));
    s_dial_step = step_c > 0.0f ? step_c : DEFAULT_STEP_C;
}

void apply_thermostat(float current_c, float target_c, const char *mode, Hvac state)
{
    if (s_dial == nullptr) {
        return;
    }
    write_temperature(s_dial_current, current_c, true);
    if (!s_dial_dragging) {
        write_temperature(s_dial_target, target_c, true);
        if (target_c >= 0.0f) {
            lv_arc_set_value(s_dial, static_cast<int>(target_c * DIAL_SCALE + 0.5f));
        }
    }

    paint_dial(state);

    lv_obj_set_state(s_dial_mode, LV_STATE_CHECKED, state != Hvac::Off);
    lv_obj_t *mode_text = lv_obj_get_child(s_dial_mode, 0);
    theme::set_text(mode_text, mode != nullptr ? mode : "--");
    theme::set_text_color(mode_text, theme::text);
}

void apply_presence(bool has_key, bool present, bool ever_seen)
{
    if (s_phone_icon == nullptr) {
        return;
    }
    const bool here = has_key && present;
    static int shown_icon = -1;
    if (shown_icon != (here ? 1 : 0)) {
        shown_icon = here ? 1 : 0;
        lv_image_set_src(s_phone_icon, here ? &icons::phone_icon : &icons::phone_off_icon);
    }

    // A phone never seen is not here: the owner's pages stay hidden and the
    // guest presets show until it is.
    (void)ever_seen;
    if (here != s_present) {
        s_present = here;
        select_page(s_page);
    }
}

void apply_time(const char *text)
{
    if (s_clock_hours == nullptr) {
        return;
    }
    char        hours[4]   = "--";
    char        minutes[4] = "--";
    const char *colon      = text != nullptr ? std::strchr(text, ':') : nullptr;
    s_clock_known          = colon != nullptr;
    if (s_clock_known) {
        const std::size_t count = static_cast<std::size_t>(colon - text);
        std::snprintf(hours, sizeof(hours), "%.*s", static_cast<int>(count), text);
        std::snprintf(minutes, sizeof(minutes), "%s", colon + 1);
    } else {
        lv_obj_set_style_opa(s_clock_colon, LV_OPA_COVER, 0);
    }
    theme::set_text(s_clock_hours, hours);
    theme::set_text(s_clock_minutes, minutes);
}

void apply_wifi(bool wifi)
{
    static int shown = -1;
    if (s_wifi_icon == nullptr || shown == (wifi ? 1 : 0)) {
        return;
    }
    shown = wifi ? 1 : 0;
    lv_image_set_src(s_wifi_icon, wifi ? &icons::wifi_icon : &icons::wifi_off_icon);
}

void apply_row(int card, int row, const char *value, Level level)
{
    lv_obj_t *label = s_row_value[card][row];
    if (label == nullptr) {
        return;
    }
    theme::set_text(label, value != nullptr && value[0] != '\0' ? value : "--");
    theme::set_text_color(label, info_ink(level));
}

void apply_card(int card, const char *summary, Level level)
{
    if (s_tile_dot[card] == nullptr) {
        return;
    }
    theme::set_text(s_tile_value[card], summary != nullptr && summary[0] != '\0' ? summary : "--");
    theme::set_bg_color(s_tile_dot[card], level_ink(level));
    theme::set_text_color(s_tile_value[card], info_ink(level));
    if (s_card_level[card] != level) {
        s_card_level[card] = level;
        refresh_diag_summary();
    }
}

void apply_media_hold(int preset)
{
    s_media_hold = preset;
    if (preset >= 0 && s_media_panel.has_value()) {
        s_media_panel->close();  // what it controlled is no longer on the card
    }
}

void apply_notification_volume(int percent)
{
    if (s_volume_slider == nullptr) {
        return;
    }
    lv_slider_set_value(s_volume_slider, percent, LV_ANIM_OFF);
    write_percent(s_volume_value, percent);
}

void apply_screen(bool on)
{
    s_screen_on         = on;
    s_notice_lit_screen = false;
}

void apply_notice(const Notice &notice)
{
    if (s_notice_card == nullptr) {
        return;
    }
    if (s_notice_count == NOTIFY_QUEUE_LEN) {
        ESP_LOGW(TAG, "notification queue full, dropping oldest");
        drop_oldest(s_notice_queue, s_notice_count);
    }
    s_notice_queue[s_notice_count++] = notice;
    if (lv_obj_is_hidden(s_notice_card)) {
        show_next_notice();
    }
}

void apply_settings_and_presence()
{
    bool on = false;
    for (int i = 0; i < SETTING_COUNT; ++i) {
        if (take(p_setting[i], on)) {
            apply_setting(i, on);
        }
    }
    if (PresenceArgs presence{}; take(p_presence, presence)) {
        apply_presence(presence.has_key, presence.present, presence.ever_seen);
    }
    if (bool screen = false; take(p_screen, screen)) {
        apply_screen(screen);
    }
}

void apply_desk_updates()
{
    bool on = false;
    for (int i = 0; i < kPresetCount; ++i) {
        if (take(p_preset_active[i], on)) {
            apply_preset_active(i, on);
        }
    }
    if (bool available = false; take(p_desk_available, available)) {
        apply_desk_available(available);
    }
    if (int height = 0; take(p_height, height)) {
        apply_height(height);
    }
}

void apply_media_updates()
{
    static MediaArgs media;  // large for the LVGL task's stack
    if (take(p_media, media)) {
        if (media.art_coming) {
            hold_media(media);
        } else if (s_media_held && same_track(media, s_held_media)) {
            s_held_media = media;  // still waiting on its cover
        } else {
            drop_held_media();
            apply_media_args(media);
        }
    }
    if (ArtArgs art{}; take(p_art, art)) {
        apply_album_art(art.pixels, art.placeholder);
        release_held_media();
    }
    if (SegmentsArgs segments{}; take(p_segments, segments)) {
        apply_media_segments(segments.items, segments.count);
    }
    if (bool seeks = false; take(p_media_seeks, seeks)) {
        apply_media_seeks(seeks);
    }
    if (const void *still = nullptr; take(p_still, still)) {
        apply_cinema_still(still);
    }
    for (int i = 0; i < media::kPickCount; ++i) {
        if (PickArgs pick{}; take(p_pick[i], pick)) {
            apply_pick(i, pick.name.get());
        }
        if (const void *pixels = nullptr; take(p_pick_art[i], pixels)) {
            apply_pick_art(i, pixels);
        }
    }
    if (ProgressArgs progress{}; take(p_progress, progress)) {
        apply_media_progress(progress.position_s, progress.duration_s, progress.playing);
    }
    if (int volume = 0; take(p_media_volume, volume)) {
        apply_media_volume(volume);
    }
    if (int hold = 0; take(p_media_hold, hold)) {
        apply_media_hold(hold);
    }
}

void apply_home_updates()
{
    for (int i = 0; i < kPillCount; ++i) {
        if (PillArgs pill{}; take(p_pill[i], pill)) {
            apply_pill(i, pill.label.get(), pill.value.get(), pill.level);
        }
    }
    if (LightsArgs lights{}; take(p_lights, lights)) {
        apply_lights(lights.label.get(), lights.state.get(), lights.on);
    }
    for (int i = 0; i < kLightCount; ++i) {
        if (LightArgs light{}; take(p_light[i], light)) {
            apply_light(i, light.name.get(), light.state.get(), light.on);
        }
    }
    for (int i = 0; i < kDialToggleCount; ++i) {
        if (ToggleArgs toggle{}; take(p_toggle[i], toggle)) {
            apply_dial_toggle(i, toggle.label.get(), toggle.on);
        }
    }
    if (RangeArgs range{}; take(p_range, range)) {
        apply_thermostat_range(range.min_c, range.max_c, range.step_c);
    }
    if (ThermostatArgs thermostat{}; take(p_thermostat, thermostat)) {
        apply_thermostat(thermostat.current_c, thermostat.target_c, thermostat.mode.get(),
                         thermostat.state);
    }
}

void apply_rail_updates()
{
    if (TimeText time{}; take(p_time, time)) {
        apply_time(time.get());
    }
    if (bool wifi = false; take(p_wifi, wifi)) {
        apply_wifi(wifi);
    }
    if (int volume = 0; take(p_notification_volume, volume)) {
        apply_notification_volume(volume);
    }
}

void apply_diagnostics_updates()
{
    for (int c = 0; c < s_card_count; ++c) {
        if (InfoArgs card{}; take(p_card[c], card)) {
            apply_card(c, card.value.get(), card.level);
        }
        for (int r = 0; r < s_cards[c].row_count && r < kMaxRows; ++r) {
            if (InfoArgs row{}; p_rows != nullptr && take(p_rows[c * kMaxRows + r], row)) {
                apply_row(c, r, row.value.get(), row.level);
            }
        }
    }
}

void apply_glances()
{
    for (int i = 0; i < kGlanceCount; ++i) {
        if (Text<24> value{}; take(p_glance[i], value)) {
            apply_glance(i, value.get());
        }
    }
}

void apply_page_updates()
{
    if (Focus focus{}; take(p_focus, focus)) {
        show_focus(focus);
    }
    if (UpdateState update{}; take(p_update, update)) {
        paint_update_icon(update);
        // Only once all that arrives is in and checked, not while any still comes.
        paint_setup_dot((update.panel_ready || update.companion_ready) &&
                        update.busy == UpdateTarget::None);
        paint_update_tile(update);
    }
    if (bool calendar = false; take(p_calendar, calendar)) {
        show_calendar();
    }
    if (bool radar = false; take(p_radar, radar)) {
        refresh_radar();
    }
    static DetailsArgs details;
    if (take(p_details, details)) {
        show_radar_details(details.hex, details.details);
    }
    if (PhotoArgs photo{}; take(p_photo, photo)) {
        show_radar_photo(photo.hex, photo.pixels, photo.width, photo.height);
    }
}

void apply_inbox()
{
    static Notice notices[NOTICE_INBOX_LEN];
    portENTER_CRITICAL(&s_pending_lock);
    const int count = s_inbox_count;
    for (int i = 0; i < count; ++i) {
        notices[i] = s_inbox[i];
    }
    s_inbox_count = 0;
    portEXIT_CRITICAL(&s_pending_lock);
    for (int i = 0; i < count; ++i) {
        apply_notice(notices[i]);
    }
}

void apply_pending(lv_timer_t *)
{
    if (!s_pending.exchange(false, std::memory_order_acquire)) {
        return;
    }
    apply_splash();
    // Settings and presence first: they decide which pages and presets show.
    apply_settings_and_presence();
    apply_desk_updates();
    apply_media_updates();
    apply_home_updates();
    apply_rail_updates();
    apply_diagnostics_updates();
    apply_glances();
    apply_page_updates();
    apply_inbox();
}
}  // namespace

void detail::request_apply()
{
    s_pending.store(true, std::memory_order_release);
}

esp_err_t set_preset_active(int index, bool active)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < kPresetCount, ESP_ERR_INVALID_ARG, TAG, "preset %d",
                        index);
    put(p_preset_active[index], active);
    return ESP_OK;
}

esp_err_t set_height(int height_mm)
{
    put(p_height, height_mm);
    return ESP_OK;
}

esp_err_t set_desk_available(bool available)
{
    put(p_desk_available, available);
    return ESP_OK;
}

esp_err_t set_media(const char *source, const char *title, const char *artist, const char *state,
                    bool playing, bool controllable, bool art_coming)
{
    static MediaArgs args;  // too large to build on a caller's stack
    portENTER_CRITICAL(&s_pending_lock);
    args.source.set(source);
    args.title.set(title);
    args.artist.set(artist);
    args.state.set(state);
    args.playing      = playing;
    args.controllable = controllable;
    args.art_coming   = art_coming;
    p_media.value     = args;
    p_media.dirty = true;
    portEXIT_CRITICAL(&s_pending_lock);
    s_pending.store(true, std::memory_order_release);
    return ESP_OK;
}

esp_err_t set_media_progress(int position_s, int duration_s, bool playing)
{
    put(p_progress, ProgressArgs{position_s, duration_s, playing});
    return ESP_OK;
}

esp_err_t set_media_volume(int percent)
{
    put(p_media_volume, percent);
    return ESP_OK;
}

esp_err_t set_album_art(const void *pixels, bool placeholder)
{
    put(p_art, ArtArgs{pixels, placeholder});
    return ESP_OK;
}

esp_err_t set_media_segments(const MediaSegment *segments, int count)
{
    SegmentsArgs args{};
    args.count = std::clamp(count, 0, kMaxSegments);
    for (int i = 0; i < args.count; ++i) {
        args.items[i] = segments[i];
    }
    put(p_segments, args);
    return ESP_OK;
}

esp_err_t set_cinema_still(const void *pixels)
{
    put(p_still, pixels);
    return ESP_OK;
}

esp_err_t set_media_seeks(bool seeks)
{
    put(p_media_seeks, seeks);
    return ESP_OK;
}

esp_err_t set_pick(int index, const char *name)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < media::kPickCount, ESP_ERR_INVALID_ARG, TAG,
                        "favourite %d", index);
    PickArgs args{};
    args.name.set(name);
    put(p_pick[index], args);
    return ESP_OK;
}

esp_err_t set_pick_art(int index, const void *pixels)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < media::kPickCount, ESP_ERR_INVALID_ARG, TAG,
                        "favourite %d", index);
    put(p_pick_art[index], pixels);
    return ESP_OK;
}

esp_err_t set_pill(int index, const char *label, const char *value, Level level)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < kPillCount, ESP_ERR_INVALID_ARG, TAG, "pill %d",
                        index);
    PillArgs args{};
    args.label.set(label);
    args.value.set(value);
    args.level = level;
    put(p_pill[index], args);
    return ESP_OK;
}

esp_err_t set_lights(const char *label, const char *state, bool on)
{
    LightsArgs args{};
    args.label.set(label);
    args.state.set(state);
    args.on = on;
    put(p_lights, args);
    return ESP_OK;
}

esp_err_t set_light(int index, const char *name, const char *state, bool on)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < kLightCount, ESP_ERR_INVALID_ARG, TAG, "light %d",
                        index);
    LightArgs args{};
    args.name.set(name);
    args.state.set(state);
    args.on = on;
    put(p_light[index], args);
    return ESP_OK;
}

esp_err_t set_dial_toggle(int index, const char *label, bool on)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < kDialToggleCount, ESP_ERR_INVALID_ARG, TAG,
                        "toggle %d", index);
    ToggleArgs args{};
    args.label.set(label);
    args.on = on;
    put(p_toggle[index], args);
    return ESP_OK;
}

esp_err_t set_thermostat_range(float min_c, float max_c, float step_c)
{
    put(p_range, RangeArgs{min_c, max_c, step_c});
    return ESP_OK;
}

esp_err_t set_thermostat(float current_c, float target_c, const char *mode, Hvac state)
{
    ThermostatArgs args{};
    args.current_c = current_c;
    args.target_c  = target_c;
    args.mode.set(mode);
    args.state = state;
    put(p_thermostat, args);
    return ESP_OK;
}

esp_err_t set_presence(bool has_key, bool present, bool ever_seen)
{
    put(p_presence, PresenceArgs{has_key, present, ever_seen});
    return ESP_OK;
}

esp_err_t set_time(const char *text)
{
    TimeText args{};
    args.set(text);
    put(p_time, args);
    return ESP_OK;
}

esp_err_t set_links(bool wifi, bool mqtt)
{
    (void)mqtt;  // the broker has its own indicator in Home Assistant
    put(p_wifi, wifi);
    return ESP_OK;
}

esp_err_t set_battery(bool present, int percent, bool charging)
{
    (void)present;
    (void)percent;
    (void)charging;
    return ESP_OK;
}

void set_cards(const Card *cards, int count)
{
    s_cards      = cards;
    s_card_count = std::min(count, kMaxCards);
    if (p_rows == nullptr) {
        p_rows = static_cast<Slot<InfoArgs> *>(heap_caps_calloc(
            kMaxCards * kMaxRows, sizeof(Slot<InfoArgs>), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
}

esp_err_t set_card(int card, const char *summary, Level level)
{
    ESP_RETURN_ON_FALSE(card >= 0 && card < s_card_count, ESP_ERR_INVALID_ARG, TAG, "card %d",
                        card);
    InfoArgs args{};
    args.value.set(summary);
    args.level = level;
    put(p_card[card], args);
    return ESP_OK;
}

esp_err_t set_glance(Glance which, const char *value)
{
    const int index = static_cast<int>(which);
    ESP_RETURN_ON_FALSE(index >= 0 && index < kGlanceCount, ESP_ERR_INVALID_ARG, TAG, "glance %d",
                        index);
    Text<24> text{};
    text.set(value);
    put(p_glance[index], text);
    return ESP_OK;
}

esp_err_t set_row(int card, int row, const char *value, Level level)
{
    ESP_RETURN_ON_FALSE(card >= 0 && card < s_card_count && row >= 0 &&
                            row < std::min(s_cards[card].row_count, kMaxRows),
                        ESP_ERR_INVALID_ARG, TAG, "card %d row %d", card, row);
    ESP_RETURN_ON_FALSE(p_rows != nullptr, ESP_ERR_NO_MEM, TAG, "rows");
    InfoArgs args{};
    args.value.set(value);
    args.level = level;
    put(p_rows[card * kMaxRows + row], args);
    return ESP_OK;
}

esp_err_t set_calendar()
{
    put(p_calendar, true);
    return ESP_OK;
}

esp_err_t set_focus(const Focus &focus)
{
    put(p_focus, focus);
    return ESP_OK;
}

esp_err_t set_update(const UpdateState &state)
{
    put(p_update, state);
    return ESP_OK;
}

esp_err_t set_radar(const radar::Snapshot &snapshot)
{
    (void)snapshot;  // too big to copy here; the page takes radar's own copy
    put(p_radar, true);
    return ESP_OK;
}

esp_err_t set_radar_details(const char *hex, const radar::Details &details)
{
    static DetailsArgs args;
    portENTER_CRITICAL(&s_pending_lock);
    copy_text(args.hex, sizeof(args.hex), hex);
    args.details    = details;
    p_details.value = args;
    p_details.dirty = true;
    portEXIT_CRITICAL(&s_pending_lock);
    s_pending.store(true, std::memory_order_release);
    return ESP_OK;
}

esp_err_t set_radar_photo(const char *hex, const void *pixels, int width, int height)
{
    PhotoArgs args{};
    copy_text(args.hex, sizeof(args.hex), hex);
    args.pixels = pixels;
    args.width  = width;
    args.height = height;
    put(p_photo, args);
    return ESP_OK;
}

esp_err_t set_media_hold_preset(int preset)
{
    put(p_media_hold, preset);
    return ESP_OK;
}

esp_err_t set_notification_volume(int percent)
{
    put(p_notification_volume, percent);
    return ESP_OK;
}

esp_err_t set_screen(bool on)
{
    put(p_screen, on);
    return ESP_OK;
}

esp_err_t set_setting(Setting setting, bool on)
{
    const int index = static_cast<int>(setting);
    ESP_RETURN_ON_FALSE(index >= 0 && index < SETTING_COUNT, ESP_ERR_INVALID_ARG, TAG, "setting %d",
                        index);
    put(p_setting[index], on);
    return ESP_OK;
}

bool diagnostics_open()
{
    return s_setup_visible.load(std::memory_order_relaxed);
}

esp_err_t notify(const char *title, const char *message, Level level, int timeout_ms)
{
    portENTER_CRITICAL(&s_pending_lock);
    if (s_inbox_count == NOTICE_INBOX_LEN) {
        drop_oldest(s_inbox, s_inbox_count);
    }
    Notice &slot = s_inbox[s_inbox_count++];
    copy_text(slot.title, sizeof(slot.title), title);
    copy_text(slot.message, sizeof(slot.message), message);
    slot.level = level;
    slot.timeout_ms = timeout_ms;
    portEXIT_CRITICAL(&s_pending_lock);
    s_pending.store(true, std::memory_order_release);
    return ESP_OK;
}

esp_err_t init(const Handlers &handlers, int initial_brightness, std::uint32_t accent,
               bool rail_right, Orientation orientation)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    fonts::init();
    theme::init_accents();
    if (accent != 0) {
        theme::set_primary(accent);
    }
    s_rail_right         = rail_right;
    s_orientation        = orientation;
    s_handlers           = handlers;
    s_initial_brightness = initial_brightness;
    build_screen();
    lv_timer_create(apply_pending, APPLY_PERIOD_MS, nullptr);
    lv_refr_now(nullptr);
    lvgl_port_unlock();
    return ESP_OK;
}

}  // namespace ui
