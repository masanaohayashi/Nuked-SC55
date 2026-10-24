#pragma once
#include <cstdint>

namespace sc55
{
// One stereo output frame, full-scale 32-bit.
struct Frame
{
    int32_t left = 0, right = 0;
};
}
