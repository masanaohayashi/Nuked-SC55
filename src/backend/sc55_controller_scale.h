#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace sc55
{
struct ControllerScaleResult
{
    uint16_t value, productLow;
    unsigned instructions;
    bool carry;
};

// One field of 5c46..5ff4, through its final store (excluding the part-pointer
// reload). Keep 8-bit subtraction, wrapping additions and magnitude truncation.
inline ControllerScaleResult ScaleVoiceController(unsigned i, uint8_t sensitivity,
    uint8_t key, const std::array<uint16_t,5>& contributions) noexcept
{
    const bool centered = i <= 3 || i == 7;
    const bool negativeInput = centered && sensitivity < 64;
    const unsigned magnitude = centered ? (negativeInput ? 64-sensitivity : sensitivity-64) : sensitivity;
    const unsigned shift = i == 0 ? 0 : centered ? 1 : 2;
    const auto initial = uint16_t((magnitude*key)>>shift);
    uint16_t sum = negativeInput ? uint16_t(0u-initial) : initial;
    for (auto value : contributions) sum = uint16_t(sum+value);
    const bool negative = (sum&32768) != 0;
    const uint16_t absolute = negative ? uint16_t(0u-sum) : sum;
    const bool rate = i == 3 || i == 7;
    const bool level = i == 2 || i == 6 || i == 10;
    const bool envelope = i == 5 || i == 9;
    const unsigned cap = i == 0 ? 0x0be8 : i <= 2 || rate ? 0x0fa0 : 0x0fc0;
    const unsigned leftShift = i <= 1 ? 3 : level ? 4 : 1;
    const unsigned coefficient = i == 0 ? 0xfbf8 : i == 1 ? 0xc49c : i == 2 ? 0x820d
        : rate ? 0xa7c7 : level ? 0x8105 : envelope ? 0xc30d : 0xbe7a;
    const uint32_t product = uint32_t(uint16_t(std::min(unsigned(absolute),cap)<<leftShift))*coefficient;
    const auto high = uint16_t(product>>16);
    // load; optional sub/branch; multiply/shift/sign; five adds/branch;
    // abs/compare/clamp/scale/sign; store. No device cycles are skipped.
    const unsigned instructions = 1 + (centered ? 2 : 0)
        + (negativeInput ? 4 : 1) + shift + 6
        + (negative ? 3 : 0) + 2 + unsigned(absolute >= cap) + leftShift + 1 + 1;
    return {negative ? uint16_t(0u-high) : high, uint16_t(product), instructions, negative && high != 0};
}
} // namespace sc55
