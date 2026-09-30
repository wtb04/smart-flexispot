#include "battery.h"

#include "esp_check.h"
#include "esp_log.h"
#include "jobs.h"
#include "power.h"
#include "settings.h"
#include "ui.h"
#include "units.h"

#include <atomic>
#include <cmath>
#include <cstdlib>

namespace battery {
namespace {
constexpr char TAG[] = "battery";

// Each reading adds its current to the charge counted, so they come often; the
// rest, the charger and what the screen shows, only every CHECK_EVERY of them.
constexpr int POLL_MS     = units::kMsPerSecond;
constexpr int        CHECK_EVERY   = 30;

constexpr float RESUME_VOLTS = 8.00f;

// A reading is logged when it moves this far, rather than every poll: the log
// is for what changed.
constexpr int LOG_STEP_PERCENT = 5;
// Far enough from any real percentage that the first reading is always logged.
constexpr int NEVER_LOGGED_PERCENT = -100;

constexpr float MILLIAMPS_PER_AMP = 1000.0f;

jobs::Job         s_job = jobs::kNoJob;
std::atomic<bool> s_check_now{true};  // the first pass looks at everything

void log_if_changed(const power::State &state, bool present, bool &was_present, int &logged)
{
    if (present == was_present && std::abs(state.percent - logged) < LOG_STEP_PERCENT) {
        return;
    }
    if (present) {
        ESP_LOGI(TAG, "%d%% %.2f V %d mA%s", state.percent, state.bus_volts,
                 static_cast<int>(std::lround(state.current_amps * MILLIAMPS_PER_AMP)),
                 state.full ? " full" : "");
    } else {
        ESP_LOGI(TAG, "no pack (%.2f V)", state.bus_volts);
    }
    was_present = present;
    logged      = state.percent;
}

// Whether there is a pack is the charger's to find out: one that has run down
// to its protection reads as absent until the charger is on to wake it. Only a
// full one is left alone, so it is not held at the top for ever.
void steer_charger(const power::State &state, bool &topped_off)
{
    if (state.full) {
        topped_off = true;
    } else if (state.bus_volts <= RESUME_VOLTS) {
        topped_off = false;
    }

    const bool wanted = settings::enabled(settings::Key::Charging) && !topped_off;
    if (wanted != power::charging_enabled() && power::set_charging(wanted) == ESP_OK) {
        ESP_LOGI(TAG, "charger %s", wanted       ? "on"
                                    : topped_off ? "off, pack full"
                                                 : "off");
    }
}

jobs::Result poll()
{
    static bool topped_off   = false;
    static bool was_present  = false;
    static int  logged       = NEVER_LOGGED_PERCENT;
    static int  kept_percent = -1;
    static int  pass         = 0;
    static bool probing      = false;  // the charger is off while the voltage settles
    // What was read before the probe: with the charger off, the rail says
    // nothing about charging.
    static power::State before{};

    power::State state{};
    bool         read = power::read(state) == ESP_OK;
    if (read && state.percent != kept_percent) {
        kept_percent = state.percent;
        settings::set(settings::Key::BatteryCharge, power::charge_mah());
    }
    bool present = false;
    if (probing) {
        probing = false;
        ESP_ERROR_CHECK_WITHOUT_ABORT(power::finish_probe(present));
        state = before;
        read  = true;
    } else {
        // Poked by refresh(), or at the turn of a round: the rest too.
        const bool check = s_check_now.exchange(false) || ++pass % CHECK_EVERY == 0;
        if (!read || !check) {
            return jobs::done();
        }
        power::reassert_charging();
        if (!power::begin_probe(present)) {
            probing = true;
            before  = state;
            return jobs::again_in(power::kProbeSettleMs);
        }
    }
    if (read) {
        log_if_changed(state, present, was_present, logged);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_battery(state.present, state.percent, state.charging));
        steer_charger(state, topped_off);
    }
    return jobs::done();
}

}  // namespace

esp_err_t start()
{
    power::restore_charge(settings::get(settings::Key::BatteryCharge));
    ESP_RETURN_ON_ERROR(power::init(), TAG, "power monitor");

    // The first pass switches the charger on or off, once it has looked at the pack.
    jobs::Spec spec;
    spec.name      = TAG;
    spec.period_ms = POLL_MS;
    spec.run       = poll;
    s_job          = jobs::add(std::move(spec));
    ESP_RETURN_ON_FALSE(s_job != jobs::kNoJob, ESP_ERR_NO_MEM, TAG, "job");
    return ESP_OK;
}

void refresh()
{
    s_check_now = true;
    jobs::poke(s_job);
}

}  // namespace battery
