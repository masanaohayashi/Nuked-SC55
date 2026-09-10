#pragma once
#include <cstddef>

// Shared stereo value type; independent of chip arithmetic and host formats.
template <typename T>
struct AudioFrame
{
    T left;
    T right;

    static constexpr std::size_t channel_count = 2;
};
