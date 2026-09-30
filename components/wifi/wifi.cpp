#include "wifi.h"

#include "app_state.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "backup_clock.h"
#include "nvs_flash.h"
#include "units.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif

namespace wifi {
namespace {
constexpr char TAG[] = "wifi";
constexpr char HOSTNAME[] = "smart-flexispot";  // what the router lists, and tools/ota.sh asks for

std::atomic<bool> s_connected{false};

constexpr EventBits_t GOT_IP_BIT = BIT0;
StaticEventGroup_t    s_events_ctrl;
EventGroupHandle_t    s_events = nullptr;
bool              s_sntp_started = false;

constexpr char TIMEZONE[] = "CET-1CEST,M3.5.0,M10.5.0/3";

void on_time_synced(timeval *)
{
    const std::time_t now = std::time(nullptr);
    if (now > 0 && rtc::store(now) == ESP_OK) {
        ESP_LOGI(TAG, "backup clock set from the network");
    }
}

void start_time_sync()
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start                    = true;
    config.sync_cb                  = on_time_synced;
    config.server_from_dhcp         = true;  // a local NTP server offered by DHCP wins
    config.renew_servers_after_new_IP = true;

    const esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed: %s", esp_err_to_name(err));
        s_sntp_started = false;
    }
}

// Only the Wi-Fi task calls into the radio: with the radio on the C6 every
// call is a round trip over SDIO that can take seconds, which must not happen
// on the event task. The event handler only records what happened.
constexpr std::uint32_t TASK_STACK    = 3072;
constexpr UBaseType_t   TASK_PRIORITY = 3;

constexpr TickType_t TICK           = pdMS_TO_TICKS(units::kMsPerSecond);
constexpr TickType_t FIRST_BACKOFF  = pdMS_TO_TICKS(units::kMsPerSecond);
constexpr TickType_t MAX_BACKOFF    = pdMS_TO_TICKS(30 * units::kMsPerSecond);
constexpr TickType_t BRING_UP_RETRY = pdMS_TO_TICKS(30 * units::kMsPerSecond);
// Without an address this long, the association is kicked; this long again,
// the radio is restarted.
constexpr TickType_t KICK_AFTER    = pdMS_TO_TICKS(3 * units::kMsPerMinute);
constexpr TickType_t RESTART_AFTER = pdMS_TO_TICKS(10 * units::kMsPerMinute);

constexpr TickType_t RADIO_POWER_SETTLE = pdMS_TO_TICKS(100);

constexpr int MAC_LENGTH = 6;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

std::atomic<bool> s_dropped{false};  // wants a connect
std::atomic<bool> s_save_wanted{false};  // power saving asked for
bool              s_save_applied = false;

// With power saving the station sleeps between beacons and the access point
// holds its packets until the next: round trips of 2 to 290 ms, measured here.
// Too slow for a desk driven from the screen, but nothing waits on it while
// the screen is dark, and the radio then costs a fraction.
void apply_power_save()
{
    const bool save = s_save_wanted.load(std::memory_order_relaxed);
    if (save == s_save_applied) {
        return;
    }
    const esp_err_t err = esp_wifi_set_ps(save ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power saving %s refused: %s", save ? "on" : "off", esp_err_to_name(err));
        return;  // tried again on the next tick
    }
    s_save_applied = save;
    ESP_LOGI(TAG, "power saving %s", save ? "on" : "off");
}

portMUX_TYPE s_info_lock = portMUX_INITIALIZER_UNLOCKED;
Info         s_info;

constexpr TickType_t INFO_REFRESH = pdMS_TO_TICKS(5 * units::kMsPerSecond);

void read_mac(Info &info)
{
    std::uint8_t mac[MAC_LENGTH]{};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        std::snprintf(info.mac, sizeof(info.mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                      mac[2], mac[3], mac[4], mac[5]);
    }
}

