#include "network.h"

#include "ota.h"

#include "board.h"
#include "desk.h"
#include "esp_check.h"
#include "ble.h"
#include "jpeg.h"
#include "media.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "app_state.h"
#include "ha_ws.h"
#include "jobs.h"
#include "jellyfin.h"
#include "hass.h"
#include "travel.h"
#include "ical.h"
#include "power.h"
#include "net.h"
#include "radar.h"
#include "room.h"
#include "settings.h"
#include "sound.h"
#include "ui.h"
#include "units.h"
#include "wifi.h"

#include <atomic>
#include <cstdio>
#include <ctime>

namespace network {
namespace {
constexpr char TAG[] = "network";

constexpr int PUBLISH_MS      = 2000;
constexpr int NETWORK_WAIT_MS = 30 * units::kMsPerSecond;
// The splash waits on the links, so at startup they are looked at this often
// rather than at the next publish, for as long as LINKS_WAIT_MS.
constexpr int LOOK_EVERY_MS = 100;
constexpr int LINKS_WAIT_MS = 10 * units::kMsPerSecond;

constexpr int         REFUSAL_NOTICE_MS    = 4000;
constexpr std::size_t REFUSAL_MESSAGE_SIZE = 96;

constexpr float MILLIAMPS_PER_AMP = 1000.0f;

// Once boot has settled, the heaps are reported once, as a baseline.
constexpr std::int64_t HEAP_BASELINE_AFTER_US = 40 * units::kUsPerSecond;

constexpr std::size_t  LOW_DMA_BYTES        = 24 * units::kBytesPerKiB;
constexpr std::int64_t LOW_MEMORY_REPEAT_US = 30 * units::kUsPerSecond;

std::atomic<int> s_brightness{0};  // told the real one at boot, before anything is sent

std::atomic<bool> s_screen_on{true};

void on_screen(bool on)
{
    note_screen(on);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_screen(on));
    ESP_ERROR_CHECK_WITHOUT_ABORT(on ? board::display_on(settings::get(settings::Key::Brightness))
                                     : board::display_off());
}

void on_preset(int preset)
{
    ESP_LOGI(TAG, "preset %d requested", preset);
    desk::on_preset(preset - 1, false);
}

void on_brightness(int percent)
{
    ESP_LOGI(TAG, "brightness %d%% requested", percent);
    s_brightness.store(percent, std::memory_order_relaxed);
    board::set_brightness_percent(percent);
    settings::set(settings::Key::Brightness, percent);
}

void on_refusal(const char *reason)
{
    char message[REFUSAL_MESSAGE_SIZE];
    std::snprintf(message, sizeof(message), "%s", reason);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::notify("Home Assistant", "That did not go through", message, ui::Level::Warn, REFUSAL_NOTICE_MS));
}

/** The journey to the next appointment is asked for well before it starts, so
 *  it is there when the calendar shows it. Asking the same again costs nothing. */
void ask_journey()
{
    ical::Event next[1];
    const int   count = ical::upcoming(next, 1);
    const auto  now   = static_cast<std::int64_t>(std::time(nullptr));
    const bool  soon  = count > 0 && next[0].start > now && next[0].start - now < ui::kJourneyAhead;
    travel::want(soon ? next[0].start : 0,
                 count > 0 && next[0].feed == ical::kWorkFeed ? travel::Place::Work
                                                              : travel::Place::Study);
}

void on_entities(const hass::ws::EntityStore &store)
{
    room::render(store);
}

void on_move(hass::protocol::Move direction)
{
    desk::Move move = desk::Move::Stop;
    switch (direction) {
        case hass::protocol::Move::Up:   move = desk::Move::Up; break;
        case hass::protocol::Move::Down: move = desk::Move::Down; break;
        default:                         move = desk::Move::Stop; break;
    }
    ESP_LOGI(TAG, "move %d requested", static_cast<int>(move));
    desk::on_network_move(move);
}

