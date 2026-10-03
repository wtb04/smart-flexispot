#include "ui_internal.h"
#include "focus_model.h"
#include "focus_view.h"
#include "diagnostics_model.h"
#include "home_model.h"
#include "notices_model.h"
#include "radar_model.h"
#include "room_model.h"
#include "settings_model.h"
#include "status_model.h"
#include "topics.h"

#ifndef SHOT_DESK_CARD
#define SHOT_DESK_CARD 0
#endif
#ifndef SHOT_ENABLED
#define SHOT_ENABLED 0
#endif

namespace ui::detail {
namespace {
constexpr std::uint32_t APPLY_PERIOD_MS = 20;

constexpr int DEFAULT_BRIGHTNESS_PERCENT = 80;

constexpr std::uint32_t SHOT_START_MS = 25000;
constexpr std::uint32_t SHOT_PAGE_MS  = 2000;
}  // namespace

bool s_dock_right = true;

Orientation s_orientation = Orientation::Normal;

Layout layout()
{
    lv_display_t      *disp = lv_display_get_default();
    const std::int32_t w    = lv_display_get_horizontal_resolution(disp);
    const std::int32_t h    = lv_display_get_vertical_resolution(disp);
    return Layout{w,
                  h,
                  s_dock_right ? GAP : GAP + DOCK_W + GAP,
                  CONTENT_Y,
                  w - DOCK_W - 3 * GAP,
                  h - CONTENT_Y - GAP,
                  s_dock_right ? w - GAP - DOCK_W : GAP,
                  s_dock_right};
}

Handlers s_handlers{};
namespace {
// The two move buttons and a button per preset, stand and sit among them.
constexpr int MOVE_BUTTON_COUNT = 2;
constexpr int DESK_CONTROL_MAX  = MOVE_BUTTON_COUNT + kPresetCount + kDeskShortcutButtons;

lv_obj_t     *s_desk_controls[DESK_CONTROL_MAX] = {};
int           s_desk_control_count              = 0;
}  // namespace


bool s_notice_lit_screen = false;

void set_screen_state(bool on)
{
    if (on == status_state().screen_on || s_handlers.screen == nullptr) {
        return;
    }
    status_state().screen_on = on;
    publish(Topic::Status);
    s_handlers.screen(on);
}
namespace {
void wake_on_touch(lv_event_t *)
{
    s_notice_lit_screen = false;
    if (status_state().screen_on) {
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

// Faded while the desk does not answer.
void register_desk_control(lv_obj_t *obj)
{
    if (s_desk_control_count == 0) {
        subscribe(Topic::Desk, kNoView, [] {
            for (int i = 0; i < s_desk_control_count; ++i) {
                theme::set_usable(s_desk_controls[i], desk_state().available);
            }
        });
    }
    if (s_desk_control_count < DESK_CONTROL_MAX) {
        s_desk_controls[s_desk_control_count++] = obj;
        theme::set_usable(obj, desk_state().available);
    }
}
int               s_initial_brightness = DEFAULT_BRIGHTNESS_PERCENT;

lv_obj_t *s_dock          = nullptr;  // the dock
lv_obj_t *s_content       = nullptr;

void place_for_side()
{
    const Layout l = layout();
    lv_obj_set_x(s_dock, l.dock_x);
    lv_obj_set_pos(s_content, l.content_x, l.content_y);
    place_desk_card();
    place_top_bar();
    place_notice();
}
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
        if (SHOT_DESK_CARD) {
            open_desk_card(true);
        }
        lv_timer_set_period(timer, SHOT_PAGE_MS);
        return;
    }
    s_presence_gate = gated;
    select_page(HOME_PAGE);
    lv_timer_delete(timer);
}

void prepare_screen()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(theme::background), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollable(scr, false);

