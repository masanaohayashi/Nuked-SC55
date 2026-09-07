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

enum class VoiceControlResult { invalidInput, stopped, finished, updated };

// 3196..3362: PCM readback/release, second modulation, amplitude, second
// envelope, pitch, then TVA/pan/sends. First modulation is updated separately
// by its owner before this entry. This is a serialized native control step:
// callbacks must not reenter or change voice lifetimes. PCM time and MIDI event
// scheduling remain outside; final writes use UpdateVoicePcm after success.
// An invalid late-stage prepared input may leave earlier stages advanced.
template<class Read,class Write>
VoiceControlResult AdvanceVoiceControl(unsigned channel,VoiceControlState& voice,
    std::array<VoiceModulation,24>& modulation,std::array<uint8_t,24>& sources,
    const ModulationBlock& first,const VoiceControlInputs& inputs,const SoundData& data,
    const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
{
    if (channel >= 24 || !data.times() || !data.modulationRates() || !data.secondEnvelope()
        || !data.glideRates() || !data.pan()) return VoiceControlResult::invalidInput;
    // Preserve stop-task stages0e/10 rather than reconstructing them from the
    // natural envelope runner, whose finished state represents a different exit.
    if (voice.lifecycle.stages[0] >= 14) return VoiceControlResult::stopped;
    const auto entry = BeginVoiceUpdate(uint8_t(channel),voice.amplitude,voice.release,voice.pitch.envelope,
        voice.output.tva,voice.second,read,write);
    PrepareVoiceAmplitude(voice.lifecycle,voice.amplitude);
    modulation[channel].firstStage = voice.lifecycle.stages[0];
    if (entry == VoiceUpdateEntry::stopped) return VoiceControlResult::stopped;
    VoiceModulationUpdate task;
    using ModResult = VoiceModulationUpdate::Result;
    auto mod = task.begin(channel,modulation,sources);
    if (mod == ModResult::ready) mod = task.resume(modulation,inputs.ticks,*data.modulationRates(),waves,read,write);
    if (mod != ModResult::updated && mod != ModResult::shared) return VoiceControlResult::invalidInput;

    auto level = inputs.level; auto second = inputs.second; auto pitch = inputs.pitch;
    ApplyVoiceModulationOutputs(first,modulation[channel].block,level,second,pitch);
    if (!voice.amplitude.tick(inputs.ticks,inputs.amplitude,*data.times())) return VoiceControlResult::invalidInput;
    PrepareVoiceAmplitude(voice.lifecycle,voice.amplitude);
    modulation[channel].firstStage = voice.lifecycle.stages[0];
    // 3472 discards the envelope-call return and goes directly to3393;
    // unlike the entry-stage gate, this must notify the allocator.
    if (voice.lifecycle.stages[0] >= 14) return VoiceControlResult::finished;

    auto timing = inputs.secondTiming;
    timing.attackControlEnabled = (modulation[channel].fieldA2&16) != 0;
    const auto result = AdvanceSecondEnvelope(voice.release.second,voice.second,inputs.secondBypass,
        inputs.ticks,timing,*data.times(),second,inputs.secondBase,inputs.secondController,inputs.secondLimit,*data.secondEnvelope());
    if (result == SecondEnvelopeReleaseState::Result::invalidInput) return VoiceControlResult::invalidInput;
    voice.lifecycle.stages[1] = voice.release.second.stage;
    if (voice.pitch.advance(inputs.ticks,false,pitch,inputs.glideRate,*data.glideRates(),inputs.pitchReference,
        inputs.correctionSource,conversion) == VoicePitchRunner::Result::invalidInput) return VoiceControlResult::invalidInput;
    PrepareVoicePitch(voice.lifecycle,voice.pitch);
    if (voice.output.advance(voice.lifecycle.stages[0],level,inputs.spatial,*data.pan()) != VoiceOutputState::Result::updated)
        return VoiceControlResult::invalidInput;
    return VoiceControlResult::updated;
}
}
