#pragma once
#include "sc55_voice_set.h"
#include "sc55_note_setup.h"
#include "sc55_note_start.h"
#include "sc55_voice_control.h"
#include "sc55_sample_install.h"

namespace sc55
{
struct NormalVoicePreparationInputs
{
    InstalledVoice installed;
    PartialSamplePlan sample;
    uint8_t amplitude, secondary, sourceKey;
    uint16_t fractionalTune;
    bool unoffsetStart;
    uint8_t historyNibble;
    VoiceControlInputs controls;
    FirstModulationInputs firstControls;
};

struct PreparedNormalVoice
{
    VoiceControlState voice;
    VoiceControlInputs controls;
    FirstModulationInputs firstControls;
    VoicePostEnable post;
    PreparedPartPitch partPitch;
};

struct NormalVoicePreparationEntry
{
    unsigned slot;
    NormalVoicePreparationInputs request;
    VoiceStopState lifecycle;
    PreparedPartPitch previousPitch;
    const VoiceControlState* continuing = nullptr; // Live owner, borrowed only during preparation.
};

struct PreparedNormalVoiceBatch
{
    std::array<std::optional<PreparedNormalVoice>,2> voices;
    unsigned count = 0;
};

struct NormalPartialDspInputs
{
    uint8_t sourceKey;
    bool unoffsetStart;
    uint8_t historyNibble;
    VoiceControlInputs controls;
    FirstModulationInputs firstControls;
    PreparedPartPitch previousPitch;
};

struct DispatchedNormalVoiceInputs
{
    std::array<NormalVoicePreparationEntry,2> entries{};
    unsigned count = 0;
};

// Match this installed note to the next firmware-ordered preparation task,
// then assemble DSP requests in that task's order (not partial array order).
// One/two distinct normal owners; no same-slot partial overwrite semantics.
// Absent/special samples are not DSP owners. Previous pitch/control policy is
// explicit per partial, never reset implicitly. No PCM access or allocation.
// On mismatch/other pending work, leave task flags and activity untouched;
// the scheduler must service that work rather than dropping it or this note.
inline std::optional<DispatchedNormalVoiceInputs> DispatchNormalVoiceInputs(
    const MelodicNoteVelocity& selection,const InstalledPartialSamples& samples,
    const std::array<NormalPartialDspInputs,2>& inputs,
    std::array<VoiceStopState,voiceCapacity>& lifecycle,const VoiceLinks& links,
    std::array<uint8_t,voiceCapacity>& activity) noexcept
{
    std::array<std::optional<NormalVoicePreparationEntry>,2> byPartial{};
    unsigned count = 0; VoiceSet slots = 0;
    for (unsigned partial = 0; partial < 2; ++partial)
    {
        if (!samples[partial] || !samples[partial]->installed) continue;
        const auto& sample = *samples[partial]; const auto& installed = *sample.installed;
        const auto& input = inputs[partial];
        if (sample.slot >= voiceCapacity || (sample.sample.sampleId&0x8000)
            || installed.input.tone != selection.tone || installed.input.partial != partial
            || installed.input.sample != sample.sample.sampleId
            || !selection.partials.partials[partial]
            || (input.sourceKey >= 128 && input.sourceKey != 255)) return std::nullopt;
        // Both partial installations have already run in firmware order.
        // When partial1 falls back onto partial0's slot, task2 sees only the
        // final metadata. Do not dispatch two DSP owners for one PCM voice.
        if(slots&(VoiceSet::single(sample.slot))) {
            for(auto& previous:byPartial)
                if(previous && previous->slot==sample.slot) { previous.reset(); --count; }
        }
        slots |= VoiceSet::single(sample.slot); ++count;
        const auto& velocity = *selection.partials.partials[partial];
        byPartial[partial] = NormalVoicePreparationEntry{sample.slot,
            {installed,sample.sample,velocity.amplitude,velocity.secondary,input.sourceKey,
             sample.sample.pitch.fraction,input.unoffsetStart,input.historyNibble,input.controls,input.firstControls},
            lifecycle[sample.slot],input.previousPitch};
    }
    if (count == 0) return std::nullopt;
    auto pending = lifecycle; auto nextActivity = activity;
    const auto task = DispatchNextVoiceTask(pending,links,nextActivity);
    if (!task || task->kind != VoiceTaskDispatch::Kind::prepare || task->count != count) return std::nullopt;
    DispatchedNormalVoiceInputs result; result.count = count;
    VoiceSet selected = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto slot = task->slots[i];
        if (slot >= voiceCapacity || !(slots&(VoiceSet::single(slot))) || (selected&(VoiceSet::single(slot)))) return std::nullopt;
        selected |= VoiceSet::single(slot);
        for (const auto& entry : byPartial)
            if (entry && entry->slot == slot)
            { result.entries[i] = *entry; result.entries[i].lifecycle = pending[slot]; }
    }
    lifecycle = pending; activity = nextActivity;
    return result;
}

