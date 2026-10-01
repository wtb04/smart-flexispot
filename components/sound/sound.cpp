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
constexpr char CODEC_TAG[] = "Adev_Codec";
constexpr char I2S_IF_TAG[] = "I2S_IF";  // the codec library's I2S side

constexpr int SAMPLE_RATE     = 16000;
constexpr int BITS_PER_SAMPLE = sizeof(std::int16_t) * CHAR_BIT;
constexpr int CODEC_CHANNELS  = 2;  // the codec is stereo; the chime is mono, duplicated
constexpr int MAX_VOLUME      = 100;

// One soft drop, not a chime: a pure tone that falls quickly from its first
// pitch to its last, as a drop of water does, and rings on a little. It sits
// where the Tab5's small speaker speaks, over 800 Hz, and softly.
constexpr float FROM_HZ   = 1500.0f;
constexpr float TO_HZ     = 1000.0f;
constexpr float GLIDE_S   = 0.025f;  // the pitch falls most of the way in this
constexpr float ATTACK_S  = 0.005f;
constexpr float RING_S    = 0.080f;  // the sound falls to 1/e in this
constexpr int   CHIME_MS  = 400;
constexpr float PEAK      = 0.20f;   // of full scale
constexpr float FULL_TURN = 2.0f * static_cast<float>(M_PI);

// Between chimes the amplifier is off and the codec closed, which stops its
// clocks as well; a chime has both for as long as it plays. The amplifier
// comes on over silence and goes off once the DMA has played the last of it.
constexpr TickType_t AMP_SETTLE = pdMS_TO_TICKS(20);
constexpr TickType_t TAIL       = pdMS_TO_TICKS(80);

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
    const int count = SAMPLE_RATE * CHIME_MS / units::kMsPerSecond;
    const int fade  = SAMPLE_RATE * 20 / units::kMsPerSecond;  // to nothing, so the amplifier goes off on silence
    s_chime.reserve(static_cast<std::size_t>(count));
    float phase = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float t      = static_cast<float>(i) / SAMPLE_RATE;
        const float hz     = TO_HZ + (FROM_HZ - TO_HZ) * std::exp(-t / GLIDE_S);
        const float attack = t < ATTACK_S ? t / ATTACK_S : 1.0f;
        const float tail   = i > count - fade ? static_cast<float>(count - i) / fade : 1.0f;
        phase += FULL_TURN * hz / SAMPLE_RATE;
        const float sample = std::sin(phase) * attack * std::exp(-t / RING_S) * tail * PEAK;
        s_chime.push_back(static_cast<std::int16_t>(sample * INT16_MAX));
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

esp_err_t make_speaker()
{
    // The board package opens its I2S channel with the driver's default of 240
    // frames a DMA buffer, which the driver rounds up to 256 to align it and
    // warns about on every boot. The package takes no channel settings of its
    // own, so the one expected warning is kept out of the log instead.
    const esp_log_level_t i2s_level = esp_log_level_get(I2S_TAG);
    esp_log_level_set(I2S_TAG, ESP_LOG_ERROR);
    s_speaker = bsp_audio_codec_speaker_init();
    esp_log_level_set(I2S_TAG, i2s_level);
    ESP_RETURN_ON_FALSE(s_speaker != nullptr, ESP_FAIL, TAG, "codec init");
    // The board package switches the amplifier on with the codec; it waits for a chime.
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_SPEAKER, false), TAG, "speaker power");
    esp_log_level_set(CODEC_TAG, ESP_LOG_WARN);  // they announce every open
    esp_log_level_set(I2S_IF_TAG, ESP_LOG_WARN);
    return ESP_OK;
}

void play_chime()
{
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = BITS_PER_SAMPLE;
    fs.channel         = CODEC_CHANNELS;
    fs.channel_mask    = 0;
    fs.sample_rate     = SAMPLE_RATE;
    // Opening sets the format, and the codec library disables the channel to do
    // so; closed between sounds, it was never enabled, which the I2S driver
    // logs as an error each time. Nothing is wrong, so that one is kept quiet.
    const esp_log_level_t i2s_level = esp_log_level_get(I2S_TAG);
    esp_log_level_set(I2S_TAG, ESP_LOG_NONE);
    const int opened = esp_codec_dev_open(s_speaker, &fs);
    esp_log_level_set(I2S_TAG, i2s_level);
    if (opened != 0) {
        ESP_LOGW(TAG, "codec would not open");
        return;
    }
    esp_codec_dev_set_out_vol(s_speaker, s_volume.load(std::memory_order_relaxed));
    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_feature_enable(BSP_FEATURE_SPEAKER, true));
    vTaskDelay(AMP_SETTLE);
    esp_codec_dev_write(s_speaker, s_chime.data(), static_cast<int>(s_chime.size() * sizeof(std::int16_t)));
    vTaskDelay(TAIL);
    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_feature_enable(BSP_FEATURE_SPEAKER, false));
    esp_codec_dev_close(s_speaker);
}

[[noreturn]] void sound_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_speaker != nullptr && !s_chime.empty()) {
            play_chime();
        }
    }
}

}  // namespace

esp_err_t init()
{
    ESP_RETURN_ON_ERROR(make_speaker(), TAG, "speaker");
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
