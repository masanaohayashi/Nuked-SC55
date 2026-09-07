#pragma once
#include "sc55_envelope_setup.h"

namespace sc55
{
// First (amplitude) envelope only. Owned by the audio/control thread. The
// caller prepares targets/key scales and supplies the control-rate clock;
// neither a firmware RAM view nor a CPU is retained. PCM level synchronization
// before release remains a caller responsibility (v1.21 31c7).
class EnvelopeRunner
{
public:
    struct Setup
    {
        PartialEnvelopePlan plan;
        std::array<uint8_t,4> targets;
        uint16_t keyScale, releaseKeyScale, delayIncrement;
    };
    struct State
    {
        EnvelopeStageState segment;
        uint16_t level, pcmWord, delayAccumulator;
    };
    struct Controls { uint8_t attack = 64, decay = 64, release = 64; };

    // Inputs at the remaining native-allocation boundary, not a RAM snapshot.
    struct NoteInputs
    {
        uint8_t keyAdjustedBase, amplitude, recordLevel, patchLevel;
        uint16_t keyScale, releaseKeyScale;
        EnvelopeStage allocatedStage;
        uint8_t attackControl;
    };
    struct KeyNoteInputs
    {
        uint8_t key, amplitude, recordLevel, patchLevel;
        EnvelopeStage allocatedStage;
        uint8_t attackControl;
    };
    static std::optional<EnvelopeRunner> fromKey(const SC55Partial& partial,KeyNoteInputs note,
        const EnvelopeLevelTables& levels,const EnvelopeKeyLevelTables& keyLevels,
        const EnvelopeKeyTables& keys,const EnvelopeTimes& times) noexcept
    {
        const auto base = PrepareEnvelopeKeyLevel(partial,note.key,levels,keyLevels);
        const auto scales = PrepareEnvelopeKeyScales(partial,note.key,keys);
        if (!base || !scales) return std::nullopt;
        return fromPartial(partial,{*base,note.amplitude,note.recordLevel,note.patchLevel,
            (*scales)[0],(*scales)[1],note.allocatedStage,note.attackControl},levels,times);
    }
    static std::optional<EnvelopeRunner> fromPartial(const SC55Partial& partial,
        NoteInputs note,const EnvelopeLevelTables& levels,const EnvelopeTimes& times) noexcept
    {
        const auto base = PrepareEnvelopeBase(note.keyAdjustedBase,note.amplitude,
            note.recordLevel,note.patchLevel,levels);
        const auto plan = PreparePartialEnvelope(partial,note.amplitude);
        if (!base || !plan) return std::nullopt;
        const auto targets = PrepareEnvelopeTargets(partial,*base,levels);
        if (!targets) return std::nullopt;
        return start({*plan,*targets,note.keyScale,note.releaseKeyScale,0},
            note.allocatedStage,partial.raw[0],note.attackControl,times);
    }

    // v1.21 2f5a..2fcc. Allocation's stage is supplied separately: this
    // routine does not write the stage field, and primes attack even in delay.
    static std::optional<EnvelopeRunner> start(Setup setup,EnvelopeStage allocatedStage,
        uint8_t delayParameter,uint8_t attackControl,const EnvelopeTimes& times) noexcept
    {
        const auto duration = PrepareEnvelopeDuration(setup.plan.stages[0].value,attackControl,
            setup.keyScale,setup.plan.velocityScale1,times);
        if (!duration) return std::nullopt;
        uint16_t accumulator = 0;
        setup.delayIncrement = 0;
        const auto delay = delayParameter < 128 ? uint16_t(uint16_t(times[delayParameter]+4) >> 3) : uint16_t(0);
        if (delayParameter < 128 && delay == 0)
            accumulator = 65535;
        else
        {
            if (delay != 0) setup.delayIncrement = uint16_t(65535u/delay);
            setup.targets[3] = 0;
        }
        const auto step = StepEnvelopeSegment(*duration,1,{0,0},0,setup.targets[0],
            setup.plan.stages[0].flag != 0,0);
        const uint16_t pcmWord = uint8_t(step->pcmWord) == 0xaf
            ? uint16_t((step->pcmWord & 0xff00)|0xba) : step->pcmWord;
        return EnvelopeRunner(setup,{{allocatedStage,step->progress,setup.plan.stages[0],0,setup.targets[0]},
            step->level,pcmWord,accumulator});
    }