void read_link(Info &info)
{
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        info.have_ap = true;
        std::snprintf(info.ssid, sizeof(info.ssid), "%s", reinterpret_cast<const char *>(ap.ssid));
        info.rssi_dbm = ap.rssi;
        info.channel  = ap.primary;
    }
    esp_netif_t        *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        std::snprintf(info.ip, sizeof(info.ip), IPSTR, IP2STR(&ip.ip));
    }
}

void refresh_info(bool connected)
{
    Info next;
    {
        portENTER_CRITICAL(&s_info_lock);
        next = s_info;
        portEXIT_CRITICAL(&s_info_lock);
    }
    next.connected = connected;
    if (next.mac[0] == '\0') {
        read_mac(next);
    }
    next.have_ap = false;
    next.ssid[0] = '\0';
    next.ip[0]   = '\0';
    if (connected) {
        read_link(next);
    }
    portENTER_CRITICAL(&s_info_lock);
    s_info = next;
    portEXIT_CRITICAL(&s_info_lock);
}

std::atomic<bool> s_got_ip{false};

void wake_task()
{
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

void on_wifi_event(void *, esp_event_base_t base, std::int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_dropped.store(true, std::memory_order_relaxed);
        wake_task();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
        s_connected.store(false, std::memory_order_relaxed);
        app::set(app::Fact::Online, false);
        if (s_events != nullptr) {
            xEventGroupClearBits(s_events, GOT_IP_BIT);
        }
        ESP_LOGW(TAG, "disconnected from '%s' (reason %d)", WIFI_SSID, event->reason);
        // The driver finds the next access point itself while roaming.
        if (event->reason != WIFI_REASON_ROAMING) {
            s_dropped.store(true, std::memory_order_relaxed);
            wake_task();
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<ip_event_got_ip_t *>(data);
        s_connected.store(true, std::memory_order_relaxed);
        s_got_ip.store(true, std::memory_order_relaxed);
        if (s_events != nullptr) {
            xEventGroupSetBits(s_events, GOT_IP_BIT);
        }
        ESP_LOGI(TAG, "joined '%s', ip " IPSTR, WIFI_SSID, IP2STR(&event->ip_info.ip));
        start_time_sync();
        wake_task();
        app::set(app::Fact::Online, true);
    }
}

esp_err_t init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    return err;
}

/** Powers the radio and starts the driver. Safe to try again after a failure. */
esp_err_t bring_up()
{
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_WIFI, true), TAG, "wifi power");
    vTaskDelay(RADIO_POWER_SETTLE);

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "wifi init");

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char *>(config.sta.ssid), WIFI_SSID,
                 sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(config.sta.password), WIFI_PASS,
                 sizeof(config.sta.password) - 1);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bringing the radio up: %s", esp_err_to_name(err));
        esp_wifi_deinit();
        return err;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    s_save_applied = false;
    ESP_LOGI(TAG, "joining '%s'", WIFI_SSID);
    return ESP_OK;
}

bool reached(TickType_t now, TickType_t at)
{
    return static_cast<std::int32_t>(now - at) >= 0;
}

bool bring_up_when_due(TickType_t now, TickType_t &retry_up_at)
{
    if (!reached(now, retry_up_at)) {
        return false;
    }
    if (bring_up() == ESP_OK) {
        return true;
    }
    ESP_LOGW(TAG, "radio not up, trying again in 30 s");
    retry_up_at = now + BRING_UP_RETRY;
    return false;
}

/** What the Wi-Fi task keeps between passes once the radio is up. */
struct Link {
    bool       want        = false;
    TickType_t next_try    = 0;
    TickType_t backoff     = FIRST_BACKOFF;
    TickType_t lost_since  = 0;
    bool       kicked      = false;
    TickType_t info_at     = 0;
    bool       info_was_up = false;
};

void take_events(Link &link, TickType_t now)
{
    if (s_got_ip.exchange(false, std::memory_order_relaxed)) {
        link.want       = false;
        link.backoff    = FIRST_BACKOFF;
        link.lost_since = 0;
        link.kicked     = false;
    }
    if (s_dropped.exchange(false, std::memory_order_relaxed)) {
        link.want     = true;
        link.next_try = now + (link.lost_since == 0 ? 0 : link.backoff);
        if (link.lost_since == 0) {
            link.lost_since = now;
        }
    }
}

