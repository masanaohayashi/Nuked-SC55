#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace sc55
{
// Logical voice identities, independent of the PCM chip's 28 writable key bits.
// No implicit integer conversion: the hardware mask must never silently truncate
// logical voices 32..127. Portable to MSVC as well as Clang (no __int128).
class VoiceSet
{
public:
    static constexpr unsigned capacity = 128;

    constexpr bool contains(unsigned voice) const noexcept
    {
        return voice < capacity && (words_[voice / 32] & (uint32_t(1) << (voice % 32))) != 0;
    }
    constexpr bool set(unsigned voice, bool enabled = true) noexcept
    {
        if (voice >= capacity) return false;
        const auto bit = uint32_t(1) << (voice % 32);
        if (enabled) words_[voice / 32] |= bit;
        else words_[voice / 32] &= ~bit;
        return true;
    }
    constexpr bool empty() const noexcept
    { return (words_[0] | words_[1] | words_[2] | words_[3]) == 0; }
    constexpr unsigned count() const noexcept
    {
        unsigned result = 0;
        for (const auto word : words_) result += std::popcount(word);
        return result;
    }
    constexpr void clear() noexcept { words_.fill(0); }
    constexpr bool operator==(const VoiceSet&) const noexcept = default;

    constexpr VoiceSet& operator|=(const VoiceSet& other) noexcept
    {
        for (unsigned i = 0; i < words_.size(); ++i) words_[i] |= other.words_[i];
        return *this;
    }
    constexpr VoiceSet& operator&=(const VoiceSet& other) noexcept
    {
        for (unsigned i = 0; i < words_.size(); ++i) words_[i] &= other.words_[i];
        return *this;
    }
    constexpr void remove(const VoiceSet& other) noexcept
    {
        for (unsigned i = 0; i < words_.size(); ++i) words_[i] &= ~other.words_[i];
    }

private:
    std::array<uint32_t, 4> words_ {};
};
}
