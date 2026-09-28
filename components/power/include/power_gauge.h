#pragma once

#include <cstddef>

// How full the pack is. Its voltage says little for most of the way down --
// between 90 and 70 percent a cell moves 25 mV -- so the charge is counted
// from the current as it flows, and the voltage only pulls the count back:
// hard near empty, where it falls steeply and can be trusted, and barely in
// the flat middle. Pure, so the host tests can run it.
namespace power {
inline constexpr float kCapacityMah = 1900.0f;

struct CellPoint {
    float volts;     // one cell of the two, at rest
    float fraction;  // of the capacity left
};

// Measured off two full discharges of this pack in Home Assistant's history,
// September 2026: one with the screen off throughout, so at a steady 120 mA,
// and one mixed; the two agree to some 20 mV. Charge left taken as the time
// left, corrected to rest by the pack's resistance.
inline constexpr CellPoint kCellCurve[] = {
    {4.089f, 1.00f}, {4.069f, 0.95f}, {4.056f, 0.90f}, {4.044f, 0.85f}, {4.036f, 0.80f},
    {4.026f, 0.70f}, {3.999f, 0.60f}, {3.944f, 0.50f}, {3.896f, 0.40f}, {3.859f, 0.30f},
    {3.814f, 0.20f}, {3.774f, 0.15f}, {3.716f, 0.10f}, {3.664f, 0.07f}, {3.616f, 0.05f},
    {3.509f, 0.03f}, {3.364f, 0.01f}, {3.200f, 0.00f},
};

inline float fraction_at(float cell_volts)
{
    constexpr std::size_t count = sizeof(kCellCurve) / sizeof(kCellCurve[0]);
    if (cell_volts >= kCellCurve[0].volts) {
        return 1.0f;
    }
    for (std::size_t i = 1; i < count; ++i) {
        const CellPoint &hi = kCellCurve[i - 1];
        const CellPoint &lo = kCellCurve[i];
        if (cell_volts >= lo.volts) {
            const float t = (cell_volts - lo.volts) / (hi.volts - lo.volts);
            return lo.fraction + t * (hi.fraction - lo.fraction);
        }
    }
    return 0.0f;
}

/** How long the voltage takes to pull the count to itself, by how full it says
 *  the pack is. */
inline float pull_seconds(float by_volts)
{
    constexpr float STEEP   = 0.12f;
    constexpr float SLOPING = 0.25f;
    return by_volts < STEEP ? 10.0f * 60.0f : by_volts < SLOPING ? 60.0f * 60.0f : 6.0f * 3600.0f;
}

struct Gauge {
    float charge_mah = 0.0f;
    bool  known      = false;
};

inline float clamped(float mah)
{
    return mah < 0.0f ? 0.0f : mah > kCapacityMah ? kCapacityMah : mah;
}

inline void start(Gauge &gauge, float fraction)
{
    gauge.charge_mah = clamped(fraction * kCapacityMah);
    gauge.known      = true;
}

/** `amps` positive on discharge, over `seconds`; `by_volts` what the voltage
 *  at rest says is left. Charging puts back a little less than it takes. */
inline void step(Gauge &gauge, float amps, float seconds, float by_volts)
{
    constexpr float CHARGE_EFFICIENCY = 0.95f;
    constexpr float SECONDS_PER_HOUR  = 3600.0f;
    constexpr float MA_PER_A          = 1000.0f;
    if (!gauge.known) {
        start(gauge, by_volts);
        return;
    }
    const float used = amps * MA_PER_A * seconds / SECONDS_PER_HOUR;
    gauge.charge_mah = clamped(gauge.charge_mah - (used > 0.0f ? used : used * CHARGE_EFFICIENCY));
    const float pull = seconds / pull_seconds(by_volts);
    gauge.charge_mah += (by_volts * kCapacityMah - gauge.charge_mah) * (pull < 1.0f ? pull : 1.0f);
}

inline void full(Gauge &gauge)
{
    start(gauge, 1.0f);
}

inline int percent(const Gauge &gauge)
{
    return static_cast<int>(gauge.charge_mah / kCapacityMah * 100.0f + 0.5f);
}

}  // namespace power