void on_notify(const hass::protocol::Notification &notice)
{
    ESP_LOGI(TAG, "showing notification: '%s'", notice.message.c_str());
    sound::ding();
    // Home Assistant says it in words; the screen takes a level.
    const std::string &word  = notice.level;
    const ui::Level    level = word == "error"     ? ui::Level::Bad
                               : word == "warning" ? ui::Level::Warn
                               : word == "success" ? ui::Level::Good
                                                   : ui::Level::Neutral;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify("Home Assistant", notice.title.c_str(), notice.message.c_str(),
                                             level, notice.timeout_ms));
}

void fill_network(hass::protocol::Telemetry &out)
{
    const wifi::Info info = wifi::info();
    if (info.have_ap) {
        out.rssi_dbm = info.rssi_dbm;
    }
    if (info.ip[0] != '\0') {
        out.ip_address = info.ip;
    }
}

void on_radar(const radar::Snapshot &snapshot)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_radar(snapshot));
}

void on_radar_details(const char *hex, const radar::Details &details)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_radar_details(hex, details));
}

void on_radar_photo(const char *hex, const void *pixels, int width, int height, const char *credit)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_radar_photo(hex, pixels, width, height, credit));
}

void on_album_art(media::Art state, const void *pixels, int width, const void *large)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_album_art(pixels, state == media::Art::Failed, width));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_album_art_large(large));
}

void on_pick_art(int index, const void *pixels)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_pick_art(index, pixels));
}

void on_still(const void *pixels)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_cinema_still(pixels));
}

unsigned kib(std::size_t bytes)
{
    return static_cast<unsigned>(bytes / units::kBytesPerKiB);
}

hass::protocol::Telemetry gather_telemetry(const ble::Stats &radio)
{
    hass::protocol::Telemetry out;
    out.height_mm      = desk::height_mm();
    out.desk_connected = desk::linked();
    out.motion         = desk::motion();
    out.preset         = desk::active_preset_label();
    out.screen         = s_screen_on.load(std::memory_order_relaxed);
    out.brightness     = s_brightness.load(std::memory_order_relaxed);
    out.uptime_s       = static_cast<std::uint32_t>(esp_timer_get_time() / units::kUsPerSecond);
    out.free_heap      = static_cast<std::uint32_t>(esp_get_free_heap_size());
    if (const ota::RolledBack back = ota::rolled_back(); back.happened) {
        out.last_update = std::string("rolled back: ") + back.version;
    }

    power::State battery{};
    if (power::read(battery) == ESP_OK && battery.present) {
        out.battery_percent   = battery.percent;
        out.battery_volts     = battery.bus_volts;
        out.battery_milliamps = static_cast<int>(battery.current_amps * MILLIAMPS_PER_AMP);
        out.charging          = battery.charging;
        out.on_battery        = battery.on_battery;
    }

    if (wifi::connected()) {
        fill_network(out);
    }

    out.presence      = radio.phone_present;
    out.presence_rssi = radio.ever_seen ? radio.phone_rssi : hass::protocol::kUnheardRssiDbm;
    return out;
}

bool links_up()
{
    return wifi::connected() && hass::connected() && hass::ws::connected();
}

void show_links(const ble::Stats &radio)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_links(wifi::connected(), hass::connected() && hass::ws::connected()));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_presence(radio.has_key, radio.phone_present, radio.ever_seen));

    if (links_up()) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_done());
    }
}

void log_heap_baseline_once()
{
    static bool settled = false;
    if (settled || esp_timer_get_time() <= HEAP_BASELINE_AFTER_US) {
        return;
    }
    settled = true;
    multi_heap_info_t dma{};
    multi_heap_info_t psram{};
    heap_caps_get_info(&dma, MALLOC_CAP_DMA);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "dma-capable: %u KB free of %u KB, largest block %u KB, low %u KB",
             kib(dma.total_free_bytes), kib(dma.total_free_bytes + dma.total_allocated_bytes),
             kib(dma.largest_free_block), kib(dma.minimum_free_bytes));
    ESP_LOGI(TAG, "psram: %u KB free of %u KB, largest block %u KB", kib(psram.total_free_bytes),
             kib(psram.total_free_bytes + psram.total_allocated_bytes),
             kib(psram.largest_free_block));
}

