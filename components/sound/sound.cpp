#include "sound.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace sound {
namespace {

constexpr char TAG[] = "sound";

constexpr int SAMPLE_RATE = 16000;
constexpr int VOLUME      = CONFIG_SOUND_VOLUME;

// Two notes a fifth apart: a chime rather than an alarm.
constexpr float NOTE_HZ[] = {880.0f, 1320.0f};
constexpr int   NOTE_MS   = 110;
// The speaker clicks audibly if a tone starts or stops at full amplitude, so
// each note fades in and out over this fraction of its length.
constexpr float EDGE_FRACTION = 0.25f;
constexpr float AMPLITUDE     = 0.22f;

constexpr std::uint32_t TASK_STACK    = 4096;
constexpr UBaseType_t   TASK_PRIORITY = 3;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

esp_codec_dev_handle_t s_speaker = nullptr;
std::vector<std::int16_t> s_chime;

void build_chime()
{
    const int per_note = SAMPLE_RATE * NOTE_MS / 1000;
    s_chime.reserve(static_cast<std::size_t>(per_note) * 2);

    for (float hz : NOTE_HZ) {
        const int edge = static_cast<int>(per_note * EDGE_FRACTION);
        for (int i = 0; i < per_note; ++i) {
            const float phase = 2.0f * static_cast<float>(M_PI) * hz * i / SAMPLE_RATE;
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

[[noreturn]] void sound_task(void *)
{
    for (;;) {
        // Coalesced: several notifications at once chime once, rather than
        // queueing a stack of overlapping beeps.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_speaker != nullptr && !s_chime.empty()) {
            esp_codec_dev_write(s_speaker, s_chime.data(),
                                static_cast<int>(s_chime.size() * sizeof(std::int16_t)));
        }
    }
}

}  // namespace

esp_err_t init()
{
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_SPEAKER, true), TAG, "speaker power");

    s_speaker = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(s_speaker != nullptr, ESP_FAIL, TAG, "codec init");

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16;
    fs.channel         = 2;   // the codec is stereo; the chime is duplicated below
    fs.channel_mask    = 0;
    fs.sample_rate     = SAMPLE_RATE;
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_speaker, &fs) == 0, ESP_FAIL, TAG, "codec open");
    esp_codec_dev_set_out_vol(s_speaker, VOLUME);

    build_chime();
    std::vector<std::int16_t> stereo;
    stereo.reserve(s_chime.size() * 2);
    for (std::int16_t sample : s_chime) {
        stereo.push_back(sample);
        stereo.push_back(sample);
    }
    s_chime.swap(stereo);

    s_task = xTaskCreateStaticPinnedToCore(sound_task, "sound", TASK_STACK, nullptr, TASK_PRIORITY,
                                           s_task_stack, &s_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "chime ready (%u samples)", static_cast<unsigned>(s_chime.size()));
    return ESP_OK;
}

void ding()
{
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

}  // namespace sound