// 5542..559e /55de..560e normal preparation in dispatcher order. Non-restarted
// entries require the live owner and preserve its EG/LFO state through2c18.
// Caller owns one sound-data generation, modulation peers and prior pitch.
// No allocation, H8 execution, key-on, PCM time advance or MIDI defaults here.
// A failure after I/O may leave modulation/PCM state changed; do not retry it.
class NormalVoiceDspPreparation
{
public:
    enum class Progress { advanced, complete, failed };
    static std::optional<NormalVoiceDspPreparation> begin(
        std::span<const NormalVoicePreparationEntry> entries,const PartControllerState& controllers,
        std::array<FirstModulationVoice,voiceCapacity>& first,std::array<VoiceModulation,voiceCapacity>& second,
        std::array<uint8_t,voiceCapacity>& secondSources,const SoundData& data)
    {
        if (entries.empty() || entries.size() > 2
            || (entries.size() == 2 && entries[0].slot == entries[1].slot)
            || !data.samples() || !data.levels() || !data.keyLevels() || !data.keys()
            || !data.times() || !data.pan() || !data.modulationPreparation() || !data.modulationRates()
            || !data.secondPreparation() || !data.secondEnvelope() || !data.pitchEnvelope()
            || !data.pitchTiming() || !data.glideRates()) return std::nullopt;
        NormalVoiceDspPreparation operation;
        auto& result=operation.result_;
        result.count = unsigned(entries.size());
        // Validate both records before shared-state mutation or device I/O.
        for (unsigned i = 0; i < result.count; ++i)
        {
            const auto& entry = entries[i]; const auto& request = entry.request;
            const auto& installed = request.installed; const auto& input = installed.input;
            if (entry.slot >= voiceCapacity || input.partial >= 2 || input.part >= 16 || input.originalKey >= 128
                || input.sample != request.sample.sampleId || (!(installed.flags&128) && !entry.continuing)
                || entry.lifecycle.pendingOperation != VoiceOperation::none
                || request.firstControls.rateControl > 127 || request.firstControls.depthControl > 127
                || request.firstControls.delayControl > 127) return std::nullopt;
            if (!(installed.flags&128) && (entry.continuing->pitch.envelope.stage>22
                || (entry.continuing->pitch.envelope.stage&1))) return std::nullopt;
            const auto* patch = data.patch(input.tone);
            if (!patch) return std::nullopt;
            const bool restarting=(installed.flags&128)!=0;
            // 2bc3 tests the installed preparation flag, not a second caller
            // policy. Fresh notes use the descriptor base; reuse adds offset04.
            const auto samplePcm = PreparePartialSamplePcm(request.sample,*data.samples(),entry.slot,
                restarting,request.historyNibble);
            std::optional<EnvelopeRunner> amplitude;
            if (restarting) amplitude=PreparePartialAmplitude(*patch,input.partial,request.sample,*data.samples(),
                input.originalKey,request.amplitude,EnvelopeStage::attack1,request.controls.amplitude.attack,
                *data.levels(),*data.keyLevels(),*data.keys(),*data.times());
            if (!samplePcm || (restarting && !amplitude)) return std::nullopt;
            result.voices[i].emplace(PreparedNormalVoice{
                restarting ? VoiceControlState{*amplitude} : *entry.continuing,
                request.controls,request.firstControls,{},{}});
            auto& prepared = *result.voices[i];
            prepared.voice.lifecycle = entry.lifecycle;
            prepared.voice.lifecycle.flagMinus3B = installed.flags;
            prepared.voice.prepared.sample = samplePcm->address;
            prepared.voice.stopAtSampleEnd = samplePcm->loopFlag != 0;
            prepared.controls.spatial.basePan = patch->partial[input.partial].raw[9];
            operation.entries_[i]=entry;
            // Continuing DSP state was copied into the prepared owner above.
            // Do not retain the caller's live-owner pointer across a yield.
            operation.entries_[i].continuing=nullptr;
        }
        const auto& firstRecord = entries[0].request.installed.input;
        const auto controllerInput = controllers.inputs(firstRecord.part,firstRecord.originalKey);
        if (!controllerInput) return std::nullopt;
        const auto controllerState = PrepareVoiceControllers(*controllerInput);
        const auto& modulation = *data.modulationPreparation();
        auto& secondSetup=operation.secondSetup_;
        // 5639 metadata,5c20/5ff5 controllers,37fc depths for both.
        for (unsigned i = 0; i < result.count; ++i)
        {
            const auto slot = entries[i].slot;
            const auto& input = entries[i].request.installed.input;
            const auto& patch = *data.patch(input.tone); const auto& partial = patch.partial[input.partial];
            auto& prepared = *result.voices[i];
            const auto previousFirstRate = first[slot].block.rateIndex;
            const bool restarting=(entries[i].request.installed.flags&128)!=0;
            if (restarting) { first[slot] = {}; second[slot] = {}; secondSources[slot] = voiceCapacity; }
            // 3d1a preserves the destination's rate index; only3891 sets it.
            first[slot].block.rateIndex = previousFirstRate;
            first[slot].commonIdentity = input.tone;
            first[slot].field9b = second[slot].field9b = input.part;
            second[slot].partialIdentity = input.tone; second[slot].field99 = input.partial;
            // Pending peers must not be mistaken for already running voices.
            first[slot].firstStage = second[slot].firstStage = entries[i].lifecycle.stages[0];
            secondSetup[i].input = prepared.controls.second;
            secondSetup[i].timing = prepared.controls.secondTiming;
            secondSetup[i].controller = prepared.controls.secondController;
            secondSetup[i].input.suppressPositiveControl = (partial.raw[8]&4) != 0;
            secondSetup[i].timing.attackControlEnabled = (partial.raw[8]&16) != 0;
            controllerState.apply(prepared.controls.level,secondSetup[i].input,prepared.controls.pitch,
                first[slot].block,second[slot].block);
            auto& inputFirst = prepared.firstControls;
            if (entries[0].request.installed.flags&128)
                inputFirst.pitchDepth = PrepareModulationDepths(partial,first[slot].block,second[slot].block,modulation.depths);
            inputFirst.mode = patch.common[2]; inputFirst.baseRate = patch.common[3];
            inputFirst.delay = patch.common[4]; inputFirst.attack = patch.common[5];
        }
        return operation;
    }