    for (lv_indev_t *dev = lv_indev_get_next(nullptr); dev != nullptr;
         dev = lv_indev_get_next(dev)) {
        lv_indev_add_event_cb(dev, wake_on_touch, LV_EVENT_PRESSED, nullptr);
    }
}

// Behind the splash, one part at a time, laid out before the next, so that
// between parts the splash moves on. Each is hidden until the splash leaves.
bool build_next_part()
{
    static int next = 0;
    lv_obj_t  *scr  = lv_screen_active();
    switch (next++) {
        case 0:
            create_dock(scr);
            create_content(scr);
            create_top_bar(scr);
            return true;
        case 1:
            if (build_next_page()) {
                --next;
            }
            return true;
        case 2:
            // Development: hands a picture of the screen to tools/screenshot.py a
            // little after boot. It holds the LVGL lock for several seconds, so
            // it is off unless wanted.
            if (SHOT_ENABLED) {
                lv_timer_create(take_screenshots, SHOT_START_MS, nullptr);
            }
            create_desk_card(scr);
            build_cinema(scr);
            build_music(scr);
            build_favourites(scr);
            build_focus_full(scr);
            create_notice_card();
            follow_pages();
            return true;
        default:
            return false;
    }
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
struct BatteryArgs {
    bool present;
    int  percent;
    bool charging;
    bool on_battery;
};
struct ArtArgs {
    const void *pixels;
    bool        placeholder;
    int         width;
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
Slot<bool>           p_media_remote;
Slot<std::uint8_t>   p_subtitles;  // 1 for some to show, 2 for shown
Slot<const void *>   p_still;
Slot<const void *>   p_art_large;
Slot<std::uint8_t>   p_neighbours;  // bit 0 an episode before, bit 1 one after
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
Slot<BatteryArgs>    p_battery;
Slot<bool>           p_radar;
Slot<bool>           p_claude;
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


void apply_preset_active(int index, bool active)
{
    desk_take_active(index, active);
}

void apply_height(int height_mm)
{
    desk_take_height(height_mm);
}

void apply_desk_available(bool available)
{
    desk_take_available(available);
}

void apply_media_args(const MediaArgs &media)
{
    media_take_track(media.source.get(), media.title.get(), media.artist.get(), media.state.get(),
                     media.playing, media.controllable);
}

// A new track's text waits here for its cover, or for this long when that is slow.
constexpr std::uint32_t ART_WAIT_MS = 3 * units::kMsPerSecond;
MediaArgs               s_held_media;
bool                    s_media_held = false;
lv_timer_t             *s_held_timer = nullptr;
// And its progress with it, so the title, the cover and how far it is change
// together, rather than the times running ahead under the last track's title.
ProgressArgs            s_held_progress{};
bool                    s_progress_held = false;

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
        if (std::exchange(s_progress_held, false)) {
            media_take_progress(s_held_progress.position_s, s_held_progress.duration_s, s_held_progress.playing);
        }
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

void apply_pill(int index, const char *label, const char *value, Level level)
{
    PillState &pill = home_state().pills[index];
    copy_text(pill.label, sizeof(pill.label), label);
    copy_text(pill.value, sizeof(pill.value), value);
    pill.level = level;
    publish(Topic::Home);
}

void apply_lights(const char *label, const char *state, bool on)
{
    lights_take(label, state, on);
}

void apply_light(int index, const char *name, const char *state, bool on)
{
    lights_take_light(index, name, state, on);
}

void apply_dial_toggle(int index, const char *label, bool on)
{
    ToggleState &toggle = home_state().toggles[index];
    copy_text(toggle.label, sizeof(toggle.label), label);
    toggle.on = on;
    publish(Topic::Home);
}

void apply_thermostat_range(float min_c, float max_c, float step_c)
{
    ThermostatState &thermostat = home_state().thermostat;
    thermostat.ranged = true;
    thermostat.min_c  = min_c;
    thermostat.max_c  = max_c;
    thermostat.step_c = step_c;
    publish(Topic::Home);
}

void apply_thermostat(float current_c, float target_c, const char *mode, Hvac state)
{
    ThermostatState &thermostat = home_state().thermostat;
    thermostat.known     = true;
    thermostat.current_c = current_c;
    thermostat.target_c  = target_c;
    copy_text(thermostat.mode, sizeof(thermostat.mode), mode);
    thermostat.state = state;
    publish(Topic::Home);
}

void apply_presence(bool has_key, bool present, bool ever_seen)
{
    // A phone never seen is not here: the owner's pages stay hidden and the
    // guest presets show until it is.
    (void)ever_seen;
    status_state().present = has_key && present;
    publish(Topic::Status);
}

void apply_time(const char *text)
{
    std::snprintf(status_state().time, sizeof(status_state().time), "%s", text != nullptr ? text : "");
    publish(Topic::Status);
}

void apply_wifi(bool wifi)
{
    status_state().wifi = wifi;
    publish(Topic::Status);
}

void apply_screen(bool on)
{
    status_state().screen_on = on;
    s_notice_lit_screen      = false;
    publish(Topic::Status);
}

void apply_settings_and_presence()
{
    bool on = false;
    for (int i = 0; i < SETTING_COUNT; ++i) {
        if (take(p_setting[i], on)) {
            settings_take(i, on);
        }
    }
    if (PresenceArgs presence{}; take(p_presence, presence)) {
        apply_presence(presence.has_key, presence.present, presence.ever_seen);
    }
    if (bool screen = false; take(p_screen, screen)) {
        apply_screen(screen);
    }
    if (BatteryArgs battery{}; take(p_battery, battery)) {
        status_take_battery(battery.present, battery.percent, battery.charging, battery.on_battery);
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
            if (std::exchange(s_progress_held, false)) {
                media_take_progress(s_held_progress.position_s, s_held_progress.duration_s, s_held_progress.playing);
            }
        }
    }
    if (ArtArgs art{}; take(p_art, art)) {
        media_take_cover(art.pixels, art.placeholder, art.width);
        release_held_media();
    }
    if (SegmentsArgs segments{}; take(p_segments, segments)) {
        media_take_segments(segments.items, segments.count);
    }
    if (bool remote = false; take(p_media_remote, remote)) {
        media_take_remote(remote);
    }
    if (bool seeks = false; take(p_media_seeks, seeks)) {
        media_take_video(seeks);
    }
    if (std::uint8_t subtitles = 0; take(p_subtitles, subtitles)) {
        media_take_subtitles((subtitles & 1) != 0, (subtitles & 2) != 0);
    }
    if (const void *still = nullptr; take(p_still, still)) {
        media_take_still(still);
    }
    if (const void *large = nullptr; take(p_art_large, large)) {
        media_take_large_cover(large);
    }
    if (std::uint8_t around = 0; take(p_neighbours, around)) {
        media_take_neighbours((around & 1) != 0, (around & 2) != 0);
    }
    for (int i = 0; i < media::kPickCount; ++i) {
        if (PickArgs pick{}; take(p_pick[i], pick)) {
            media_take_pick(i, pick.name.get());
        }
        if (const void *pixels = nullptr; take(p_pick_art[i], pixels)) {
            media_take_pick_art(i, pixels);
        }
    }
    if (ProgressArgs progress{}; take(p_progress, progress)) {
        if (s_media_held) {
            s_held_progress = progress;
            s_progress_held = true;
        } else {
            media_take_progress(progress.position_s, progress.duration_s, progress.playing);
        }
    }
    if (int volume = 0; take(p_media_volume, volume)) {
        media_take_volume(volume);
    }
    if (int hold = 0; take(p_media_hold, hold)) {
        media_take_hold_preset(hold);
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

void apply_status_updates()
{
    if (TimeText time{}; take(p_time, time)) {
        apply_time(time.get());
    }
    if (bool wifi = false; take(p_wifi, wifi)) {
        apply_wifi(wifi);
    }
    if (int volume = 0; take(p_notification_volume, volume)) {
        settings_take_volume(volume);
    }
}

void apply_diagnostics_updates()
{
    for (int c = 0; c < s_card_count; ++c) {
        if (InfoArgs card{}; take(p_card[c], card)) {
            diagnostics_take_card(c, card.value.get(), card.level);
        }
        for (int r = 0; r < s_cards[c].row_count && r < kMaxRows; ++r) {
            if (InfoArgs row{}; p_rows != nullptr && take(p_rows[c * kMaxRows + r], row)) {
                diagnostics_take_row(c, r, row.value.get(), row.level);
            }
        }
    }
}

void apply_glances()
{
    for (int i = 0; i < kGlanceCount; ++i) {
        if (Text<24> value{}; take(p_glance[i], value)) {
            diagnostics_take_glance(i, value.get());
        }
    }
}

void apply_page_updates()
{
    if (Focus focus{}; take(p_focus, focus)) {
        detail::set_focus_state(focus);
    }
    if (UpdateState update{}; take(p_update, update)) {
        settings_take_update(update);
    }
    if (bool calendar = false; take(p_calendar, calendar)) {
        publish(Topic::Calendar);
    }
    if (bool radar = false; take(p_radar, radar)) {
        publish(Topic::Radar);
    }
    if (bool claude = false; take(p_claude, claude)) {
        publish(Topic::Claude);
    }
    static DetailsArgs details;
    if (take(p_details, details)) {
        radar_take_details(details.hex, details.details);
    }
    if (PhotoArgs photo{}; take(p_photo, photo)) {
        radar_take_photo(photo.hex, photo.pixels, photo.width, photo.height);
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
        notices_take(notices[i]);
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
    apply_status_updates();
    apply_diagnostics_updates();
    apply_glances();
    apply_page_updates();
    apply_inbox();
    detail::deliver_topics();  // what follows a topic, once for all that changed
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

esp_err_t set_album_art(const void *pixels, bool placeholder, int width)
{
    put(p_art, ArtArgs{pixels, placeholder, width > 0 ? std::min(width, media::kArtSize) : media::kArtSize});
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

esp_err_t set_media_neighbours(bool previous, bool next)
{
    put(p_neighbours, static_cast<std::uint8_t>((previous ? 1 : 0) | (next ? 2 : 0)));
    return ESP_OK;
}

esp_err_t set_album_art_large(const void *pixels)
{
    put(p_art_large, pixels);
    return ESP_OK;
}

esp_err_t set_cinema_still(const void *pixels)
{
    put(p_still, pixels);
    return ESP_OK;
}

esp_err_t set_media_remote(bool remote)
{
    put(p_media_remote, remote);
    return ESP_OK;
}

esp_err_t set_media_seeks(bool seeks)
{
    put(p_media_seeks, seeks);
    return ESP_OK;
}

esp_err_t set_media_subtitles(bool available, bool shown)
{
    put(p_subtitles, static_cast<std::uint8_t>((available ? 1 : 0) | (shown ? 2 : 0)));
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

esp_err_t set_battery(bool present, int percent, bool charging, bool on_battery)
{
    put(p_battery, BatteryArgs{present, percent, charging, on_battery});
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

esp_err_t set_claude()
{
    put(p_claude, true);
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

esp_err_t notify(const char *source, const char *title, const char *message, Level level,
                 int timeout_ms)
{
    portENTER_CRITICAL(&s_pending_lock);
    if (s_inbox_count == NOTICE_INBOX_LEN) {
        drop_oldest(s_inbox, s_inbox_count);
    }
    Notice &slot = s_inbox[s_inbox_count++];
    copy_text(slot.source, sizeof(slot.source), source);
    copy_text(slot.title, sizeof(slot.title), title);
    copy_text(slot.message, sizeof(slot.message), message);
    slot.level = level;
    slot.timeout_ms = timeout_ms;
    portEXIT_CRITICAL(&s_pending_lock);
    s_pending.store(true, std::memory_order_release);
    return ESP_OK;
}

esp_err_t init(const Handlers &handlers, int initial_brightness, std::uint32_t accent,
               bool dock_right, Orientation orientation)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    fonts::init();
    theme::init_accents();
    if (accent != 0) {
        theme::set_primary(accent);
    }
    s_dock_right         = dock_right;
    s_orientation        = orientation;
    s_handlers           = handlers;
    s_initial_brightness = initial_brightness;
    prepare_screen();
    build_splash();
    lv_refr_now(nullptr);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t build()
{
    constexpr TickType_t BETWEEN_PARTS = pdMS_TO_TICKS(20);
    const std::uint32_t  from          = lv_tick_get();
    for (bool more = true; more;) {
        ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
        more = build_next_part();
        keep_under_splash();
        lv_obj_update_layout(lv_screen_active());
        if (!more) {
            lv_timer_create(apply_pending, APPLY_PERIOD_MS, nullptr);
        }
        lvgl_port_unlock();
        vTaskDelay(BETWEEN_PARTS);
    }
    ESP_LOGI(TAG, "built behind the splash in %u ms", static_cast<unsigned>(lv_tick_elaps(from)));
    return ESP_OK;
}

}  // namespace ui
