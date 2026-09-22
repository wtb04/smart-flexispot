#include "wifi.h"

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

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
// A build without the secrets header runs; it just never joins.
#ifndef TAB5_WIFI_SSID
#define TAB5_WIFI_SSID ""
#define TAB5_WIFI_PASS ""
#endif

namespace wifi {
namespace {

constexpr char TAG[] = "wifi";

std::atomic<bool> s_connected{false};

constexpr EventBits_t GOT_IP_BIT = BIT0;
StaticEventGroup_t    s_events_ctrl;
EventGroupHandle_t    s_events = nullptr;
bool              s_sntp_started = false;

// A POSIX TZ string, as there is no timezone database on the device:
// Europe/Amsterdam with its DST rules.
constexpr char TIMEZONE[] = "CET-1CEST,M3.5.0,M10.5.0/3";

// Written back so the next cold boot starts from the right time rather than
// from 1970, whether or not the network is there when it happens.
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

void on_wifi_event(void *, esp_event_base_t base, std::int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
        s_connected.store(false, std::memory_order_relaxed);
        if (s_events != nullptr) {
            xEventGroupClearBits(s_events, GOT_IP_BIT);
        }
        ESP_LOGW(TAG, "disconnected from '%s' (reason %d), retrying", TAB5_WIFI_SSID,
                 event->reason);
        // Retry forever: this is a wall panel and the router may be rebooting.
        esp_wifi_connect();
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<ip_event_got_ip_t *>(data);
        s_connected.store(true, std::memory_order_relaxed);
        if (s_events != nullptr) {
            xEventGroupSetBits(s_events, GOT_IP_BIT);
        }
        ESP_LOGI(TAG, "joined '%s', ip " IPSTR, TAB5_WIFI_SSID, IP2STR(&event->ip_info.ip));
        start_time_sync();
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

}  // namespace

esp_err_t start()
{
    // Before anything reads a clock: an unset TZ silently offsets every local time.
    setenv("TZ", TIMEZONE, 1);
    tzset();

    s_events = xEventGroupCreateStatic(&s_events_ctrl);

    if (std::strlen(TAB5_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no SSID compiled in, not starting - see wifi_secrets.example.h");
        return ESP_OK;
    }

    // The C6 co-processor sits behind a power gate on the IO expander; without
    // this it never answers on SDIO.
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_WIFI, true), TAG, "wifi power");
    vTaskDelay(pdMS_TO_TICKS(100));

    // Wi-Fi keeps its calibration data in NVS.
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != nullptr, ESP_FAIL, TAG, "sta netif");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "wifi init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            on_wifi_event, nullptr, nullptr),
                        TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            on_wifi_event, nullptr, nullptr),
                        TAG, "ip events");

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char *>(config.sta.ssid), TAB5_WIFI_SSID,
                 sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(config.sta.password), TAB5_WIFI_PASS,
                 sizeof(config.sta.password) - 1);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "sta config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    // The default has the station sleep between beacons, and the access point
    // holds its packets until the next one. Measured on this network: a round
    // trip alternating between 2 ms and 90 ms, with a worst case of 290 ms.
    // Nothing here is worth that, and a desk driven over the network is worth
    // it least of all.
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "joining '%s'", TAB5_WIFI_SSID);
    return ESP_OK;
}

bool connected()
{
    return s_connected.load(std::memory_order_relaxed);
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
