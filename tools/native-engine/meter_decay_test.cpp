#include "../../Plugins/Source/NativeMeterDecay.h"
#include <cstdlib>
#include <iostream>

static void check (bool condition)
{
    if (! condition) std::abort();
}

int main()
{
    NativeMeterDecay meter;
    std::array<uint16_t, 16> targets {};
    check (meter.update (targets, 0.0)[0] == 0);
    targets[0] = 65535;
    targets[15] = 1;
    auto bars = meter.update (targets, 0.1);
    check (bars[0] == 16 && bars[15] == 1 && bars[1] == 0);
    targets.fill (0);
    bars = meter.update (targets, 0.12);
    check (bars[0] == 16 && bars[15] == 1); // No instantaneous clearing.
    bars = meter.update (targets, 0.15);
    check (bars[0] == 15 && bars[15] == 0);
    check (meter.update (targets, 0.15) == bars); // Duplicate paint does not decay.
    check (meter.update (targets, 0.751)[0] == 0); // Audio need not advance.
    targets[0] = 32768;
    check (meter.update (targets, 0.76)[0] == 8); // Immediate retrigger.
    targets[0] = 8192;
    check (meter.update (targets, 2.0)[0] == 2); // Never undershoot held target.

    NativeMeterDecay fast, slow;
    targets[0] = 65535;
    fast.update (targets, 0.0);
    slow.update (targets, 0.0);
    targets.fill (0);
    for (int i = 1; i < 10; ++i) fast.update (targets, i * 0.017);
    check (fast.update (targets, 0.175) == slow.update (targets, 0.175));
    std::cout << "Meter decay: attack, gradual release, zero, retrigger, independent parts, polling cadence PASS\n";
}
