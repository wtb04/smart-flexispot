#include "sound.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "units.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace sound {
namespace {
constexpr char TAG[] = "sound";
constexpr char I2S_TAG[] = "i2s_common";  // the I2S driver's

constexpr int SAMPLE_RATE     = 16000;
constexpr int BITS_PER_SAMPLE = sizeof(std::int16_t) * CHAR_BIT;
constexpr int CODEC_CHANNELS  = 2;  // the codec is stereo; the chime is mono, duplicated
constexpr int MAX_VOLUME      = 100;

constexpr float NOTE_HZ[]     = {880.0f, 1320.0f};
constexpr int   NOTE_MS       = 110;
constexpr float EDGE_FRACTION = 0.25f;
constexpr float AMPLITUDE     = 0.22f;
constexpr float FULL_TURN     = 2.0f * static_cast<float>(M_PI);

constexpr std::uint32_t TASK_STACK    = 2560;  // measured: uses 0.3 KB
constexpr UBaseType_t   TASK_PRIORITY = 3;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

esp_codec_dev_handle_t s_speaker = nullptr;
std::atomic<int>       s_volume{0};  // silent until set_volume says otherwise
std::vector<std::int16_t> s_chime;

void build_chime()
{
    const int per_note = SAMPLE_RATE * NOTE_MS / units::kMsPerSecond;
    s_chime.reserve(static_cast<std::size_t>(per_note) * std::size(NOTE_HZ));

    for (float hz : NOTE_HZ) {
        const int edge = static_cast<int>(per_note * EDGE_FRACTION);
        for (int i = 0; i < per_note; ++i) {
            const float phase = FULL_TURN * hz * i / SAMPLE_RATE;
            float       gain  = 1.0f;
            if (i < edge) {
                gain = static_cast<float>(i) / edge;
            } else if (i > per_note - edge) {
                gain = static_cast<float>(per_note - i) / edge;
            }
            const float sample = std::sin(phase) * gain * AMPLITUDE;
            s_chime.push_back(static_cast<std::int16_t>(sample * INT16_MAX));
        }
    }
}

void spread_to_every_channel()
{
    std::vector<std::int16_t> spread;
    spread.reserve(s_chime.size() * CODEC_CHANNELS);
    for (std::int16_t sample : s_chime) {
        spread.insert(spread.end(), CODEC_CHANNELS, sample);
    }
    s_chime.swap(spread);
}

esp_err_t open_speaker()
{
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_SPEAKER, true), TAG, "speaker power");

    // The board package opens its I2S channel with the driver's default of 240
    // frames a DMA buffer, which the driver rounds up to 256 to align it and
    // warns about on every boot. The package takes no channel settings of its
    // own, so the one expected warning is kept out of the log instead.
    const esp_log_level_t i2s_level = esp_log_level_get(I2S_TAG);
    esp_log_level_set(I2S_TAG, ESP_LOG_ERROR);
    s_speaker = bsp_audio_codec_speaker_init();
    esp_log_level_set(I2S_TAG, i2s_level);
    ESP_RETURN_ON_FALSE(s_speaker != nullptr, ESP_FAIL, TAG, "codec init");

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = BITS_PER_SAMPLE;
    fs.channel         = CODEC_CHANNELS;
    fs.channel_mask    = 0;
    fs.sample_rate     = SAMPLE_RATE;
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_speaker, &fs) == 0, ESP_FAIL, TAG, "codec open");
    esp_codec_dev_set_out_vol(s_speaker, s_volume.load(std::memory_order_relaxed));
    return ESP_OK;
}

[[noreturn]] void sound_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_speaker != nullptr && !s_chime.empty()) {
            esp_codec_dev_set_out_vol(s_speaker, s_volume.load(std::memory_order_relaxed));
            esp_codec_dev_write(s_speaker, s_chime.data(),
                                static_cast<int>(s_chime.size() * sizeof(std::int16_t)));
        }
    }
}

}  // namespace

esp_err_t init()
{
    ESP_RETURN_ON_ERROR(open_speaker(), TAG, "speaker");
    build_chime();
    spread_to_every_channel();

    s_task = xTaskCreateStaticPinnedToCore(sound_task, "sound", TASK_STACK, nullptr, TASK_PRIORITY,
                                           s_task_stack, &s_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "chime ready (%u samples)", static_cast<unsigned>(s_chime.size()));
    return ESP_OK;
}

void set_volume(int percent)
{
    s_volume.store(std::clamp(percent, 0, MAX_VOLUME), std::memory_order_relaxed);
}

void ding()
{
    if (s_volume.load(std::memory_order_relaxed) <= 0) {
        return;
    }
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

}  // namespace sound
