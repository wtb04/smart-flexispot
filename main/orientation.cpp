#include "orientation.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "imu.h"
#include "jobs.h"
#include "settings.h"

#include <cmath>

namespace orientation {
namespace {
constexpr char TAG[] = "orientation";

constexpr int TICK_MS = 100;

// Standing, not lying on the desk or held at an angle: most of gravity along
// the screen's long-side axis.
constexpr float STANDING_G = 0.6f;

// Turned only once it has stayed the other way up this long, so a panel being
// picked up or knocked does not spin the screen.
constexpr int SETTLED_TICKS = 5;

jobs::Job s_job = jobs::kNoJob;

bool standing(float x, float z)
{
    return std::fabs(x) >= STANDING_G && std::fabs(x) > std::fabs(z);
}

// Turns the screen once it has stood the other way up for SETTLED_TICKS looks.
jobs::Result look()
{
    static int other_way = 0;
    if (!settings::enabled(settings::Key::OrientAuto)) {
        other_way = 0;
        return jobs::done();
    }
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!imu::gravity(x, y, z) || !standing(x, z)) {
        other_way = 0;
        return jobs::done();
    }
    const int  upright = settings::get(settings::Key::OrientSign);
    const bool flipped = (x > 0.0f ? 1 : -1) != upright;
    if (flipped == settings::enabled(settings::Key::Flipped)) {
        other_way = 0;
        return jobs::done();
    }
    if (++other_way < SETTLED_TICKS) {
        return jobs::done();
    }
    other_way = 0;
    ESP_LOGI(TAG, "stood %s (x %.2f g)", flipped ? "the other way up" : "upright", x);
    board::set_flipped(flipped);
    // Kept, so the next boot starts the way it last stood.
    settings::set(settings::Key::Flipped, flipped);
    return jobs::done();
}

}  // namespace

esp_err_t start()
{
    jobs::Spec spec;
    spec.name      = TAG;
    spec.period_ms = TICK_MS;
    spec.run       = look;
    s_job          = jobs::add(std::move(spec));
    ESP_RETURN_ON_FALSE(s_job != jobs::kNoJob, ESP_ERR_NO_MEM, TAG, "job");
    return ESP_OK;
}

void calibrate_and_refresh()
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (imu::gravity(x, y, z) && standing(x, z)) {
        // Somebody just pressed Auto, so the screen is the right way up for them.
        const bool flipped = settings::enabled(settings::Key::Flipped);
        const int  sign    = x > 0.0f ? 1 : -1;
        settings::set(settings::Key::OrientSign, flipped ? -sign : sign);
        ESP_LOGI(TAG, "upright is x %s", (flipped ? -sign : sign) > 0 ? "positive" : "negative");
    } else {
        ESP_LOGI(TAG, "not standing, keeping what upright was taken to be");
    }
    jobs::poke(s_job);
}

}  // namespace orientation
