#pragma once
#include "sc55_envelope_runner.h"
#include "sc55_voice_links.h"
#include "sc55_envelope_ramp.h"
#include <optional>

namespace sc55
{
enum class VoiceReuseReadiness { ready, pending, cancelled };
inline VoiceReuseReadiness CheckVoiceReuse(uint16_t level32,uint16_t level34,uint8_t flagCAF4) noexcept
{
    if (flagCAF4 != 0) return VoiceReuseReadiness::cancelled;
    return level32 == 0 || level34 == 0 ? VoiceReuseReadiness::ready : VoiceReuseReadiness::pending;
}
// One iteration of 5710..573e. Firmware yields with TRAPA0 when pending;
// native scheduling must retry later, never spin in the audio callback.
// Caller owns serialized I/O and supplies the current CAF4 state.
template<class Read,class Write>
std::optional<VoiceReuseReadiness> PollVoiceReuse(uint8_t channel,uint8_t flagCAF4,Read&& read,Write&& write)
{
    if (channel >= 24) return std::nullopt;
    if constexpr(requires { write.voiceGainLevels(channel); })
    {
        const auto levels=write.voiceGainLevels(channel);
        return CheckVoiceReuse(levels[0],levels[1],flagCAF4);
    }
    write(uint8_t(0x3e),channel);
    const auto level = [&](uint8_t a) {
        (void)read(a); const auto hi = read(uint8_t(0x3a)); const auto lo = read(uint8_t(0x3b));
        return uint16_t((hi<<8)|lo);
    };
    const auto first = level(0x32), second = level(0x34);
    return CheckVoiceReuse(first,second,flagCAF4);
}

struct VoiceStopPlan
{
    uint8_t pcmAddress, cachedWordOffset;
    uint16_t stageCode;
};
// v1.21 5404..542f. Both readings are unsigned raw PCM levels. Equal levels
// select 18. All three firmware stage words become stageCode; the selected
// cached command (+1a or +1e) becomes 00b6. These are special stopping stages,
// not the first-envelope runner's normal release/finished states.
inline VoiceStopPlan PrepareVoiceStop(uint16_t level32,uint16_t level34) noexcept
{
    return level34 < level32 ? VoiceStopPlan{0x16,0x1a,0x12} : VoiceStopPlan{0x18,0x1e,0x14};
}

// PCM portion of 53e6..542f; caller clears CB30, commits the plan's stage/cache
// fields, and sets CAF4=4 before reclaiming. This starts a ramp, not immediate
// silence. Caller owns serialized device I/O; invalid channel performs no I/O.
template<class Read,class Write>
std::optional<VoiceStopPlan> StopVoicePcm(uint8_t channel,Read&& read,Write&& write)
{
    if (channel >= 24) return std::nullopt;
    if constexpr(requires { write.voiceGainLevels(channel);
        write.setVoiceRamp(channel,EnvelopeRamp::Stage::firstGain,uint16_t(0)); }) {
        const auto levels=write.voiceGainLevels(channel);
        const auto plan=PrepareVoiceStop(levels[0],levels[1]);
        write.setVoiceRamp(channel,levels[1]<levels[0] ? EnvelopeRamp::Stage::firstGain
            : EnvelopeRamp::Stage::secondGain,0xb6);
        return plan;
    }
    else {
    write(uint8_t(0x3e),channel);
    const auto level = [&](uint8_t address) {
        (void)read(address);
        const auto hi = read(uint8_t(0x3a));
        const auto lo = read(uint8_t(0x3b));
        return uint16_t((hi<<8)|lo);
    };
    const auto first = level(0x32);
    const auto second = level(0x34);
    const auto plan = PrepareVoiceStop(first,second);
    write(plan.pcmAddress,uint8_t(0));
    write(uint8_t(plan.pcmAddress+1),uint8_t(0xb6));
    return plan;
    }
}

struct EnvelopePcmSync
{
    uint8_t address;
    uint16_t word;
    bool readLevel;
};
inline EnvelopePcmSync PrepareEnvelopePcmSync(const EnvelopeRunner& runner) noexcept
{
    return runner.state().pcmWord == 0xff00
        ? EnvelopePcmSync{0x34,uint16_t(runner.state().level >> 1),false}
        : EnvelopePcmSync{0x18,0xff00,true};
}
inline uint16_t DecodeEnvelopePcmLevel(uint16_t registerValue) noexcept
{ return uint16_t(registerValue << 1); }

template<class Write>
void WriteEnvelopePcm(const EnvelopeRunner& runner,Write&& write)
{
    write(uint8_t(0x18),uint8_t(runner.state().pcmWord >> 8));
    write(uint8_t(0x19),uint8_t(runner.state().pcmWord));
}

// v1.21 33c9..33d0, only the PCM portion of voice termination.
// It ramps to zero with b6, not an immediate register-level zero or mask
// removal. Allocation links and the firmware's following event are separate.
template<class Write>
bool WriteEnvelopeTermination(uint8_t physicalChannel,Write&& write)
{
    if (physicalChannel >= 32) return false;
    if constexpr(requires { write.setVoiceRamp(physicalChannel,EnvelopeRamp::Stage::secondGain,uint16_t(0)); })
        write.setVoiceRamp(physicalChannel,EnvelopeRamp::Stage::secondGain,0xb6);
    else {
    write(uint8_t(0x3e),physicalChannel);
    write(uint8_t(0x18),uint8_t(0));
    write(uint8_t(0x19),uint8_t(0xb6));
    }
    return true;
}

enum class EnvelopeTermination { bypassed, waiting, notifyAllocator };
// 3393..33d4, also reached directly from the natural-envelope exit3472.
// That exit already knows its logical envelope is zero and does not poll PCM.
template<class Write>
bool FinishEnvelopeTermination(unsigned channel,uint16_t& stage,uint8_t& activity,
    VoiceLinks& links,Write&& write)
{
    if (!links.detach(channel)) return false;
    activity = 0; stage = 22;
    return WriteEnvelopeTermination(uint8_t(channel),write);
}
// 3363..33d4. A finished envelope is not proof of PCM silence. Stage0e
// polls32, stage10 polls34; other stages bypass without any I/O. Only an
// exactly zero raw level detaches links and permits the allocator event.
template<class Read,class Write>
std::optional<EnvelopeTermination> PollEnvelopeTermination(unsigned channel,
    uint16_t& stage,uint8_t& activity,VoiceLinks& links,Read&& read,Write&& write)
{
    if (channel >= 24) return std::nullopt;
    if (stage != 14 && stage != 16) return EnvelopeTermination::bypassed;
    const auto level=[&]() -> uint16_t {
    if constexpr(requires { write.voiceRampLevel(uint8_t(channel),EnvelopeRamp::Stage::firstGain); })
        return write.voiceRampLevel(uint8_t(channel),stage==14 ? EnvelopeRamp::Stage::firstGain : EnvelopeRamp::Stage::secondGain);
    else {
    write(uint8_t(0x3e),uint8_t(channel));
    (void)read(uint8_t(stage == 14 ? 0x32 : 0x34));
    const auto hi = read(uint8_t(0x3a));
    const auto lo = read(uint8_t(0x3b));
    return uint16_t((hi<<8)|lo);
    }
    }();
    if (level)
    {
        activity = uint8_t(uint16_t(level*2)>>8);
        if (activity == 255) activity = 254;
        return EnvelopeTermination::waiting;
    }
    if (!FinishEnvelopeTermination(channel,stage,activity,links,write)) return std::nullopt;
    return EnvelopeTermination::notifyAllocator;
}

// v1.21 31c7..31e5. Caller selects the physical PCM channel and owns its
// serialized I/O. Read/write callbacks must be bounded and allocation-free.
// Read 34 first to latch the current level, then obtain its bytes from 3a/3b.
template<class Read,class Write>
uint16_t SynchronizePcmRamp(uint16_t command,uint16_t level,uint8_t commandAddress,
    uint8_t levelAddress,Read&& read,Write&& write)
{
    const auto address = command == 0xff00 ? levelAddress : commandAddress;
    const uint16_t value = command == 0xff00 ? uint16_t(level>>1) : uint16_t(0xff00);
    write(address,uint8_t(value>>8)); write(uint8_t(address+1),uint8_t(value));
    if (command != 0xff00)
    {
        (void)read(levelAddress);
        const auto high = read(uint8_t(0x3a));
        const auto low = read(uint8_t(0x3b));
        return DecodeEnvelopePcmLevel(uint16_t((high<<8)|low));
    }
    return level;
}

template<class Read,class Write>
void SynchronizeEnvelopePcm(EnvelopeRunner& runner,Read&& read,Write&& write)
{
    runner.synchronizePcmLevel(SynchronizePcmRamp(runner.state().pcmWord,runner.state().level,
        0x18,0x34,read,write));
}
}