    template<class Read,class Write>
    Progress resume(std::array<FirstModulationVoice,voiceCapacity>& first,std::array<VoiceModulation,voiceCapacity>& second,
        std::array<uint8_t,voiceCapacity>& secondSources,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        if(failed_) return Progress::failed;
        if(phase_>result_.count) return Progress::complete;
        if(phase_==0) {
            if(!initializeFirst(first,data,waves,read,write)) { failed_=true; return Progress::failed; }
        } else if(!prepareVoice(phase_-1,first,second,secondSources,data,conversion,waves,read,write)) {
            failed_=true; return Progress::failed;
        }
        ++phase_;
        return phase_>result_.count ? Progress::complete : Progress::advanced;
    }
    const PreparedNormalVoiceBatch* result() const noexcept
    { return !failed_ && phase_>result_.count ? &result_ : nullptr; }

private:
    NormalVoiceDspPreparation()=default;
    template<class Read,class Write>
    bool initializeFirst(std::array<FirstModulationVoice,voiceCapacity>& first,const SoundData& data,
        const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        const auto& entries=entries_; auto& result=result_;
        const auto& modulation=*data.modulationPreparation();
        const auto firstSlot = entries[0].slot;
        if ((entries[0].request.installed.flags&128)
            && InitializeFirstVoiceModulation(firstSlot,first,result.voices[0]->firstControls,modulation.timing,
            modulation.depths.pitch,*data.modulationRates(),waves,read,write) == ModulationRoute::invalidInput)
            return false;
        if (result.count == 2 && (entries[0].request.installed.flags&128))
        {
            const auto secondSlot = entries[1].slot;
            const auto& input = result.voices[1]->firstControls;
            // 558d calls the full3d1a initializer, not the periodic3d44 tail.
            if (!InitializeSharedFirstModulation(first[secondSlot].block,first[secondSlot].sharing,
                first[firstSlot].block,first[firstSlot].sharing,input.pitchDepth,input.depthControl,modulation.depths.pitch))
                return false;
        }
        return true;
    }
    template<class Read,class Write>
    bool prepareVoice(unsigned i,std::array<FirstModulationVoice,voiceCapacity>& first,
        std::array<VoiceModulation,voiceCapacity>& second,std::array<uint8_t,voiceCapacity>& secondSources,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        const auto& entries=entries_; auto& result=result_; auto& secondSetup=secondSetup_;
        const auto& modulation=*data.modulationPreparation();
        // 56b5 ->2a66 and following DSP, first member completely before second.
        {
            const auto& entry = entries[i]; const auto slot = entry.slot;
            const auto& request = entry.request; const auto& installed = request.installed;
            const auto& input = installed.input; const auto& patch = *data.patch(input.tone);
            const auto& partial = patch.partial[input.partial];
            auto& prepared = *result.voices[i]; auto& voice = prepared.voice;
            auto& controls = prepared.controls; auto& setup = secondSetup[i];
            if (!(installed.flags&128))
            {
                // 2a66 ->2bac ->2c18 ->4858: keep amplitude/TVF/TVA and LFO
                // progress. Only sample metadata, frozen PCM levels and pitch are
                // prepared. Activation later restores the saved first-EG stage.
                if (!PrepareReusedVoicePcm(slot,voice.lifecycle,voice.amplitude.state().level,
                    voice.second,voice.prepared,prepared.post,write)) return false;
                auto amplitudeState=voice.amplitude.state();
                amplitudeState.pcmWord=voice.lifecycle.cached18;
                voice.amplitude=EnvelopeRunner(voice.amplitude.setup(),amplitudeState);
                voice.output.tva.command=voice.lifecycle.cached16;
                ApplyVoiceModulationOutputs(first[slot].block,second[slot].block,controls.level,setup.input,controls.pitch);
                const NormalPitchStartInputs pitchInput{input.adjustedKey,input.originalKey,input.velocity,
                    request.fractionalTune,installed.flags,request.sourceKey,controls.glideRate,controls.correctionSource,controls.pitch};
                const auto pitch=PrepareNormalVoicePitch(patch,input.partial,request.sample,pitchInput,
                    entry.previousPitch,data,conversion,read,write,&voice.pitch,controls.ticks);
                if (!pitch) return false;
                voice.pitch=pitch->runner; prepared.partPitch=pitch->part;
                PrepareVoicePitch(voice.lifecycle,voice.pitch);
                controls.pitchReference=prepared.partPitch.values.reference;
                voice.alternatePitchReference=prepared.partPitch.values.alternateReference;
                return true;
            }
            if (PrepareVoiceSecondModulation(slot,voice.lifecycle,second,secondSources,partial,
                modulation.timing,*data.modulationRates(),waves,read,write) == SecondModulationPreparation::invalidInput)
                return false;
            PrepareVoiceAmplitude(voice.lifecycle,voice.amplitude);
            if (!voice.output.spatial.initialize(controls.spatial,*data.pan(),read,write)) return false;
            voice.prepared.pcm12 = voice.output.spatial.panWord; voice.prepared.pcm14 = voice.output.spatial.effects;
            // Do not overwrite a shared initializer's copied source rate.
            ApplyVoiceModulationOutputs(first[slot].block,second[slot].block,controls.level,setup.input,controls.pitch);
            voice.output.tva.initialize(controls.level); PrepareVoiceTva(voice.lifecycle,voice.output.tva);
            const auto& envelope = *data.secondPreparation();
            if (!setup.prepare(partial,input.originalKey,request.secondary,voice.release.second,voice.second,
                envelope.keys,envelope.targets,data.pitchEnvelope()->curve,envelope.timing,data.keys()->multipliers,
                *data.times(),*data.secondEnvelope())) return false;
            PrepareVoiceSecondEnvelope(voice.lifecycle,voice.prepared,prepared.post,voice.release.second,setup,voice.second);
            const NormalPitchStartInputs pitchInput{input.adjustedKey,input.originalKey,input.velocity,
                request.fractionalTune,installed.flags,request.sourceKey,controls.glideRate,controls.correctionSource,controls.pitch};
            const auto pitch = PrepareNormalVoicePitch(patch,input.partial,request.sample,pitchInput,
                entry.previousPitch,data,conversion,read,write);
            if (!pitch) return false;
            voice.pitch = pitch->runner; prepared.partPitch = pitch->part;
            PrepareVoicePitch(voice.lifecycle,voice.pitch);
            controls.pitchReference = prepared.partPitch.values.reference;
            voice.alternatePitchReference=prepared.partPitch.values.alternateReference;
            controls.secondBypass = setup.mode.bypass; controls.secondTiming = setup.timing;
            controls.second = setup.input; controls.secondBase = setup.controlBase;
            controls.secondController = setup.controller; controls.secondLimit = setup.limit;
            first[slot].firstStage = second[slot].firstStage = voice.lifecycle.stages[0];
        }
        return true;
    }
    std::array<NormalVoicePreparationEntry,2> entries_{};
    std::array<SecondEnvelopeSetup,2> secondSetup_{};
    PreparedNormalVoiceBatch result_{};
    unsigned phase_=0;
    bool failed_=false;
};

