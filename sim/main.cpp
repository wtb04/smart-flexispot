// The panel's screen in a window: components/ui as it is, the live feeds
// fetched for real, the hardware played by hardware.cpp. See README.md.
#include "hardware.h"
#include "home_assistant.h"
#include "live/live.h"
#include "media_stub.h"
#include "notices.h"
#include "updates.h"
#include "ui.h"
#include "ui_internal.h"

#include "esp_heap_caps.h"
#include "lvgl.h"

#include <SDL.h>
#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace {
constexpr int WIDTH  = 1280;
constexpr int HEIGHT = 720;

// As main/diagnostics.cpp lays out the Setup page's cards; their rows stay
// "--" but for what the simulator knows.
constexpr const char *WIFI_ROWS[]     = {"Network", "Signal", "Address", "Channel", "MAC"};
constexpr const char *HASS_ROWS[]     = {"Broker", "Socket", "Entities", "Cover art", "Last refused"};
constexpr const char *PRESENCE_ROWS[] = {"Phone", "Signal", "Identity key", "Radio"};
constexpr const char *DESK_ROWS[]     = {"Height", "At", "Moving", "Preset"};
constexpr const char *LINK_ROWS[]     = {"Driven over", "Control box", "Companion", "Round trip", "Last heard"};
constexpr const char *POWER_ROWS[]    = {"Source", "Charge", "Voltage", "Current", "State"};
constexpr const char *RADAR_ROWS[]    = {"Feed", "Planes", "Reach", "Last sweep"};
constexpr const char *CALENDAR_ROWS[] = {"Feeds", "Ahead", "Next", "Journey"};
constexpr const char *SYSTEM_ROWS[]   = {"Firmware", "Built", "Uptime", "Internal free",
                                         "Largest block", "PSRAM free", "Low mark", "JPEG decoder"};
#define ROWS(rows) rows, static_cast<int>(std::size(rows))
const ui::Card CARDS[] = {
    {"Wi-Fi", ui::Glyph::Wifi, ROWS(WIFI_ROWS)},
    {"Home Assistant", ui::Glyph::Home, ROWS(HASS_ROWS)},
    {"Presence", ui::Glyph::Presence, ROWS(PRESENCE_ROWS)},
    {"Desk", ui::Glyph::Desk, ROWS(DESK_ROWS)},
    {"Desk link", ui::Glyph::Link, ROWS(LINK_ROWS)},
    {"Power", ui::Glyph::Power, ROWS(POWER_ROWS)},
    {"Radar", ui::Glyph::Radar, ROWS(RADAR_ROWS)},
    {"Calendar", ui::Glyph::Calendar, ROWS(CALENDAR_ROWS)},
    {"System", ui::Glyph::System, ROWS(SYSTEM_ROWS)},
};

void said(const char *what)
{
    std::printf("I (sim) %s\n", what);
}

// A PNG of whatever is on screen, next to the simulator in shots/.
void save_screenshot()
{
    int            w = 0, h = 0;
    std::uint16_t *pixels = ui::capture(w, h);
    if (pixels == nullptr) {
        said("no screenshot");
        return;
    }
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            const std::uint16_t c = pixels[y * w + x];
            raw.push_back(static_cast<std::uint8_t>((c >> 11) << 3));
            raw.push_back(static_cast<std::uint8_t>(((c >> 5) & 0x3f) << 2));
            raw.push_back(static_cast<std::uint8_t>((c & 0x1f) << 3));
        }
    }
    heap_caps_free(pixels);
    uLongf                    packed_size = compressBound(raw.size());
    std::vector<std::uint8_t> packed(packed_size);
    compress2(packed.data(), &packed_size, raw.data(), raw.size(), 6);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    const auto chunk = [&png](const char *tag, const std::uint8_t *body, std::size_t size) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            png.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        const std::size_t at = png.size();
        png.insert(png.end(), tag, tag + 4);
        png.insert(png.end(), body, body + size);
        const uLong crc = crc32(0, png.data() + at, static_cast<uInt>(size + 4));
        for (int shift = 24; shift >= 0; shift -= 8) {
            png.push_back(static_cast<std::uint8_t>(crc >> shift));
        }
    };
    const std::uint8_t header[] = {0, 0, static_cast<std::uint8_t>(w >> 8), static_cast<std::uint8_t>(w),
                                   0, 0, static_cast<std::uint8_t>(h >> 8), static_cast<std::uint8_t>(h),
                                   8, 2, 0, 0, 0};
    chunk("IHDR", header, sizeof(header));
    chunk("IDAT", packed.data(), packed_size);
    chunk("IEND", nullptr, 0);

    char              name[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof(name), "shots/%Y%m%d-%H%M%S.png", std::localtime(&now));
    std::system("mkdir -p shots");
    if (FILE *file = std::fopen(name, "wb"); file != nullptr) {
        std::fwrite(png.data(), 1, png.size(), file);
        std::fclose(file);
        std::printf("I (sim) wrote %s\n", name);
    }
}

