#pragma once

#include <cstdint>

namespace desk::detail {

/**
 * Where one of the box's presets stops, learned from a single clean run of it:
 * the box keeps its presets to itself, so a tap sends the key and the panel
 * watches where the desk comes to rest. Only a run that leaves within moments,
 * goes one way and is asked nothing else on the way is learned from, and the
 * lesson ends at the first rest whatever it holds: a later move, a travel or a
 * hold, is never taken for the preset, as it once was while the panel kept
 * watching for 45 seconds. Pure, so the host tests can run it.
 */
class Lesson {
public:
    static constexpr std::uint32_t LEAVE_MS    = 4000;   // the box answers a key within this, or was at the preset already
    static constexpr std::uint32_t SETTLE_MS   = 1500;   // still this long: the run is over
    static constexpr std::uint32_t LONGEST_MS  = 40000;  // longer than any run across the desk's range
    static constexpr int           DEPARTED_MM = 8;
    static constexpr int           BACK_MM     = 2;      // what the readings jitter by against the way it goes

    /** The preset `index` was sent with the desk at `from_mm`; `asked` counts
     *  every command to the desk, this one included. */
    void start(int index, int from_mm, std::uint32_t asked, std::uint32_t now_ms);
    void end(const char *why);
    bool active() const { return index_ >= 0; }

    /** Each tick: the height, negative while not known; the commands counted so
     *  far; and how long the height has not changed. The preset's height once,
     *  as the lesson ends with it learned, and negative otherwise. */
    int step(int height_mm, std::uint32_t asked, std::uint32_t now_ms, std::uint32_t still_ms);

    int         index() const { return index_; }
    const char *why() const { return why_; }  // how the last lesson ended

private:
    int           index_   = -1;
    int           from_    = -1;
    int           last_    = -1;
    int           dir_     = 0;  // 0 until it leaves, then +1 up or -1 down
    std::uint32_t asked_   = 0;
    std::uint32_t started_ = 0;
    const char   *why_     = "";
};

}  // namespace desk::detail