template<class Read,class Write>
std::optional<PreparedNormalVoiceBatch> PrepareNormalVoicesDsp(
    std::span<const NormalVoicePreparationEntry> entries,const PartControllerState& controllers,
    std::array<FirstModulationVoice,voiceCapacity>& first,std::array<VoiceModulation,voiceCapacity>& second,
    std::array<uint8_t,voiceCapacity>& secondSources,const SoundData& data,
    const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
{
    auto operation=NormalVoiceDspPreparation::begin(entries,controllers,first,second,secondSources,data);
    if(!operation) return std::nullopt;
    for(unsigned phase=0;phase<3;++phase) {
        const auto progress=operation->resume(first,second,secondSources,data,conversion,waves,read,write);
        if(progress==NormalVoiceDspPreparation::Progress::failed) return std::nullopt;
        if(const auto* result=operation->result()) return *result;
    }
    return std::nullopt;
}

template<class Read,class Write>
std::optional<PreparedNormalVoice> PrepareNormalVoiceDsp(unsigned slot,
    const NormalVoicePreparationInputs& request,const VoiceStopState& lifecycle,
    const PreparedPartPitch& previousPitch,const PartControllerState& controllers,
    std::array<FirstModulationVoice,voiceCapacity>& first,std::array<VoiceModulation,voiceCapacity>& second,
    std::array<uint8_t,voiceCapacity>& secondSources,const SoundData& data,
    const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
{
    const NormalVoicePreparationEntry entry{slot,request,lifecycle,previousPitch};
    const auto result = PrepareNormalVoicesDsp(std::span(&entry,1),controllers,first,second,secondSources,
        data,conversion,waves,read,write);
    return result ? result->voices[0] : std::nullopt;
}
}
