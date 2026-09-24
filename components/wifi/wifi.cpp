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

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
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

constexpr TickType_t FIRST_BACKOFF   = pdMS_TO_TICKS(1000);
constexpr TickType_t MAX_BACKOFF     = pdMS_TO_TICKS(30000);
constexpr TickType_t BRING_UP_RETRY  = pdMS_TO_TICKS(30000);
// Without an address this long, the association is kicked; this long again,
// the radio is restarted.
constexpr TickType_t KICK_AFTER      = pdMS_TO_TICKS(3 * 60 * 1000);
constexpr TickType_t RESTART_AFTER   = pdMS_TO_TICKS(10 * 60 * 1000);

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

std::atomic<bool> s_dropped{false};  // wants a connect
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
        if (s_events != nullptr) {
            xEventGroupClearBits(s_events, GOT_IP_BIT);
        }
        ESP_LOGW(TAG, "disconnected from '%s' (reason %d)", TAB5_WIFI_SSID, event->reason);
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
        ESP_LOGI(TAG, "joined '%s', ip " IPSTR, TAB5_WIFI_SSID, IP2STR(&event->ip_info.ip));
        start_time_sync();
        wake_task();
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
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "wifi init");

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char *>(config.sta.ssid), TAB5_WIFI_SSID,
                 sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(config.sta.password), TAB5_WIFI_PASS,
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
    ESP_LOGI(TAG, "joining '%s'", TAB5_WIFI_SSID);
    return ESP_OK;
}

[[noreturn]] void wifi_task(void *)
{
    bool       up          = false;
    TickType_t retry_up_at = 0;
    bool       want        = false;
    TickType_t next_try    = 0;
    TickType_t backoff     = FIRST_BACKOFF;
    TickType_t lost_since  = xTaskGetTickCount();
    bool       kicked      = false;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        const TickType_t now = xTaskGetTickCount();

        if (!up) {
            if (static_cast<std::int32_t>(now - retry_up_at) >= 0) {
                up = bring_up() == ESP_OK;
                if (!up) {
                    ESP_LOGW(TAG, "radio not up, trying again in 30 s");
                    retry_up_at = now + BRING_UP_RETRY;
                }
            }
            continue;
        }

        if (s_got_ip.exchange(false, std::memory_order_relaxed)) {
            want       = false;
            backoff    = FIRST_BACKOFF;
            lost_since = 0;
            kicked     = false;
        }
        if (s_dropped.exchange(false, std::memory_order_relaxed)) {
            want     = true;
            next_try = now + (lost_since == 0 ? 0 : backoff);
            if (lost_since == 0) {
                lost_since = now;
            }
        }
        if (s_connected.load(std::memory_order_relaxed)) {
            lost_since = 0;
            continue;
        }
        if (lost_since == 0) {
            lost_since = now;
        }

        if (want && static_cast<std::int32_t>(now - next_try) >= 0) {
            const esp_err_t err = esp_wifi_connect();
            if (err == ESP_OK) {
                want = false;  // the next disconnect asks again
            } else {
                ESP_LOGW(TAG, "connect refused: %s", esp_err_to_name(err));
            }
            next_try = now + backoff;
            backoff  = std::min<TickType_t>(backoff * 2, MAX_BACKOFF);
        }

        const TickType_t lost_for = now - lost_since;
        if (lost_for > RESTART_AFTER) {
            ESP_LOGW(TAG, "no address for ten minutes, restarting the radio");
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());  // STA_START asks for a connect
            lost_since = now;
            kicked     = false;
            backoff    = FIRST_BACKOFF;
        } else if (lost_for > KICK_AFTER && !kicked) {
            ESP_LOGW(TAG, "no address for three minutes, starting the association over");
            kicked = true;
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
            want     = true;
            next_try = now;
        }
    }
}

}  // namespace

esp_err_t start()
{
    setenv("TZ", TIMEZONE, 1);
    tzset();

    s_events = xEventGroupCreateStatic(&s_events_ctrl);

    if (std::strlen(TAB5_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "no SSID compiled in, not starting - see wifi_secrets.example.h");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_sta() != nullptr, ESP_FAIL, TAG, "sta netif");

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
