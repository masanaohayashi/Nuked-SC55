#pragma once

#include "math_util.h"
#include "audio_frame.h"
#include <cstddef>
#include <cstdint>

enum class AudioFormat
{
    S16,
    S32,
    F32,
};

inline void Normalize(const AudioFrame<int32_t>& in, AudioFrame<int16_t>& out)
{
    out.left  = (int16_t)Clamp<int32_t>(in.left >> 15, INT16_MIN, INT16_MAX);
    out.right = (int16_t)Clamp<int32_t>(in.right >> 15, INT16_MIN, INT16_MAX);
}

inline void Normalize(const AudioFrame<int32_t>& in, AudioFrame<int32_t>& out)
{
    out.left  = (int32_t)Clamp<int64_t>((int64_t)in.left << 1, INT32_MIN, INT32_MAX);
    out.right = (int32_t)Clamp<int64_t>((int64_t)in.right << 1, INT32_MIN, INT32_MAX);
}

inline void Normalize(const AudioFrame<int32_t>& in, AudioFrame<float>& out)
{
    constexpr float DIV_REC = 1.0f / 536870912.0f;

    out.left  = (float)in.left * DIV_REC;
    out.right = (float)in.right * DIV_REC;
}

inline void MixFrame(AudioFrame<int16_t>& dest, const AudioFrame<int16_t>& src)
{
    dest.left  = SaturatingAdd(dest.left, src.left);
    dest.right = SaturatingAdd(dest.right, src.right);
}

inline void MixFrame(AudioFrame<int32_t>& dest, const AudioFrame<int32_t>& src)
{
    dest.left  = SaturatingAdd(dest.left, src.left);
    dest.right = SaturatingAdd(dest.right, src.right);
}

inline void MixFrame(AudioFrame<float>& dest, const AudioFrame<float>& src)
{
    dest.left  += src.left;
    dest.right += src.right;
}

template <typename SampleT>
void Scale(AudioFrame<SampleT>& frame, float scalar_gain)
{
    frame.left  = SaturatingMul(frame.left, scalar_gain);
    frame.right = SaturatingMul(frame.right, scalar_gain);
}

inline void Scale(AudioFrame<float>& frame, float scalar_gain)
{
    frame.left  = frame.left * scalar_gain;
    frame.right = frame.right * scalar_gain;
}
