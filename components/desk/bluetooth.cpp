#include "transport.h"

#include "ble_desk.h"
#include "deskproto.h"
#include "esp_log.h"

#include <atomic>

namespace desk::detail {
namespace {
constexpr char TAG[] = "desk";

// What the companion reports, a round trip behind.
std::atomic<int>  s_motion{0};
std::atomic<bool> s_driving{false};
void (*s_on_height)(int) = nullptr;

void on_status(const deskproto::Status &status)
{
    trace("heard", status.height_mm, deskproto::direction_of(status.motion) * 10 + (status.driving ? 1 : 0));
    s_motion.store(deskproto::direction_of(status.motion), std::memory_order_relaxed);
    s_driving.store(status.driving, std::memory_order_relaxed);
    if (status.height_mm >= 0 && s_on_height != nullptr) {
        s_on_height(status.height_mm);
    }
}

class Bluetooth final : public Transport {
public:
    esp_err_t start(void (*on_height)(int)) override
    {
        s_on_height = on_height;
        ble::desk::on_status(on_status);
        return ESP_OK;
    }

    void move(int direction) override
    {
        if (direction == 0) {
            ble::desk::stop();
        } else {
            ble::desk::hold(deskproto::motion_of(direction));
        }
    }

    void preset(int index) override
    {
        trace("sent preset", index + 1);
        ble::desk::preset(index);
    }
    void store(int index) override { ble::desk::store(index); }

    bool goto_height(int height_mm) override
    {
        ble::desk::goto_height(height_mm);
        return true;
    }

    void wake() override
    {
        ESP_LOGI(TAG, "waking the desk through the proxy");
        ble::desk::wake();
    }

    // A wake needs the companion there to carry it.
    bool can_wake() const override { return ble::desk::connected(); }

    int  motion() const override { return s_motion.load(std::memory_order_relaxed); }
    bool travelling() const override { return s_driving.load(std::memory_order_relaxed); }

    const char *status(TickType_t, bool, int) override
    {
        if (!ble::desk::connected()) {
            return "no proxy in range";
        }
        deskproto::Status status{};
        if (!ble::desk::last(status)) {
            return "proxy not reporting";
        }
        return status.linked ? kConnected : "proxy up, control box silent";
    }

    const char *name() const override { return "bluetooth"; }
};

}  // namespace

Transport &bluetooth()
{
    static Bluetooth link;
    return link;
}

}  // namespace desk::detail