bool s_quit = false;

void toggle_help();

// Everything the keyboard does, in the order H lists it: the states that only
// the hardware and the services behind the panel can put it in.
struct Key {
    SDL_Keycode code;
    const char *name;
    const char *what;
    void (*act)();
};
const Key KEYS[] = {
    {SDLK_h, "H", "This list, and away again", toggle_help},
    {SDLK_a, "A", "Home Assistant answering, with states; again, gone", home_assistant::toggle},
    {SDLK_r, "R", "The air: good, some of it not, bad", home_assistant::next_air},
    {SDLK_m, "M", "What plays: nothing, music, a Jellyfin episode", media_stub::next_scene},
    {SDLK_n, "N", "A notice from Home Assistant, another each time", notices::next_example},
    {SDLK_t, "T", "The focus part under way runs out now", hardware::end_focus_part},
    {SDLK_u, "U", "An update: arriving, ready, the companion's, both, none", updates::next},
    {SDLK_d, "D", "The desk link lost, and back", hardware::toggle_desk_link},
    {SDLK_b, "B", "The battery: charging, on battery, low, none", hardware::next_battery},
    {SDLK_p, "P", "The phone away, and back", hardware::toggle_phone},
    {SDLK_w, "W", "Wi-Fi down, and back", hardware::toggle_wifi},
    {SDLK_s, "S", "A screenshot into sim/shots/", save_screenshot},
    {SDLK_ESCAPE, "Esc", "Quit", [] { s_quit = true; }},
};

// The list, over everything, notices and popups included, and in screenshots.
lv_obj_t *s_help = nullptr;

void build_help()
{
    constexpr std::int32_t W = 820, PAD = 32, ROW = 36, KEY_W = 90;
    const std::int32_t     h = PAD * 2 + 56 + ROW * static_cast<std::int32_t>(std::size(KEYS)) + 48;
    s_help = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_help, W, h);
    lv_obj_center(s_help);
    ui::theme::style_panel(s_help, ui::theme::panel_light, ui::theme::radius::card);
    lv_obj_set_style_pad_all(s_help, PAD, 0);
    lv_obj_set_scrollable(s_help, false);

    lv_obj_t *title = ui::theme::make_eyebrow(s_help, "SIMULATOR KEYS");
    lv_obj_set_pos(title, 0, 0);
    std::int32_t y = 56;
    for (const Key &key : KEYS) {
        lv_obj_t *name = ui::theme::make_label(s_help, key.name, ui::theme::primary, ui::fonts::size_22());
        lv_obj_set_pos(name, 0, y);
        lv_obj_t *what = ui::theme::make_label(s_help, key.what, ui::theme::text, ui::fonts::size_22());
        lv_obj_set_pos(what, KEY_W, y);
        y += ROW;
    }
    lv_obj_t *send = ui::theme::make_label(s_help, "sim/send.sh notify '<json>' sends any notice Home Assistant could",
                                           ui::theme::secondary, ui::fonts::size_20());
    lv_obj_set_pos(send, 0, y + 12);
    lv_obj_set_hidden(s_help, true);
}

void toggle_help()
{
    if (s_help == nullptr) {
        build_help();
    }
    lv_obj_set_hidden(s_help, !lv_obj_is_hidden(s_help));
    lv_obj_move_foreground(s_help);
}

void press(SDL_Keycode code)
{
    for (const Key &key : KEYS) {
        if (key.code == code) {
            key.act();
        }
    }
}

// A finger down and up again there, as the mouse would, a few frames apart.
void run_for(std::uint32_t ms)
{
    const std::uint32_t until = SDL_GetTicks() + ms;
    while (SDL_GetTicks() < until) {
        lv_timer_handler();
        SDL_Delay(5);
    }
}

void tap(SDL_Point at)
{
    // Addressed to the window, as LVGL only takes a window's own events.
    const Uint32 window = SDL_GetWindowID(lv_sdl_window_get_window(lv_display_get_default()));
    SDL_Event    event{};
    event.type            = SDL_MOUSEMOTION;
    event.motion.windowID = window;
    event.motion.x        = at.x;
    event.motion.y        = at.y;
    SDL_PushEvent(&event);
    for (const Uint32 type : {SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP}) {
        event                 = {};
        event.type            = type;
        event.button.windowID = window;
        event.button.button   = SDL_BUTTON_LEFT;
        event.button.x        = at.x;
        event.button.y        = at.y;
        SDL_PushEvent(&event);
        run_for(120);
    }
    run_for(300);
}

int on_event(void *, SDL_Event *event)
{
    if (event->type == SDL_QUIT) {
        s_quit = true;
    }
    if (event->type == SDL_KEYDOWN && event->key.repeat == 0) {
        press(event->key.keysym.sym);
    }
    return 0;
}

