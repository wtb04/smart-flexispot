#include "transport.h"

#include "esp_log.h"
#include "loctek.h"

namespace desk::detail {
namespace {
constexpr char TAG[] = "desk";

constexpr int        WIRING_FAULT_AFTER_WAKES = 3;
constexpr TickType_t LINK_TIMEOUT             = pdMS_TO_TICKS(3000);

loctek::Move as_move(int direction)
{
    return direction > 0 ? loctek::Move::Up : direction < 0 ? loctek::Move::Down : loctek::Move::Stop;
}

class Wire final : public Transport {
public:
    esp_err_t start(void (*on_height)(int)) override { return loctek::start(on_height); }

    void move(int direction) override
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::request_move(as_move(direction)));
    }

    void preset(int index) override
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::goto_preset(static_cast<loctek::Preset>(index)));
    }

    void store(int index) override
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::store_preset(static_cast<loctek::Preset>(index)));
    }

    bool goto_height(int height_mm) override { return loctek::goto_height(height_mm) == ESP_OK; }

    void wake() override
    {
        ESP_LOGI(TAG, "waking the desk on the wire");
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::wake());
    }

    bool can_wake() const override { return true; }

    void nudge() override
    {
#if CONFIG_LOCTEK_NUDGE_WAKE
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::nudge());
#endif
    }

    int motion() const override
    {
        switch (loctek::motion()) {
            case loctek::Move::Up:   return 1;
            case loctek::Move::Down: return -1;
            case loctek::Move::Stop: break;
        }
        return 0;
    }

    bool travelling() const override { return loctek::driving_to() >= 0; }

    const char *status(TickType_t now, bool height_known, int wake_attempts) override
    {
        const loctek::Stats stats = loctek::stats();
        if (stats.frames_decoded != previous_.frames_decoded) {
            last_frame_ = now;
        }
        previous_ = stats;

        if (last_frame_ != 0 && now - last_frame_ < LINK_TIMEOUT) {
            if (!height_known && stats.height_frames > 0 && stats.heights_decoded == 0) {
                return kAsleep;
            }
            return kConnected;
        }
        if (stats.bytes_received == 0) {
            return wake_attempts < WIRING_FAULT_AFTER_WAKES ? "waking desk"
                                                            : "no data on RX - check wiring";
        }
        if (stats.frames_decoded == 0) {
            return "garbage on RX - check baud and TX/RX";
        }
        return "disconnected";
    }

    const char *name() const override { return "the local wire"; }

private:
    loctek::Stats previous_{};
    TickType_t    last_frame_ = 0;
};

}  // namespace

Transport &wire()
{
    static Wire link;
    return link;
}

}  // namespace desk::detail