void log_dma_heap()
{
    multi_heap_info_t now{};
    heap_caps_get_info(&now, MALLOC_CAP_DMA);
    ESP_LOGD(TAG, "dma-capable: %u KB free, largest %u KB, low %u KB", kib(now.total_free_bytes),
             kib(now.largest_free_block), kib(now.minimum_free_bytes));
}

void warn_if_memory_low()
{
    static std::int64_t complained = 0;
    const std::size_t   dma_free   = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const std::size_t   internal   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (dma_free < LOW_DMA_BYTES && esp_timer_get_time() - complained > LOW_MEMORY_REPEAT_US) {
        complained = esp_timer_get_time();
        // The largest block is what a client restart needs for its task stack.
        ESP_LOGW(TAG, "low memory: %u KB dma-capable, %u KB internal, largest %u KB",
                 kib(dma_free), kib(internal),
                 kib(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    }
}

void start_clients()
{
    const hass::Handlers handlers{on_preset, on_brightness, on_notify, on_move, on_screen};
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::start(handlers, board::kMinBrightness));
    hass::ws::on_refusal(on_refusal);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::start(on_entities, room::entities(), room::attributes()));
    ESP_ERROR_CHECK_WITHOUT_ABORT(jellyfin::start(room::on_jellyfin));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ble::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(jpeg::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(media::start(hass::ws::http_origin(), on_album_art, on_pick_art, on_still));
    ESP_ERROR_CHECK_WITHOUT_ABORT(radar::start(on_radar, on_radar_details, on_radar_photo));
}

void publish()
{
    if (wifi::connected()) {
        ota::confirm();  // reachable for the next update: the firmware stays
    }
    const ble::Stats                radio = ble::stats();
    const hass::protocol::Telemetry out   = gather_telemetry(radio);
    show_links(radio);
    hass::publish(out);
    ask_journey();
    log_heap_baseline_once();
    log_dma_heap();
    warn_if_memory_low();
}

// Waits for an address, or long enough, to start the clients; then for the
// links, a while; then publishes now and then.
jobs::Result tick()
{
    enum class Stage { Address, Links, Publishing };
    static Stage        stage = Stage::Address;
    static std::int64_t since = esp_timer_get_time();
    const int           for_ms =
        static_cast<int>((esp_timer_get_time() - since) / units::kUsPerMs);
    switch (stage) {
        case Stage::Address:
            if (!wifi::connected() && for_ms < NETWORK_WAIT_MS) {
                return jobs::again_in(LOOK_EVERY_MS);
            }
            if (!wifi::connected()) {
                ESP_LOGW(TAG, "no address after %d s, starting clients anyway",
                         NETWORK_WAIT_MS / units::kMsPerSecond);
            }
            start_clients();
            stage = Stage::Links;
            since = esp_timer_get_time();
            return jobs::again_in(LOOK_EVERY_MS);
        case Stage::Links:
            if (!links_up() && for_ms < LINKS_WAIT_MS) {
                return jobs::again_in(LOOK_EVERY_MS);
            }
            stage = Stage::Publishing;
            break;
        case Stage::Publishing: break;
    }
    publish();
    return jobs::again_in(PUBLISH_MS);
}

}  // namespace

esp_err_t start()
{
    jobs::Spec spec;
    spec.name = TAG;
    spec.run  = tick;
    ESP_RETURN_ON_FALSE(jobs::add(std::move(spec)) != jobs::kNoJob, ESP_ERR_NO_MEM, TAG, "job");
    return ESP_OK;
}

// Whoever turned it off, the panel or Home Assistant; each part that cares
// hears it from app.
void note_screen(bool on)
{
    s_screen_on.store(on, std::memory_order_relaxed);
    app::set(app::Fact::ScreenOn, on);
}

void note_brightness(int percent)
{
    s_brightness.store(percent, std::memory_order_relaxed);
}

}  // namespace network
