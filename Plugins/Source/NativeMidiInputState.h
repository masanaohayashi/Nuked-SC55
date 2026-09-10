#pragma once
#include "sc55_sysex.h"
#include <atomic>
#include <optional>
#include <algorithm>
#include <charconv>
#include <string_view>

// Version1 session value and lock-free handoff. Audio publishes applied state;
// a host restore wins over an overlapping publication and is consumed on audio.
// No UI/component/state-tree access is needed by the audio owner.
class NativeMidiInputState
{
public:
    static constexpr uint32_t defaultValue=0x310;
    static constexpr uint32_t encode(sc55::MidiInputSettings settings) noexcept
    {
        return uint32_t(std::min<unsigned>(settings.deviceId,31))
            | (uint32_t(settings.receiveExclusive)<<8)
            | (uint32_t(settings.receiveReset)<<9)
            | (uint32_t(settings.ignoreChecksum)<<10)
            | (uint32_t(!settings.receiveProgramChanges)<<11);
    }
    static std::optional<sc55::MidiInputSettings> decode(int64_t value) noexcept
    {
        if(value<0 || (uint64_t(value)&~uint64_t(0xf1f))!=0) return {};
        // Disabled bit: sessions saved before this option keep reception on.
        return sc55::MidiInputSettings{uint8_t(value&31),bool(value&0x100),bool(value&0x200),bool(value&0x400),!(value&0x800)};
    }
    // JUCE's XML-backed ValueTree restores attributes as strings. Parse the
    // entire decimal value, rejecting overflow, suffixes and unknown bits.
    static std::optional<uint32_t> parse(std::string_view text) noexcept
    {
        if(text.empty()) return {};
        uint32_t value=0;
        const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
        if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size() || !decode(value)) return {};
        return value;
    }
    uint32_t encoded() const noexcept {return state_.load(std::memory_order_acquire)&~pendingRestore;}
    sc55::MidiInputSettings read() const noexcept {return *decode(encoded());}
    void restore(int64_t value) noexcept
    {state_.store(encode(decode(value).value_or(sc55::MidiInputSettings{}))|pendingRestore,std::memory_order_release);}
    // Audio-owner only; one bounded CAS, never waits for a concurrent restore.
    std::optional<sc55::MidiInputSettings> takeRestore() noexcept
    {
        auto value=state_.load(std::memory_order_acquire);
        if(!(value&pendingRestore)) return {};
        if(!state_.compare_exchange_strong(value,value&~pendingRestore,std::memory_order_acq_rel)) return {};
        return decode(value&~pendingRestore);
    }
    void publish(sc55::MidiInputSettings settings) noexcept
    {
        auto previous=state_.load(std::memory_order_acquire);
        if(!(previous&pendingRestore))
            (void)state_.compare_exchange_strong(previous,encode(settings),std::memory_order_acq_rel);
    }
private:
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static constexpr uint32_t pendingRestore=0x80000000;
    std::atomic<uint32_t> state_{defaultValue};
};