    const Setup& setup() const noexcept { return setup_; }

    // v1.21 57ca..57f3, NEW voice only (partial flag bit 7 set).
    // A nonzero initialized delay accumulator enables the primed attack;
    // otherwise enter delay, discard its primed position and send zero/b6.
    // Other envelopes and the pitch PCM word belong to the voice owner.
    void activateNewVoice() noexcept
    {
        if (state_.delayAccumulator != 0) state_.segment.stage = EnvelopeStage::attack1;
        else
        {
            state_.segment.stage = EnvelopeStage::delay;
            state_.segment.progress.position = 0;
            state_.pcmWord = 0x00b6;
        }
    }

    // Explicit initial state until note-allocation initialization is native.
    EnvelopeRunner(Setup setup,State initial) noexcept : setup_(setup), state_(initial) {}
    const State& state() const noexcept { return state_; }

    // Current amplitude returned by the PCM engine, already converted from
    // its half-scale register representation. Does not replace segment state.
    void synchronizePcmLevel(uint16_t level) noexcept { state_.level = level; }

    void release(uint16_t synchronizedPcmLevel) noexcept
    {
        state_.segment = ReleaseEnvelope(state_.segment,synchronizedPcmLevel,
            setup_.plan.stages[4],setup_.delayIncrement);
        state_.level = synchronizedPcmLevel;
    }

    // One firmware control tick, not one audio block. A completed segment
    // advances on the NEXT tick; do not drain all stages in a while loop.
    // Invalid controls leave all persistent state unchanged.
    bool tick(uint16_t elapsedTicks,Controls controls,const EnvelopeTimes& times) noexcept
    {
        if (controls.attack > 127 || controls.decay > 127 || controls.release > 127) return false;
        auto& segment = state_.segment;
        if (segment.stage == EnvelopeStage::finished) return true;
        segment = AdvanceEnvelopeStage(segment,setup_.plan.stages,setup_.targets);
        switch (segment.stage)
        {
        case EnvelopeStage::finished: return true;
        case EnvelopeStage::delay:
        {
            const uint32_t sum = uint32_t(state_.delayAccumulator)+setup_.delayIncrement;
            state_.delayAccumulator = uint16_t(sum);
            if (sum > 65535) segment.stage = EnvelopeStage::attack1;
            return true;
        }
        case EnvelopeStage::sustain:
            segment.progress.deferredTicks = 0;
            state_.pcmWord = 0xff00;
            state_.level = uint16_t(segment.target << 8);
            if (state_.level == 0) segment.stage = EnvelopeStage::finished;
            return true;
        case EnvelopeStage::release:
            if (state_.level == 0) { segment.stage = EnvelopeStage::finished; return true; }
            break;
        default: break;
        }
        const bool attack = segment.stage == EnvelopeStage::attack1 || segment.stage == EnvelopeStage::attack2;
        const bool releasing = segment.stage == EnvelopeStage::release;
        const auto duration = PrepareEnvelopeDuration(segment.parameter.value,
            attack ? controls.attack : releasing ? controls.release : controls.decay,
            releasing ? setup_.releaseKeyScale : setup_.keyScale,
            attack ? setup_.plan.velocityScale1 : setup_.plan.velocityScale2,times);
        if (!duration) return false; // Setup parameters must be expanded 7-bit values.
        const auto step = StepEnvelopeSegment(*duration,elapsedTicks,segment.progress,
            segment.start,segment.target,segment.parameter.flag != 0,state_.level);
        segment.progress = step->progress;
        state_.level = step->level;
        state_.pcmWord = step->pcmWord;
        return true;
    }
private:
    Setup setup_;
    State state_;
};
}
