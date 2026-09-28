// The panel's screen in a window: components/ui as it is, the live feeds
// fetched for real, the hardware played by hardware.cpp. See README.md.
#include "hardware.h"
#include "live/live.h"
#include "room_layout.h"
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

// Keys as well as the mouse: the hardware's states, which nothing on the
// screen can change, and a screenshot.
int on_event(void *, SDL_Event *event)
{
    if (event->type == SDL_QUIT) {
        s_quit = true;
    }
    if (event->type != SDL_KEYDOWN || event->key.repeat != 0) {
        return 0;
    }
    switch (event->key.keysym.sym) {
        case SDLK_d:      hardware::toggle_desk_link(); break;
        case SDLK_b:      hardware::next_battery(); break;
        case SDLK_p:      hardware::toggle_phone(); break;
        case SDLK_w:      hardware::toggle_wifi(); break;
        case SDLK_s:      save_screenshot(); break;
        case SDLK_ESCAPE: s_quit = true; break;
        default:          break;
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
    h.brightness  = [](int percent) { std::printf("I (sim) brightness %d%%\n", percent); };
    h.restart     = [] { said("restart asked for"); };
    h.update_now  = [] { said("update asked for"); };
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

// --page N opens page N; --shot S saves a screenshot after S seconds and quits;
// --splash plays the twelve seconds of splash the panel boots with.
struct Options {
    int  page   = -1;
    int  shot_s = -1;
    bool splash = false;
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
    room::show_unknown();  // as the panel shows it until Home Assistant answers
    hardware::start();
    live::start();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("desk"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("network"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_done());
    if (!opts.splash) {
        lv_tick_set_cb(ticks_past_splash);
    }

    if (opts.page >= 0) {
        // Once what was set above has been applied: until the phone is seen,
        // the presence gate leaves only Home and Setup to open.
        const std::uint32_t settled = SDL_GetTicks() + 300;
        while (SDL_GetTicks() < settled) {
            lv_timer_handler();
            SDL_Delay(5);
        }
        ui::detail::select_page(opts.page);
    }

    const std::uint32_t shot_at = opts.shot_s >= 0 ? SDL_GetTicks() + opts.shot_s * 1000u : 0;
    while (!s_quit) {
        if (shot_at != 0 && SDL_GetTicks() >= shot_at) {
            save_screenshot();
            break;
        }
        live::pump();
        hardware::tick();
        const std::uint32_t idle = lv_timer_handler();
        SDL_Delay(idle < 5 ? idle : 5);
    }
    return 0;
}
