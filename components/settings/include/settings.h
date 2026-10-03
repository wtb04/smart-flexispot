#pragma once

#include "esp_err.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>

namespace settings {
enum class Key : std::uint8_t {
    Brightness,
    Charging,
    Volume,
    PresenceGate,
    DeskBluetooth,
    Accent,
    DockSide,
    Flipped,
    OrientAuto,  // follow the IMU rather than Flipped
    OrientSign,  // which way along the IMU's x gravity points with the screen upright, -1 or 1
    FocusWork,   // minutes of the focus timer's parts, and rounds before the long break
    FocusBreak,
    FocusLong,
    FocusRounds,
    FocusPhase,    // the timer as it stood, kept across a restart: see focus::Saved
    FocusRound,
    FocusRunning,
    FocusLeft,     // milliseconds
    FocusLength,
    FocusEnds,     // seconds since the epoch, 0 when not running or not known
    BatteryCharge, // mAh the gauge counted, kept across a restart; -1 for none
    RadarRange,    // km the radar was last zoomed to
    Count,
};

/** Reads what was stored. Call once, before anything asks for a value. */
esp_err_t load();

int get(Key key);

bool enabled(Key key);

/** Takes effect at once and is written back shortly after; a run of changes,
 *  such as a slider being dragged, costs one write rather than one per step. */
void set(Key key, int value);

/** Writes back now whatever is waiting, for when the panel is about to restart. */
void flush();

// ---- Kept by the components themselves ----
//
// A value a component keeps across restarts, declared where it is used rather
// than listed above: any plain struct or array, under its own namespace and key,
// so what was stored before stays readable.
//
//     settings::Record<std::array<int, 6>> s_presets{"desk", "presets2", {-1, -1, -1, -1, -1, -1}};
//     s_presets.set(heights);            // written a moment after the last change
//     s_presets.set(heights, Write::Now) // or before this returns
//
// Read from flash the first time it is asked for, after load(). Thread-safe.

enum class Write : std::uint8_t {
    Soon,  // with the rest, a moment after the last change: a run of changes is one write
    Now,   // before set() returns, as before a restart; from a task with its stack in internal RAM
};

namespace detail {
class Slot;
/** What was stored, in bytes, into the slot's value if it will do; false keeps the default. */
using Accept = bool (*)(Slot &slot, const void *bytes, std::size_t size);

class Slot {
public:
    Slot(const char *space, const char *key, void *value, std::size_t size, Accept accept);
    Slot(const Slot &)            = delete;
    Slot &operator=(const Slot &) = delete;

protected:
    void read(void *out);
    void write(const void *in, Write when);

private:
    friend void commit_slots();
    void load_locked();
    void store_locked();

    const char *space_;
    const char *key_;
    void       *value_;
    std::size_t size_;
    Accept      accept_;
    std::mutex  lock_;
    bool        loaded_ = false;
    bool        dirty_  = false;
    Slot       *next_   = nullptr;
};
}  // namespace detail

template <typename T>
class Record : detail::Slot {
    static_assert(std::is_trivially_copyable_v<T>, "stored as its bytes");

public:
    /** `accept` reads what an older build stored, of another size; without it
     *  only a value of the same size is taken. */
    Record(const char *space, const char *key, T fallback,
           bool (*accept)(const void *bytes, std::size_t size, T &out) = nullptr)
        : detail::Slot(space, key, &value_, sizeof(T), accept == nullptr ? nullptr : accept_thunk),
          value_(fallback), accept_(accept)
    {
    }

    T get()
    {
        T out;
        read(&out);
        return out;
    }

    void set(const T &value, Write when = Write::Soon) { write(&value, when); }

private:
    static bool accept_thunk(detail::Slot &slot, const void *bytes, std::size_t size)
    {
        auto &self = static_cast<Record &>(slot);
        return self.accept_(bytes, size, self.value_);
    }

    T value_;
    bool (*accept_)(const void *bytes, std::size_t size, T &out);
};

}  // namespace settings
