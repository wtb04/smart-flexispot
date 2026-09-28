#include "power_gauge.h"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}
}  // namespace

int main()
{
    using namespace power;
    check(fraction_at(4.2f) == 1.0f && fraction_at(3.0f) == 0.0f, "above full is full, below empty empty");
    check(std::fabs(fraction_at(3.944f) - 0.50f) < 0.001f, "the measured middle");
    check(std::fabs(fraction_at(3.8365f) - 0.25f) < 0.01f, "between points, in proportion");

    Gauge g;
    start(g, 1.0f);
    for (int s = 0; s < 3600; ++s) {
        step(g, 0.190f, 1.0f, 0.95f);  // an hour at a tenth of the capacity, the voltage agreeing
    }
    check(std::abs(percent(g) - 90) <= 1, "an hour at 190 mA takes a tenth");

    Gauge flat;
    start(flat, 0.70f);
    for (int s = 0; s < 600; ++s) {
        step(flat, 0.0f, 1.0f, 0.50f);  // the flat middle reading low for ten minutes
    }
    check(percent(flat) >= 68, "in the flat middle the voltage barely moves the count");

    Gauge low;
    start(low, 0.30f);
    for (int s = 0; s < 1800; ++s) {
        step(low, 0.0f, 1.0f, 0.05f);  // near empty the voltage is believed
    }
    check(percent(low) <= 8, "near empty the voltage pulls the count down");

    Gauge charging;
    start(charging, 0.50f);
    for (int s = 0; s < 3600; ++s) {
        step(charging, -0.950f, 1.0f, charging.charge_mah / kCapacityMah);  // an hour at half the capacity
    }
    check(percent(charging) > 90 && percent(charging) <= 100, "charging adds, a little less than it takes");
    full(charging);
    check(percent(charging) == 100, "full is full");

    Gauge fresh;
    step(fresh, 0.1f, 1.0f, 0.42f);
    check(fresh.known && percent(fresh) == 42, "unknown, it starts where the voltage says");

    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
