#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Message-thread presentation only; never modifies the sound engine's EG.
class NativeMeterDecay
{
public:
    std::array<unsigned, 16> update (const std::array<uint16_t, 16>& targets,
                                     double nowSeconds) noexcept
    {
        const double elapsed = initialised ? std::max (0.0, nowSeconds - lastTime) : 0.0;
        lastTime = nowSeconds;
        initialised = true;
        std::array<unsigned, 16> bars {};
        for (std::size_t part = 0; part < levels.size(); ++part)
        {
            const auto target = (unsigned (targets[part]) + 4095u) / 4096u;
            // Immediate attack, one display segment per 40 ms on release.
            // UI ballistics, not a claim about the original hardware timing.
            levels[part] = std::max (double (target), levels[part] - elapsed / 0.040);
            bars[part] = unsigned (std::ceil (levels[part]));
        }
        return bars;
    }

private:
    std::array<double, 16> levels {};
    double lastTime = 0.0;
    bool initialised = false;
};