ui::Handlers handlers()
{
    ui::Handlers h{};
    h.move        = hardware::on_move;
    h.preset      = hardware::on_preset;
    h.focus       = hardware::on_focus;
    h.focus_plan  = hardware::on_focus_plan;
    h.radar       = [](bool showing, bool) { live::set_radar_showing(showing); };
    h.details     = live::look_up;
    h.media       = media_stub::on_media;
    h.seek        = media_stub::on_seek;
    h.media_volume = media_stub::on_volume;
    h.pick        = media_stub::on_pick;
    h.lights      = home_assistant::on_lights;
    h.light       = home_assistant::on_light;
    h.setpoint    = home_assistant::on_setpoint;
    h.mode        = home_assistant::on_mode;
    h.dial_toggle = home_assistant::on_dial_toggle;
    h.update_now  = updates::install;
    h.brightness  = [](int percent) { std::printf("I (sim) brightness %d%%\n", percent); };
    h.restart     = [] { said("restart asked for"); };
    h.screen      = [](bool on) { said(on ? "screen on" : "screen off"); };
    h.setting     = [](ui::Setting setting, bool on) {
        std::printf("I (sim) setting %d %s\n", static_cast<int>(setting), on ? "on" : "off");
    };
    return h;
}

// Without --splash, LVGL's clock runs this far ahead of SDL's, which the splash
// takes for its twelve seconds having gone by.
constexpr std::uint32_t SPLASH_SKIP_MS = 13000;

std::uint32_t ticks_past_splash()
{
    return SDL_GetTicks() + SPLASH_SKIP_MS;
}

// --page N opens page N; --press KEYS presses those keys, as "am" for Home
// Assistant answering and music; --tap X,Y taps there, as often as given;
// --shot S saves a screenshot after S seconds and quits; --splash plays the
// twelve seconds of splash the panel boots with.
struct Options {
    int         page   = -1;
    int         shot_s = -1;
    bool        splash = false;
    std::string keys;
    std::vector<SDL_Point> taps;
};

Options options(int argc, char **argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string name  = argv[i];
        const char       *value = i + 1 < argc ? argv[i + 1] : "0";
        if (name == "--page") {
            o.page = std::atoi(value);
            ++i;
        } else if (name == "--shot") {
            o.shot_s = std::atoi(value);
            ++i;
        } else if (name == "--press") {
            o.keys = value;
            ++i;
        } else if (name == "--tap") {
            SDL_Point at{};
            if (std::sscanf(value, "%d,%d", &at.x, &at.y) == 2) {
                o.taps.push_back(at);
            }
            ++i;
        } else if (name == "--splash") {
            o.splash = true;
        }
    }
    return o;
}
}  // namespace

int main(int argc, char **argv)
{
    const Options opts = options(argc, argv);
    live::init_http();
    lv_init();
    lv_display_t *display = lv_sdl_window_create(WIDTH, HEIGHT);
    lv_sdl_window_set_title(display, "Smart Flexispot");
    if (const char *zoom = std::getenv("SIM_ZOOM"); zoom != nullptr) {
        lv_sdl_window_set_zoom(display, static_cast<float>(std::atof(zoom)));
    }
    lv_sdl_mouse_create();
    SDL_AddEventWatch(on_event, nullptr);

    ui::set_cards(CARDS, static_cast<int>(std::size(CARDS)));
    ESP_ERROR_CHECK(ui::init(handlers(), 80, 0, false, ui::Orientation::Normal));
    // The settings as a panel fresh from the factory has them, from settings.cpp.
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_setting(ui::Setting::Charging, true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_setting(ui::Setting::PresenceGate, true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_setting(ui::Setting::DeskBluetooth, true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_notification_volume(60));
    home_assistant::start();  // not answering yet: the page as the panel has it until it does
    hardware::start();
    media_stub::start();
    live::start();
    notices::listen();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("desk"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("network"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_done());
    if (!opts.splash) {
        lv_tick_set_cb(ticks_past_splash);
    }

    run_for(2000);  // what start-up set, the presence gate and the phone among it, applied
    for (const char key : opts.keys) {
        press(key == 'x' ? SDLK_ESCAPE : static_cast<SDL_Keycode>(key));  // letters are their own keycodes
    }
    // Asked for until it is up: until the phone is seen, the presence gate
    // leaves only Home and Setup to open.
    for (std::uint32_t until = SDL_GetTicks() + 5000; opts.page >= 0 && ui::detail::s_page != opts.page &&
                                                      SDL_GetTicks() < until;) {
        ui::detail::select_page(opts.page);
        run_for(50);
    }
    for (const SDL_Point &at : opts.taps) {
        tap(at);
    }

    const std::uint32_t shot_at = opts.shot_s >= 0 ? SDL_GetTicks() + opts.shot_s * 1000u : 0;
    while (!s_quit) {
        if (shot_at != 0 && SDL_GetTicks() >= shot_at) {
            save_screenshot();
            break;
        }
        live::pump();
        notices::pump();
        hardware::tick();
        media_stub::tick();
        updates::tick();
        const std::uint32_t idle = lv_timer_handler();
        SDL_Delay(idle < 5 ? idle : 5);
    }
    return 0;
}