void refresh_info_when_due(Link &link, bool is_up, TickType_t now)
{
    if (is_up != link.info_was_up || now - link.info_at > INFO_REFRESH) {
        refresh_info(is_up);
        link.info_was_up = is_up;
        link.info_at     = now;
    }
}

void connect_when_due(Link &link, TickType_t now)
{
    if (!link.want || !reached(now, link.next_try)) {
        return;
    }
    const esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        link.want = false;  // the next disconnect asks again
    } else {
        ESP_LOGW(TAG, "connect refused: %s", esp_err_to_name(err));
    }
    link.next_try = now + link.backoff;
    link.backoff  = std::min<TickType_t>(link.backoff * 2, MAX_BACKOFF);
}

void recover_when_lost_long(Link &link, TickType_t now)
{
    const TickType_t lost_for = now - link.lost_since;
    if (lost_for > RESTART_AFTER) {
        ESP_LOGW(TAG, "no address for ten minutes, restarting the radio");
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());  // STA_START asks for a connect
        link.lost_since = now;
        link.kicked     = false;
        link.backoff    = FIRST_BACKOFF;
    } else if (lost_for > KICK_AFTER && !link.kicked) {
        ESP_LOGW(TAG, "no address for three minutes, starting the association over");
        link.kicked = true;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
        link.want     = true;
        link.next_try = now;
    }
}

[[noreturn]] void wifi_task(void *)
{
    bool       up          = false;
    TickType_t retry_up_at = 0;
    Link       link;
    link.lost_since = xTaskGetTickCount();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, TICK);
        const TickType_t now = xTaskGetTickCount();

        if (!up) {
            up = bring_up_when_due(now, retry_up_at);
            continue;
        }

        take_events(link, now);
        apply_power_save();
        const bool is_up = s_connected.load(std::memory_order_relaxed);
        refresh_info_when_due(link, is_up, now);
        if (is_up) {
            link.lost_since = 0;
            continue;
        }
        if (link.lost_since == 0) {
            link.lost_since = now;
        }
        connect_when_due(link, now);
        recover_when_lost_long(link, now);
    }
}

// Power saving on while nothing needs the link answering at once, as while the screen is dark.
void set_power_save(bool save)
{
    s_save_wanted.store(save, std::memory_order_relaxed);
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

}  // namespace


esp_err_t start()
{
    setenv("TZ", TIMEZONE, 1);
    tzset();

    s_events = xEventGroupCreateStatic(&s_events_ctrl);

    if (std::strlen(WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no SSID compiled in, not starting - see wifi_secrets.example.h");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(netif != nullptr, ESP_FAIL, TAG, "sta netif");
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_hostname(netif, HOSTNAME));

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            on_wifi_event, nullptr, nullptr),
                        TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            on_wifi_event, nullptr, nullptr),
                        TAG, "ip events");

    // The radio is brought up by the task, which keeps trying if it will not come.
    s_task = xTaskCreateStatic(wifi_task, "wifi_sup", TASK_STACK, nullptr, TASK_PRIORITY,
                               s_task_stack, &s_task_ctrl);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    set_power_save(!app::get(app::Fact::ScreenOn));
    app::watch(app::Fact::ScreenOn, [](bool on) { set_power_save(!on); });
    xTaskNotifyGive(s_task);  // up now, not at its first tick
    return ESP_OK;
}

bool connected()
{
    return s_connected.load(std::memory_order_relaxed);
}

Info info()
{
    portENTER_CRITICAL(&s_info_lock);
    Info copy = s_info;
    portEXIT_CRITICAL(&s_info_lock);
    return copy;
}

bool wait_for_ip(int timeout_ms)
{
    if (s_events == nullptr) {
        return false;
    }
    const EventBits_t bits =
        xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & GOT_IP_BIT) != 0;
}

}  // namespace wifi
