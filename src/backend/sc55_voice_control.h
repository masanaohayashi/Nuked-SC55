#pragma once
#include "sc55_voice_lifecycle.h"
#include "sc55_sound_data.h"

namespace sc55
{
// One physical voice's native control owners. Modulation sharing arrays live
// at the instrument owner because their sources span multiple voices.
struct VoiceControlState
{
    EnvelopeRunner amplitude;
    VoiceReleaseAuxiliary release;
    VoicePitchRunner pitch;
    SecondEnvelopePcmState second;
    VoiceOutputState output;
    VoiceStopState lifecycle;
    PreparedVoicePcm prepared;
    uint32_t alternatePitchReference = 0; // voice2a:40, selected at first PCM loop IRQ
    bool stopAtSampleEnd = false; // descriptor0a bit1, voice[-17]
};

struct VoiceControlInputs
{
    uint16_t ticks = 1;
    EnvelopeRunner::Controls amplitude;
    bool secondBypass = false;
    SecondEnvelopeTiming secondTiming;
    SecondEnvelopeOutputInputs second;
    uint8_t secondBase = 64, secondController = 64, secondLimit = 127;
    PitchModulationInputs pitch;
    uint8_t glideRate = 0, correctionSource = 0;
    uint32_t pitchReference = 0;
    LevelInputs level;
    SpatialInputs spatial;
};

// 2928..29d9: PCM boundary event, not a periodic envelope tick. The caller
// acknowledges the IRQ and serializes this with installation/startup.
template<class Read,class Write>
bool HandleVoicePcmBoundary(unsigned channel,VoiceControlState& voice,
    VoiceControlInputs& inputs,VoiceLinks& links,const PitchConversion& conversion,
    Read&& read,Write&& write)
{
    if(channel>=24) return false;
    if(voice.lifecycle.stages[0]>=14) return true;
    if(voice.stopAtSampleEnd) {
        auto nextLinks=links;
        if(!nextLinks.detach(channel)) return false;
        const auto plan=StopVoicePcm(uint8_t(channel),read,write);
        if(!plan) return false;
        if(plan->pcmAddress==0x16) voice.lifecycle.cached16=0x00b6;
        else voice.lifecycle.cached18=0x00b6;
        voice.lifecycle.stages.fill(uint16_t(plan->stageCode-4)); // 0e/10, not restart12/14
        links=nextLinks;
        return true;
    }
    inputs.pitchReference=voice.alternatePitchReference;
    auto& pitch=voice.pitch.glide.pitch;
    pitch.correction.source=0; // Retain A6 while invalidating only A4.
    voice.pitch.pcmWord=ApplyPitchOffset(conversion.fromDelta(
        pitch.accumulator-inputs.pitchReference-12000u),pitch.correction.offset);
    voice.lifecycle.pcm10=voice.pitch.pcmWord;
    if constexpr(requires { write.setVoicePitch(uint8_t(channel),voice.pitch.pcmWord); })
        write.setVoicePitch(uint8_t(channel),voice.pitch.pcmWord);
    else {
    write(uint8_t(0x3e),uint8_t(channel));
    write(uint8_t(0x10),uint8_t(voice.pitch.pcmWord>>8));
    write(uint8_t(0x11),uint8_t(voice.pitch.pcmWord));
    }
    return true;
}

// The activation batch owns the actual stage/progress changes. Continue all
// three envelopes from that one result, never independently restart them.
// Only commit-owned fields change. Invalid stage combinations preserve the
// complete voice, including its lifecycle snapshot, with no device I/O.
inline bool ContinueVoiceControlAfterStart(VoiceControlState& voice,const VoiceStopState& committed) noexcept
{
    const auto amplitude = ContinueVoiceAmplitude(committed,voice.amplitude);
    const auto second = ContinueVoiceSecondEnvelope(committed,voice.release.second);
    const auto pitch = ContinueVoicePitch(committed,voice.pitch);
    if (!amplitude || !second || !pitch) return false;
    voice.amplitude = *amplitude;
    voice.release.second = *second;
    voice.pitch = *pitch;
    voice.lifecycle = committed;
    return true;
}

enum class VoiceControlResult { invalidInput, stopped, finished, updated, suspended };
enum class VoiceControlReadback { invalidInput, stopped, ready };
enum class VoiceCalculationStage { modulation, amplitude, filter, pitch, level };

// PCM readback/hold and release entry. First modulation has already run.
// The owner validates dependencies before touching PCM and retains the voice
// until calculation/publication finish. Callbacks must not reenter. PCM time
// and MIDI scheduling remain outside these semantic control operations.
template<class Read,class Write>
VoiceControlReadback ReadVoiceControl(unsigned channel,VoiceControlState& voice,
    std::array<VoiceModulation,24>& modulation,const SoundData& data,Read&& read,Write&& write)
{
    if (channel >= 24 || !data.times() || !data.modulationRates() || !data.secondEnvelope()
        || !data.glideRates() || !data.pan()) return VoiceControlReadback::invalidInput;
    // Preserve stop-task stages0e/10 rather than reconstructing them from the
    // natural envelope runner, whose finished state represents a different exit.
    if (voice.lifecycle.stages[0] >= 14) return VoiceControlReadback::stopped;
    const auto entry = BeginVoiceUpdate(uint8_t(channel),voice.amplitude,voice.release,voice.pitch.envelope,
        voice.output.tva,voice.second,read,write);
    PrepareVoiceAmplitude(voice.lifecycle,voice.amplitude);
    modulation[channel].firstStage = voice.lifecycle.stages[0];
    return entry == VoiceUpdateEntry::stopped ? VoiceControlReadback::stopped : VoiceControlReadback::ready;
}

// Resume after ReadVoiceControl. The owner serializes these phases, preserves
// voice identity, and never repeats readback when PCM time advances between
// them. Publication remains separate; in particular a linked partner computes
// before either voice publishes. This function does not choose a delay.
template<class Read,class Write>
VoiceControlResult CalculateVoiceControlStage(VoiceCalculationStage stage,unsigned channel,VoiceControlState& voice,
    std::array<VoiceModulation,24>& modulation,std::array<uint8_t,24>& sources,
    const ModulationBlock& first,const VoiceControlInputs& inputs,const SoundData& data,
    const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write,
    VoiceModulationUpdate* continuation=nullptr)
{
    if (channel >= 24 || !data.times() || !data.modulationRates() || !data.secondEnvelope()
        || !data.glideRates() || !data.pan()) return VoiceControlResult::invalidInput;
    // H8 unmasks interrupts between these operations and rechecks the live
    // stop stage before each one (32fc/3312/3328/333e/3354). A PCM endpoint
    // must not be overwritten by reconstructing stages from an older runner.
    if(voice.lifecycle.stages[0]>=14) {
        if(continuation) *continuation={};
        return VoiceControlResult::stopped;
    }
    if(stage==VoiceCalculationStage::modulation) {
        VoiceModulationUpdate immediate;
        auto& task=continuation ? *continuation : immediate;
        modulation[channel].firstStage=voice.lifecycle.stages[0];
        using ModResult = VoiceModulationUpdate::Result;
        auto mod=ModResult::ready;
        if(!task.pending()) {
            const bool wasSharing=modulation[channel].sharing!=0;
            mod=task.begin(channel,modulation,sources);
            if(mod==ModResult::ready && wasSharing && continuation)
                return VoiceControlResult::suspended;
        }
        if (mod == ModResult::ready) mod = task.resume(modulation,inputs.ticks,*data.modulationRates(),waves,read,write);
        if (mod != ModResult::updated && mod != ModResult::shared && mod != ModResult::stageChanged)
            return VoiceControlResult::invalidInput;
        return VoiceControlResult::updated;
    }
    if(stage==VoiceCalculationStage::amplitude) {
        if (!voice.amplitude.tick(inputs.ticks,inputs.amplitude,*data.times())) return VoiceControlResult::invalidInput;
        PrepareVoiceAmplitude(voice.lifecycle,voice.amplitude);
        modulation[channel].firstStage = voice.lifecycle.stages[0];
        // 3472 discards the envelope-call return and goes directly to3393;
        // unlike the entry-stage gate, this must notify the allocator.
        if (voice.lifecycle.stages[0] >= 14) return VoiceControlResult::finished;
        return VoiceControlResult::updated;
    }
    auto level = inputs.level; auto second = inputs.second; auto pitch = inputs.pitch;
    ApplyVoiceModulationOutputs(first,modulation[channel].block,level,second,pitch);
    if(stage==VoiceCalculationStage::filter) {
        auto timing = inputs.secondTiming;
        timing.attackControlEnabled = (modulation[channel].fieldA2&16) != 0;
        const auto result = AdvanceSecondEnvelope(voice.release.second,voice.second,inputs.secondBypass,
            inputs.ticks,timing,*data.times(),second,inputs.secondBase,inputs.secondController,inputs.secondLimit,*data.secondEnvelope());
        if (result == SecondEnvelopeReleaseState::Result::invalidInput) return VoiceControlResult::invalidInput;
        voice.lifecycle.stages[1] = voice.release.second.stage;
        return VoiceControlResult::updated;
    }
    if(stage==VoiceCalculationStage::pitch) {
        if (voice.pitch.advance(inputs.ticks,false,pitch,inputs.glideRate,*data.glideRates(),inputs.pitchReference,
            inputs.correctionSource,conversion) == VoicePitchRunner::Result::invalidInput) return VoiceControlResult::invalidInput;
        PrepareVoicePitch(voice.lifecycle,voice.pitch);
        return VoiceControlResult::updated;
    }
    if(stage!=VoiceCalculationStage::level) return VoiceControlResult::invalidInput;
    if (voice.output.advance(voice.lifecycle.stages[0],level,inputs.spatial,*data.pan()) != VoiceOutputState::Result::updated)
        return VoiceControlResult::invalidInput;
    return VoiceControlResult::updated;
}

template<class Read,class Write>
VoiceControlResult CalculateVoiceControl(unsigned channel,VoiceControlState& voice,
    std::array<VoiceModulation,24>& modulation,std::array<uint8_t,24>& sources,
    const ModulationBlock& first,const VoiceControlInputs& inputs,const SoundData& data,
    const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
{
    for(auto stage:{VoiceCalculationStage::modulation,VoiceCalculationStage::amplitude,
        VoiceCalculationStage::filter,VoiceCalculationStage::pitch,VoiceCalculationStage::level}) {
        const auto result=CalculateVoiceControlStage(stage,channel,voice,modulation,sources,
            first,inputs,data,conversion,waves,read,write);
        if(result!=VoiceControlResult::updated) return result;
    }
    return VoiceControlResult::updated;
}

// Synchronous compatibility entry: the same phases, without advancing time.
template<class Read,class Write>
VoiceControlResult AdvanceVoiceControl(unsigned channel,VoiceControlState& voice,
    std::array<VoiceModulation,24>& modulation,std::array<uint8_t,24>& sources,
    const ModulationBlock& first,const VoiceControlInputs& inputs,const SoundData& data,
    const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
{
    const auto entry=ReadVoiceControl(channel,voice,modulation,data,read,write);
    if(entry==VoiceControlReadback::invalidInput) return VoiceControlResult::invalidInput;
    if(entry==VoiceControlReadback::stopped) return VoiceControlResult::stopped;
    return CalculateVoiceControl(channel,voice,modulation,sources,first,inputs,data,conversion,waves,read,write);
}
}
