#include "power_gauge.h"

#include <gtest/gtest.h>
#include <cmath>
#include <cstdlib>

using namespace power;

TEST(Gauge, voltage_curve)
{
    EXPECT_TRUE(fraction_at(4.2f) == 1.0f && fraction_at(3.0f) == 0.0f) << "above full is full, below empty empty";
    EXPECT_LT(std::fabs(fraction_at(3.944f) - 0.50f), 0.001f) << "the measured middle";
    EXPECT_LT(std::fabs(fraction_at(3.8365f) - 0.25f), 0.01f) << "between points, in proportion";
}

TEST(Gauge, counts_what_is_drawn)
{
    Gauge g;
    start(g, 1.0f);
    for (int s = 0; s < 3600; ++s) {
        step(g, 0.190f, 1.0f, 0.95f);  // an hour at a tenth of the capacity, the voltage agreeing
    }
    EXPECT_LE(std::abs(percent(g) - 90), 1) << "an hour at 190 mA takes a tenth";
}

TEST(Gauge, flat_middle_trusts_the_count)
{
    Gauge flat;
    start(flat, 0.70f);
    for (int s = 0; s < 600; ++s) {
        step(flat, 0.0f, 1.0f, 0.50f);  // the flat middle reading low for ten minutes
    }
    EXPECT_GE(percent(flat), 68) << "in the flat middle the voltage barely moves the count";
}

TEST(Gauge, near_empty_trusts_the_voltage)
{
    Gauge low;
    start(low, 0.30f);
    for (int s = 0; s < 1800; ++s) {
        step(low, 0.0f, 1.0f, 0.05f);  // near empty the voltage is believed
    }
    EXPECT_LE(percent(low), 8) << "near empty the voltage pulls the count down";
}

TEST(Gauge, charging_adds)
{
    Gauge charging;
    start(charging, 0.50f);
    for (int s = 0; s < 3600; ++s) {
        step(charging, -0.950f, 1.0f, charging.charge_mah / kCapacityMah);  // an hour at half the capacity
    }
    EXPECT_TRUE(percent(charging) > 90 && percent(charging) <= 100) << "charging adds, a little less than it takes";
    full(charging);
    EXPECT_EQ(percent(charging), 100) << "full is full";
}

TEST(Gauge, starts_where_the_voltage_says)
{
    Gauge fresh;
    step(fresh, 0.1f, 1.0f, 0.42f);
    EXPECT_TRUE(fresh.known && percent(fresh) == 42) << "unknown, it starts where the voltage says";
}
