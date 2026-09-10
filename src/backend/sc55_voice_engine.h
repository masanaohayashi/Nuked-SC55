#pragma once
#include "sc55_voice_set.h"
#include "sc55_voice_runtime.h"
#include "sc55_note_fanout.h"
#include "sc55_rhythm_admission.h"
#include "sc55_voice_commands.h"
#include "sc55_part_settings.h"
#include "sc55_rhythm_settings.h"
#include "sc55_sysex.h"

namespace sc55
{
// Serialized native voice-engine state. Not the host adapter or GS router.
// Public state supports explicit boot/import policy; once processing starts,
// only the serialized owner may mutate it. Sound-data/PCM remain caller-owned.
// Owns voice commands and common sound-control events. MIDI routing/defaults
// and the outer command-versus-control scheduling priority stay with the receiver.
class NativeVoiceEngine
{
public:
    VoiceControlRuntime runtime;
    PartNoteState notes;
    VoiceInstallationState installation;
    std::array<VoiceStopState,voiceCapacity> lifecycle{};
    VoiceKeyMask mask;
    ControlTaskClock clock;
    NoteOnFanout noteOn;
    struct MonoPart
    {
        MonoHeldKeys held;
        uint8_t current=60,velocity=0,glideRate=0,source=255;
        bool portamento=false;
        std::optional<uint16_t> tone;
    };
    std::array<MonoPart,16> mono{};
    struct PreparationHistory
    {
        std::array<std::array<uint8_t,2>,16> partKeys{};
        uint8_t reference=0;
        std::array<PreparedPartPitch,voiceCapacity> previousPitch{};
        std::array<uint8_t,voiceCapacity> drumMap,drumKey{};
        PreparationHistory() noexcept { drumMap.fill(255); }
    };
    PreparationHistory preparation;
    // An admitted note retains its receive-time identity across capacity and
    // device waits. A replacement/held-key return starts with fresh progress.
    struct PendingAdmission
    {
        enum class Origin { midi, heldKeyReturn };
        enum class SourceReuse { search, fresh };
        NoteRequest request;
        Origin origin=Origin::midi;
        SourceReuse sourceReuse=SourceReuse::search;
        bool repeatedRetired=false;
        bool isHeldReturn() const noexcept { return origin==Origin::heldKeyReturn; }
        MidiDecoder::Event event() const noexcept
        { return {MidiDecoder::Kind::message,0x90,request.key,request.velocity,2}; }
    };
    VoiceCommands commands;
    bool admissionPending() const noexcept { return admission.has_value(); }
#if defined(SC55_CONTROL_TIMING_ORACLE)
    // Fixture access for instruction-level diagnostics, absent in the product.
    auto& admissionAudit() noexcept { return admission; }
#endif

    // Borrowed audio-owned settings for one synchronous operation. Never
    // retained across a PCM wait: receive-time tone lives in PendingAdmission,
    // while preparation reads current part/controller/shared-map settings.
    struct Configuration
    {
        const PartSettings& parts;
        const MasterControls& master;
        const VoiceCapacityPolicy& capacity;
        const RhythmSettings& rhythm;
        const RhythmPresetTable* rhythmPresets;
        bool effectsAvailable;
    };
    struct AdmissionResult { bool failed=false,unsupported=false; };
    struct CommandResult : AdmissionResult
    {
        // The controller owns capacity configuration. Report the consumed
        // program, not the receiver's possibly newer selected tone.
        std::optional<ProgramVoiceRequest> program;
    };

    // One serialized voice-management command, followed by its admission.
    // The scheduler chooses when to run us; it never takes a command itself
    // or changes the continuation after a capacity/PCM wait.
    template<class Read,class Write>
    CommandResult serviceCommand(const Configuration& config,std::span<const std::optional<uint16_t>,16> tones,
        const PartControllerState& controllers,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        CommandResult result;
        if(!serviceVoiceCompletion()) { result.failed=true; return result; }
        if(runtime.startupPending() || operationsPending()) return result;
        if(!admission) {
            if(const auto command=commands.take()) {
                const auto part=std::visit([](const auto& request) { return request.part; },*command);
                if(part>=16) { result.failed=true; return result; }
                if(const auto* note=std::get_if<NoteRequest>(&*command)) {
                    if(note->action==NoteRequest::Action::on) admission=PendingAdmission{*note};
                    else result.failed=!releaseNote(*note,config.parts.routing[part].noteFlags,tones[part]);
                } else if(const auto* pedal=std::get_if<PedalRequest>(&*command)) {
                    result.failed=!applyPedal(*pedal);
                    if(!result.failed && pedal->kind==PedalRequest::Kind::portamento) refreshControls(config);
                } else if(const auto* source=std::get_if<PortamentoSourceRequest>(&*command))
                    mono[part].source=source->key;
                else if(const auto* release=std::get_if<PartReleaseRequest>(&*command)) {
                    if(release->kind==PartReleaseRequest::Kind::notes)
                        result.failed=!releasePart(part,(config.parts.routing[part].noteFlags&0x10)!=0,false,true);
                    else result.failed=stopSoundingParts(uint16_t(1u<<part),read,write)==StopRequest::failed;
                } else if(std::holds_alternative<ControllerResetRequest>(*command))
                    result.failed=!resetVoiceControllers(part,(config.parts.routing[part].noteFlags&0x10)!=0,false);
                else if(const auto* program=std::get_if<ProgramVoiceRequest>(&*command)) {
                    programChanged(part,program->tone,config,data);
                    result.program=*program;
                } else if(const auto* mode=std::get_if<PartModeRequest>(&*command))
                    result.failed=!changePartMode(part,mode->poly,read,write);
            }
        }
        if(!result.failed) prepareAdmission(config,controllers,data,conversion,waves,read,write,result);
        return result;
    }

    void setPortamentoTime(unsigned part,uint8_t value) noexcept
    { if(part<16) mono[part].glideRate=value; }

    template<class Read,class Write>
    AdmissionResult serviceAdmission(const Configuration& config,const PartControllerState& controllers,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write)
    {
        AdmissionResult result;
        if(failed()) { result.failed=true; return result; }
        prepareAdmission(config,controllers,data,conversion,waves,read,write,result);
        return result;
    }

    bool reuseInvalidated(unsigned part) const noexcept
    { return part<16 && (reuseInvalidation_&(1u<<part)); }
    void programChanged(unsigned part,uint16_t tone,const Configuration& config,const SoundData& data) noexcept
    {
        if(part>=16) return;
        reuseInvalidation_|=uint16_t(1u<<part);
        seedPitchHistory(part,tone,config,data);
    }

    void seedPitchHistory(unsigned part,std::optional<uint16_t> tone,
        const Configuration& config,const SoundData& data) noexcept
    {
        if(part>=16 || !tone) return;
        const auto* patch=data.patch(*tone);
        if(!patch) return;
        const auto& settings=config.parts.parts[part];
        const auto key=TransposeMasterKey(TransposePartKey(60,settings.keyShift,
            uint8_t(settings.controls.coarseTuning)),config.master.keyShift);
        for(unsigned p=0;p<2;++p) if(patch->partial[p].used)
            preparation.partKeys[part][p]=TransposePartialKey(key,patch->partial[p].raw[10]);
    }
    void refreshControls(const Configuration& config) noexcept
    {
        for (unsigned slot = 0; slot < voiceCapacity; ++slot)
            if (runtime.voices[slot]) {
                applyPart(config,installation.voices[slot].input.part,runtime.inputs[slot],
                    preparation.drumMap[slot],preparation.drumKey[slot]);
                applyToneModulation(config,installation.voices[slot].input.part,runtime.firstInputs[slot]);
            }
    }
    // Retirement belongs to the admission, not to each attempt to allocate.
    // The original receive key matters for high-note mapped tones as well.
    template<class Read,class Write>
    bool retireAdmission(uint8_t selector,uint8_t noteFlags,Read&& read,Write&& write)
    {
        if(!admission || failed() || runtime.startupPending()) return false;
        if(admission->repeatedRetired) return true;
        const auto& note=admission->request;
        if(!retireRepeatedNote(note.part,note.key,selector,noteFlags,read,write)) return false;
        admission->repeatedRetired=true;
        return true;
    }

    // Determine fresh-note eligibility before retiring a repeated note. After
    // retirement, recompute against the actual allocator. The preview never
    // allocates a physical owner; sample validation still precedes the commit.
    template<class Read,class Write>
    std::optional<MelodicAllocationResult> previewMelodicAdmission(const MidiDecoder::Event& event,
        const ChannelControls::Channel& channel,const MelodicAllocationInputs& input,uint8_t noteFlags,
        const SoundData& data,Read&& read,Write&& write)
    {
        if(!admission || admission->request.part!=input.part || failed() || runtime.startupPending())
            return std::nullopt;
        auto probe=notes.allocator;
        auto selected=AllocateMelodicNote(event,channel,input,data,probe);
        using Status=MelodicAllocationResult::Status;
        if(!admission->repeatedRetired && (selected.status==Status::allocated || selected.status==Status::needsCapacity)) {
            if(!retireAdmission(input.groupFlags,noteFlags,read,write)) return std::nullopt;
            probe=notes.allocator;
            selected=AllocateMelodicNote(event,channel,input,data,probe);
        }
        return selected;
    }

    // One commit for melodic, mono/source and rhythm preparation. Rejected
    // or deferred admissions must not advance portamento or voice provenance.
    bool rememberPreparedNote(unsigned part,const VoiceControlRuntime::MelodicStartResult& result,
        uint8_t reference,uint8_t drumMap=255,uint8_t drumKey=0) noexcept
    {
        using Status=VoiceControlRuntime::MelodicStartResult::Status;
        if (part>=16 || (result.status!=Status::started && result.status!=Status::preparedOnly)
            || !result.requests || result.requests->count>2) return false;
        for(unsigned i=0;i<result.requests->count;++i)
            if(result.requests->entries[i].slot>=voiceCapacity || !result.prepared || !result.prepared->voices[i]) return false;
        preparation.reference=reference;
        result.pitchHistory.apply(preparation.partKeys[part],preparation.reference);
        for(unsigned i=0;i<result.requests->count;++i) {
            const auto slot=result.requests->entries[i].slot;
            preparation.previousPitch[slot]=result.prepared->voices[i]->partPitch;
            preparation.drumMap[slot]=drumMap;
            // Melodic reuse invalidates the map, but retains the old key byte.
            if(drumMap!=255) preparation.drumKey[slot]=drumKey;
        }
        return true;
    }

    // EG restart is a voice-owner decision, not a player interpretation of
    // cached firmware flags. Held-key return and explicit source reuse retain
    // their distinct restart policy.
    std::optional<uint8_t> monoReuseFlags(unsigned part,uint8_t group,bool heldReturn,bool sourceReuse) const noexcept
    {
        if (part>=16) return std::nullopt;
        const auto& state=mono[part];
        const auto source=heldReturn ? uint8_t(255) : state.source;
        if (group>=voiceCapacity || (!state.portamento && source>=128)) return uint8_t(0xff);
        const auto plan=notes.allocator.prepareGroupReuse(group,{0xff,{255,255}});
        if (!plan) return std::nullopt;
        auto flags=plan->flags;
        if (!heldReturn && !sourceReuse) {
            bool restart=!state.held.highest().has_value();
            for (const auto slot:plan->voices)
                if (slot<voiceCapacity && runtime.voices[slot])
                    restart|=(runtime.voices[slot]->lifecycle.fieldC8B3&128)!=0;
            flags=uint8_t((flags&0x7f)|(restart ? 0x80 : 0));
        }
        return flags;
    }

    std::optional<MonoHeldKeys::ReleaseDecision> releaseMonoNote(unsigned part,uint8_t key) noexcept
    {
        if (failed() || runtime.preparationPending() || part>=16) return std::nullopt;
        auto& state=mono[part];
        const auto decision=state.held.release(key,state.current);
        if (!decision) return std::nullopt;
        if (decision->action==MonoHeldKeys::ReleaseDecision::Action::releaseGroup
            && notes.allocator.partHead[part]<voiceCapacity && !notes.allocator.releaseMonoGroup(part)) return std::nullopt;
        if (!runtime.publishNoteReleases(notes.allocator)) return std::nullopt;
        return decision;
    }
    bool failed() const noexcept { return rhythmFailed_ || runtime.failed() || noteOn.status() == NoteOnFanout::Status::failed; }

    // Voice-management commands act on one serialized owner. The receiver
    // supplies routing/tone values, never edits held keys or publishes a
    // release snapshot itself. A mono return retains the selected live tone,
    // not the tone of the key being released.
    bool releaseNote(const NoteRequest& request,uint8_t noteFlags,
        std::optional<uint16_t> selectedTone) noexcept
    {
        if(failed() || runtime.preparationPending() || admission || request.part>=16
            || request.action!=NoteRequest::Action::off) return false;
        const auto part=request.part;
        const auto selected=SelectNoteRelease(request.key,noteFlags);
        if(!selected) return false;
        if(selected->path==NoteReleaseSelection::Path::group) {
            auto updated=notes;
            const MidiDecoder::Event event{MidiDecoder::Kind::message,0x80,request.key,0,2};
            if(!updated.noteOff(event,part,selected->selector)
                || !runtime.publishNoteReleases(updated.allocator)) return false;
            notes=updated;
            return true;
        }
        const auto decision=releaseMonoNote(part,request.key);
        if(!decision) return false;
        const auto& state=mono[part];
        if(decision->action==MonoHeldKeys::ReleaseDecision::Action::replaceKey && state.velocity)
            admission=PendingAdmission{{NoteRequest::Action::on,part,decision->replacement,
                state.velocity,selectedTone},PendingAdmission::Origin::heldKeyReturn};
        return true;
    }

    bool applyPedal(const PedalRequest& request) noexcept
    {
        if(failed() || runtime.preparationPending() || request.part>=16) return false;
        if(request.kind==PedalRequest::Kind::portamento) {
            mono[request.part].portamento=request.enabled;
            if(request.enabled) mono[request.part].source=255;
            return true;
        }
        auto updated=notes;
        const MidiDecoder::Event event{MidiDecoder::Kind::message,0xb0,
            uint8_t(request.kind==PedalRequest::Kind::hold ? 64 : 66),
            uint8_t(request.enabled ? 127 : 0),2};
        if(!updated.applyPedal(event,request.part,true)
            || !runtime.publishNoteReleases(updated.allocator)) return false;
        notes=updated;
        return true;
    }

    bool resetVoiceControllers(unsigned part,bool rhythm,bool allNotes) noexcept
    {
        if(!releasePart(part,rhythm,true,allNotes)) return false;
        mono[part].portamento=false;
        mono[part].source=255;
        return true;
    }

    template<class Read,class Write>
    bool changePartMode(unsigned part,bool poly,Read&& read,Write&& write)
    {
        if(!stopPartGroups(part,read,write)) return false;
        if(!poly) mono[part].held={};
        mono[part].current=60;
        mono[part].tone.reset();
        reuseInvalidation_&=uint16_t(~(1u<<part));
        return true;
    }

    // 07c7..0850: task1 selects command event0 before completion event1,
    // then drains the command ring before waiting for another event. Keep
    // the completed slot occupied across a queued/resumed admission.
    bool voiceCommandsPending() const noexcept
    { return admission.has_value() || commands.size()!=0 || noteOn.pending(); }

    bool serviceVoiceCompletion() noexcept
    { return !failed() && (voiceCommandsPending() || runtime.consumeVoiceCompletion(notes.allocator)); }

    bool beginNoteOn(const MidiDecoder::Event& event,std::span<const PartMidiReceive,16> routing,NoteReceiveMode mode)
    { return !failed() && !runtime.startupPending() && noteOn.begin(event,routing,mode); }

    template<class Visitor>
    NoteOnFanout::Status serviceNoteOn(Visitor&& visitor)
    {
        if (failed()) return noteOn.fail();
        if (runtime.startupPending()) return NoteOnFanout::Status::deferred;
        return noteOn.advance(visitor);
    }

    template<std::size_t Capacity,class Sink>
    MidiDispatchResult serviceMidi(MidiEventQueue<Capacity>& queue,uint64_t now,Sink&& sink)
    {
        if (failed()) return MidiDispatchResult::failed;
        // MIDI decoding is task0, not task1's completion event. In particular
        // it can enqueue a command before task1 chooses between event0/1.
        if (noteOn.pending()) return MidiDispatchResult::deferred;
        return runtime.serviceMidi(queue,now,sink);
    }

    struct ReleaseMidiResult
    {
        enum class Status { invalidInput, deferred, accepted, failed };
        Status status;
        uint16_t matchedParts = 0; // Note Off: parts with at least one matched group.
    };

    // Note Off (including velocity-zero Note On), hold and sostenuto only.
    // Receiver gates/fan-out/mono policy remain explicit. Commit note/pedal
    // bookkeeping and its complete DSP release snapshot together. A missing
    // DSP owner rejects the transaction before either side changes. No PCM I/O.
    // On failure the caller must retain/report the event, not treat it as an
    // accepted release; a MidiEventQueue sink can latch failure to avoid replay.
    ReleaseMidiResult receiveReleaseMidi(const MidiDecoder::Event& event,
        std::span<const PartMidiReceive,16> routing,NoteReceiveMode mode) noexcept
    {
        using Status = ReleaseMidiResult::Status;
        if (failed()) return {Status::failed};
        if (runtime.startupPending() || noteOn.pending()) return {Status::deferred};
        auto updated = notes;
        uint16_t matched = 0;
        const auto kind = event.status&0xf0;
        if (kind == 0x80 || (kind == 0x90 && event.second == 0))
        {
            const auto result = updated.receiveNoteOff(event,routing,mode);
            if (!result) return {Status::invalidInput};
            matched = *result;
        }
        else if (!updated.receivePedal(event,routing)) return {Status::invalidInput};
        // publishNoteReleases validates all owners before mutating any voice.
        if (!runtime.publishNoteReleases(updated.allocator)) return {Status::failed};
        notes = updated;
        return {Status::accepted,matched};
    }

    template<class Read,class Write>
    VoiceControlRuntime::MelodicStartResult startRoutedMelodicNote(const MidiDecoder::Event& event,
        const ChannelControls::Channel& channel,const MelodicAllocationInputs& allocation,
        const std::array<PartialSampleInstallInputs,2>& samples,
        const std::array<NormalPartialDspInputs,2>& dsp,const PartControllerState& parts,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write)
    {
        if (failed()) return {VoiceControlRuntime::MelodicStartResult::Status::failed,{},{}};
        return runtime.startRoutedMelodicNote(event,channel,allocation,samples,dsp,notes.allocator,
            installation,lifecycle,mask,parts,data,conversion,waves,read,write);
    }

    enum class StopRequest { invalidInput, deferred, queued, failed };

    // Admission owns reclamation and the lifecycle handoff. Callers retain
    // their pending note across PCM waits, but never copy EG owners themselves.
    template<class Read,class Write>
    bool retireRepeatedNote(unsigned part,uint8_t note,uint8_t selector,uint8_t noteFlags,
        Read&& read,Write&& write)
    {
        if (failed() || runtime.preparationPending() || part>=16) return false;
        auto current=admissionLifecycle();
        const auto result=RetireRepeatedNote(notes.allocator,current,part,note,selector,
            noteFlags,notes.part(part)->retainedKeys,read,write);
        lifecycle=current;
        return result.has_value();
    }

    template<class Read,class Write>
    std::optional<bool> ensureCapacity(unsigned part,unsigned count,const VoiceCapacityPolicy& policy,
        Read&& read,Write&& write)
    {
        if (failed() || runtime.preparationPending()) return std::nullopt;
        auto current=admissionLifecycle();
        const auto result=EnsureVoiceCapacity(notes.allocator,current,part,count,policy,read,write);
        lifecycle=current;
        return result;
    }

    template<class Read,class Write>
    bool stopGroup(unsigned part,uint8_t group,Read&& read,Write&& write)
    {
        if (failed() || runtime.preparationPending()) return false;
        auto current=admissionLifecycle();
        const auto result=StopAndReclaimGroup(notes.allocator,current,group,part,false,read,write);
        lifecycle=current;
        return result.has_value();
    }

    template<class Read,class Write>
    bool stopPartGroups(unsigned part,Read&& read,Write&& write)
    {
        if (failed() || part>=16) return false;
        for (unsigned n=0;notes.allocator.partHead[part]<128 && n<voiceCapacity;++n)
            if (!stopGroup(part,notes.allocator.partHead[part],read,write)) return false;
        return notes.allocator.partHead[part]>=128;
    }

    template<class Read,class Write>
    VoiceControlRuntime::MelodicStartResult startReusedMelodicNote(const MelodicNoteVelocity& selection,
        unsigned part,const std::array<PartialSampleInstallInputs,2>& samples,
        const std::array<NormalPartialDspInputs,2>& dsp,const PartControllerState& parts,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write,uint8_t group=255)
    {
        if (failed()) return {VoiceControlRuntime::MelodicStartResult::Status::failed,{},{}};
        return runtime.startReusedMelodicNote(selection,part,samples,dsp,notes.allocator,
            installation,lifecycle,mask,parts,data,conversion,waves,read,write,group);
    }

    struct RhythmStartResult
    {
        enum class Status { invalidInput, deferred, absent, velocityRejected, capacityRejected, started, failed };
        Status status;
        std::optional<MelodicAllocationResult> allocation;
        std::optional<VoiceControlRuntime::MelodicStartResult> start;
    };

    // One already-routed/velocity-adjusted rhythm note. Map/default ownership
    // stays with the GS layer. Admission (including chokes) commits only once;
    // started consumes the event and pollStart resumes PCM activation.
    // After admission begins a failure is terminal, never a retryable defer.
    template<class Read,class Write>
    RhythmStartResult startRoutedRhythmNote(const RhythmNoteVelocity& selection,unsigned part,
        uint8_t noteFlags,const RhythmKeyMap& map,uint8_t shift,uint8_t offset,
        const std::array<uint8_t,12>& scale,const std::array<uint8_t,2>& sampleModes,
        const std::array<uint8_t,2>& preparationFlags,const VoiceCapacityPolicy& policy,
        const std::array<NormalPartialDspInputs,2>& dsp,const PartControllerState& controllers,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write)
    {
        using Status = RhythmStartResult::Status;
        if (failed()) return {Status::failed,{},{}};
        if (part >= 16 || !(noteFlags&0x10) || selection.mapping.key >= 128
            || map.tones[selection.mapping.key] != selection.mapping.tone)
            return {Status::invalidInput,{},{}};
        if (runtime.startupPending() || mask.prepared) return {Status::deferred,{},{}};
        for (const auto& state : lifecycle)
            if (state.pendingOperation != VoiceOperation::none) return {Status::deferred,{},{}};
        // Admission stops must use current periodic DSP progress, not the
        // installation snapshot. Preserve owners until their task4 is serviced.
        lifecycle=admissionLifecycle();
        RhythmNoteAdmission admission(selection,part,noteFlags,notes.part(part)->retainedKeys,policy);
        const auto admitted = admission.run(data,notes.allocator,lifecycle,read,write);
        using Admission = RhythmNoteAdmission::Status;
        if (admitted == Admission::invalidInput) return {Status::invalidInput,{},{}};
        if (admitted == Admission::absent) return {Status::absent,{},{}};
        if (admitted == Admission::velocityRejected) return {Status::velocityRejected,{},{}};
        const auto fail = [&]() -> RhythmStartResult {
            rhythmFailed_ = true; return {Status::failed,{},{}};
        };
        if (admitted != Admission::allocated && admitted != Admission::capacityRejected) return fail();
        // Choked slots can differ from this note's new destinations. Publish
        // every stop before installing task2, otherwise task4 could outrank it.
        for (unsigned count = 0; count < voiceCapacity; ++count)
        {
            const auto stopped = serviceStopTask();
            if (stopped.status == VoiceControlRuntime::StopTaskStatus::idle) break;
            if (stopped.status != VoiceControlRuntime::StopTaskStatus::completed) return fail();
        }
        if (admitted == Admission::capacityRejected) return {Status::capacityRejected,{},{}};
        const auto& allocation = *admission.allocation();
        const auto inputs = PrepareRhythmSampleInputs(allocation,map,shift,offset,scale,sampleModes,preparationFlags);
        if (!inputs) return fail();
        const auto started = runtime.beginAllocatedNote(allocation,part,*inputs,dsp,
            notes.allocator,installation,lifecycle,mask,controllers,data,conversion,waves,read,write);
        if (started.status != VoiceControlRuntime::MelodicStartResult::Status::started
            && started.status != VoiceControlRuntime::MelodicStartResult::Status::preparedOnly) return fail();
        return {Status::started,allocation,started};
    }

    // Channel/reset command boundary. Publish once after validated note-state
    // changes. This does not advance PCM time or destroy live DSP owners.
    bool releasePart(unsigned part,bool rhythm,bool resetPedals,bool allNotes) noexcept
    {
        if (part >= 16 || failed() || runtime.startupPending() || noteOn.pending()) return false;
        auto updated = notes;
        if ((resetPedals && !updated.resetPedals(part))
            || (allNotes && !updated.allNotesOff(part,rhythm))
            || !runtime.publishNoteReleases(updated.allocator)) return false;
        notes = updated;
        return true;
    }

    // One committed choke operation, before rhythm allocation. An accepted
    // caller must advance its admission phase, never replay this on a PCM wait.
    // Keep current DSP owners until task4 and the physical stop are serviced.
    template<class Read,class Write>
    StopRequest requestRhythmChoke(unsigned part,uint8_t selector,Read&& read,Write&& write)
    {
        if (failed()) return StopRequest::failed;
        if (part >= 16) return StopRequest::invalidInput;
        if (selector == 0) return StopRequest::queued;
        if (runtime.startupPending()) return StopRequest::deferred;
        auto checked = notes.allocator;
        std::array<VoiceStopState,voiceCapacity> probe{};
        if (!StopRhythmExclusiveGroups(checked,probe,part,selector,
            [](uint8_t) { return uint8_t(0); },[](uint8_t,uint8_t) {})) return StopRequest::invalidInput;
        auto stopped = lifecycle;
        for (unsigned slot = 0; slot < voiceCapacity; ++slot)
            if (probe[slot].pendingOperation == VoiceOperation::finishStop)
            {
                if (!runtime.voices[slot]) return StopRequest::failed;
                if (lifecycle[slot].pendingOperation != VoiceOperation::none) return StopRequest::deferred;
                stopped[slot] = runtime.voices[slot]->lifecycle;
            }
        // All links and affected owners validated before the first PCM write.
        if (!StopRhythmExclusiveGroups(notes.allocator,stopped,part,selector,read,write))
            return StopRequest::failed;
        lifecycle = stopped;
        return StopRequest::queued;
    }

    template<class Read,class Write>
    StopRequest requestStop(unsigned slot,Read&& read,Write&& write)
    {
        if (failed()) return StopRequest::failed;
        if (slot >= voiceCapacity || !runtime.voices[slot]) return StopRequest::invalidInput;
        if (runtime.startupPending() || lifecycle[slot].pendingOperation != VoiceOperation::none) return StopRequest::deferred;
        // The running DSP state, not an obsolete installation snapshot, is
        // authoritative for cached words/progress before a physical stop.
        auto stopped = runtime.voices[slot]->lifecycle;
        if (!StopPreparedVoice(slot,stopped,read,write)) return StopRequest::invalidInput;
        stopped.pendingOperation = VoiceOperation::finishStop;
        lifecycle[slot] = stopped;
        return StopRequest::queued;
    }

    VoiceControlRuntime::StopTaskResult serviceStopTask()
    { return failed() ? VoiceControlRuntime::StopTaskResult{VoiceControlRuntime::StopTaskStatus::failed}
                      : runtime.serviceStopTask(lifecycle,notes.allocator); }

    // Part-level stop intent belongs with physical voice ownership. The
    // controller neither scans slots nor decides whether PCM preparation
    // permits retirement. Pending requests survive a deferred activation.
    void requestPartStops(uint16_t parts) noexcept { pendingPartStops_|=parts; }
    bool partStopsPending() const noexcept { return pendingPartStops_!=0; }
    bool operationsPending() const noexcept
    {
        for(const auto& state:lifecycle)
            if(state.pendingOperation!=VoiceOperation::none) return true;
        return false;
    }

    enum class ResetProgress { waiting, stopped, completed, failed };
    // Called only while a sound reset is requested. Normal retirement/control
    // service and PCM rendering continue between calls. 'stopped' occurs once:
    // the controller then resets its dynamic MIDI controls before draining.
    template<class Read,class Write>
    ResetProgress resetVoices(Read&& read,Write&& write)
    {
        if(failed()) return ResetProgress::failed;
        if(!resetDraining_) {
            if(runtime.startupPending() || operationsPending()) return ResetProgress::waiting;
            if(stopSoundingParts(0xffff,read,write)!=StopRequest::queued) return ResetProgress::failed;
            resetDraining_=true;
            return ResetProgress::stopped;
        }
        if(operationsPending() || notes.allocator.freeCount!=notes.allocator.voiceLimit) return ResetProgress::waiting;
        for(unsigned slot=0;slot<voiceCapacity;++slot) if(runtime.voices[slot]) {
            const auto ready=PollVoiceReuse(uint8_t(slot),false,read,write);
            if(!ready || *ready==VoiceReuseReadiness::cancelled) return ResetProgress::failed;
            if(*ready!=VoiceReuseReadiness::ready) return ResetProgress::waiting;
        }
        // Logical release alone is insufficient: PCM must have finished its
        // ramps before discarding the old owners. Preserve receive-time work,
        // the shared clock, key mask and cross-note pitch history.
        const auto retainedClock=clock;
        const auto retainedPeriodicClock=periodicClock_;
        const auto enabled=mask.enabled;
        const auto retainedKeys=preparation.partKeys;
        const auto reference=preparation.reference;
        const auto receivedCommands=commands;
        const auto receivedAdmission=admission;
        const auto limit=notes.allocator.voiceLimit;
        *this=NativeVoiceEngine{};
        commands=receivedCommands; admission=receivedAdmission;
        clock=retainedClock; mask.enabled=enabled;
        periodicClock_=retainedPeriodicClock;
        preparation.partKeys=retainedKeys; preparation.reference=reference;
        if(!notes.allocator.initializeTables(limit,limit)) return ResetProgress::failed;
        for(unsigned slot=0;slot<voiceCapacity;++slot)
            runtime.first[slot].firstStage=runtime.second[slot].firstStage=22;
        return ResetProgress::completed;
    }

    template<class Read,class Write>
    StopRequest stopSoundingParts(uint16_t parts,Read&& read,Write&& write)
    {
        if(failed()) return StopRequest::failed;
        if(runtime.startupPending()) return StopRequest::deferred;
        bool deferred=false;
        for(unsigned slot=0;slot<voiceCapacity;++slot)
            if(runtime.voices[slot] && !(notes.allocator.allocations[slot].status&0x80)
                && (parts&(1u<<installation.voices[slot].input.part))) {
                const auto result=requestStop(slot,read,write);
                if(result==StopRequest::failed || result==StopRequest::invalidInput) return result;
                deferred|=result==StopRequest::deferred;
            }
        return deferred ? StopRequest::deferred : StopRequest::queued;
    }

    template<class Read,class Write>
    bool serviceRetirements(Read&& read,Write&& write)
    {
        if(pendingPartStops_) {
            const auto result=stopSoundingParts(pendingPartStops_,read,write);
            if(result==StopRequest::failed || result==StopRequest::invalidInput) return false;
            if(result==StopRequest::queued) pendingPartStops_=0;
        }
        for(unsigned count=0;count<voiceCapacity && !runtime.startupPending();++count)
            if(serviceStopTask().status!=VoiceControlRuntime::StopTaskStatus::completed) break;
        return !failed();
    }

    template<class Read,class Write>
    VoiceControlRuntime::StartStatus pollStart(Read&& read,Write&& write)
    { return failed() ? VoiceControlRuntime::StartStatus::cancelled : runtime.pollPreparedStart(lifecycle,mask,read,write); }

    // Gain decay yields to the next common kernel tick. Key-latch protection
    // is a separate device-pass wait and must still be polled each PCM pass.
    // The deadline belongs to the activation, not the host/player scheduler.
    bool activationWaiting(uint64_t now) const noexcept
    { return activationWake_ && now<*activationWake_; }
    std::optional<uint32_t> activationDeadline(uint64_t now) const noexcept
    {
        if(!activationWaiting(now)) return std::nullopt;
        return uint32_t(*activationWake_-now);
    }
    template<class Read,class Write>
    VoiceControlRuntime::StartStatus serviceActivation(uint64_t now,Read&& read,Write&& write)
    {
        using Status=VoiceControlRuntime::StartStatus;
        if(failed()) return Status::cancelled;
        if(activationWaiting(now)) return Status::waitingForReuse;
        activationWake_.reset();
        const auto result=pollStart(read,write);
        if(result==Status::waitingForReuse) activationWake_=now+clock.untilNextKernelTick();
        return result;
    }

    // 0760..0774 acknowledges the device and latches one bit per voice even
    // while task2 waits for reuse. The deferred handler is a separate service.
    bool acceptsPcmBoundary() const noexcept
    { return !failed() && !runtime.preparationPending() && !runtime.startupAwaitingKeyLatch(); }

    bool receivePcmBoundary(unsigned slot) noexcept
    {
        if(slot>=voiceCapacity || !acceptsPcmBoundary()) return false;
        pendingBoundaries_|=VoiceSet::single(slot);
        lifecycle[slot].fieldCB30=255;
        if(runtime.voices[slot]) runtime.voices[slot]->lifecycle.fieldCB30=255;
        return true;
    }

    bool pcmBoundaryPending() const noexcept { return pendingBoundaries_!=0; }

    template<class Read,class Write>
    std::optional<unsigned> servicePcmBoundaries(const PitchConversion& conversion,Read&& read,Write&& write)
    {
        if(failed()) return std::nullopt;
        if(!pendingBoundaries_) return 0;
        // 5488..54b4: voice operations/event0 precede boundary/event1.
        // 5738 waits only for the reuse tick, not the boundary event.
        if(runtime.startupPending() || voiceCommandsPending()) return 0;
        for(const auto& voice:lifecycle) if(voice.pendingOperation != VoiceOperation::none) return 0;
        unsigned handled=0;
        for(unsigned slot=voiceCapacity;slot-- >0;) if(pendingBoundaries_&(VoiceSet::single(slot))) {
            pendingBoundaries_&=~(VoiceSet::single(slot));
            // Physical stop (53e6) and wave installation (577e) invalidate
            // an old notification; their existing lifecycle clear owns this.
            if(!lifecycle[slot].fieldCB30) continue;
            lifecycle[slot].fieldCB30=0;
            if(runtime.voices[slot]) runtime.voices[slot]->lifecycle.fieldCB30=0;
            if(!handlePcmBoundary(slot,conversion,read,write)) return std::nullopt;
            ++handled;
        }
        return handled;
    }

    // Deferred waveform-boundary operation. Reception and acknowledgement
    // belong to receivePcmBoundary; do not run it inside activation.
    template<class Read,class Write>
    bool handlePcmBoundary(unsigned slot,const PitchConversion& conversion,Read&& read,Write&& write)
    {
        if(failed() || runtime.startupPending() || slot>=voiceCapacity) return false;
        auto& voice=runtime.voices[slot];
        if(!voice) return true;
        if(!HandleVoicePcmBoundary(slot,*voice,runtime.inputs[slot],notes.allocator.pcmLinks,
            conversion,read,write)) return false;
        lifecycle[slot]=voice->lifecycle;
        runtime.first[slot].firstStage=runtime.second[slot].firstStage=voice->lifecycle.stages[0];
        return true;
    }

    bool periodicOwnersReady() const noexcept
    {
        if(runtime.preparationPending()) return false;
        for(unsigned slot=0;slot<voiceCapacity;++slot) {
            const auto& voice=lifecycle[slot];
            if(voice.pendingOperation == VoiceOperation::none) continue;
            if(voice.pendingOperation==VoiceOperation::finishStop) {
                // A physical stop has already published stage18/20 (53e6).
                if(!runtime.voices[slot] || (voice.stages[0]!=18 && voice.stages[0]!=20)) return false;
            } else if(voice.pendingOperation==VoiceOperation::prepare) {
                // 113a publishes identity/request before DSP preparation.
                // A restart has a stopped stage; a continuation retains its
                // old EG/LFO owner but uses the newly installed part/key.
                // No previous DSP owner is a different handoff: retain the
                // conservative wait rather than inventing one for a link.
                if(!runtime.voices[slot] || voice.stages[0]>24 || (voice.stages[0]&1)) return false;
            } else return false;
        }
        return true;
    }

    bool periodicWorkPending() const noexcept
    { return runtime.controlPending() || periodicClock_.has_value(); }
    bool controlEventCaptured() const noexcept { return periodicClock_.has_value(); }

    // One common sound-control event: effects first, then the voice pass.
    // The effect updater is synchronous and audio-owned, not a UI callback.
    // Capture elapsed time before effects; expirations during a deferred voice
    // pass accumulate in clock for the next event, never in this event's copy.
    template<class Read,class Write,class UpdateEffects>
    VoiceControlRuntime::ScheduledResult updateControl(const PartControllerState& parts,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write,bool effectsEnabled,UpdateEffects&& updateEffects)
    {
        using Status=VoiceControlRuntime::ScheduledStatus;
        if(failed()) return {Status::failed};
        if(effectsEnabled && !runtime.startupAwaitingKeyLatch() && periodicOwnersReady()) {
            if(!periodicClock_ && clock.ready()) {
                periodicClock_=clock;
                (void)clock.consume();
            }
            if(periodicClock_ && !runtime.controlPending() && !updateEffects()) return {Status::failed};
        }
        const auto result=serviceControl(parts,data,conversion,waves,read,write,
            periodicClock_ ? &*periodicClock_ : nullptr,VoiceControlRuntime::ControlSlice::pass);
        if(result.status==Status::updated) periodicClock_.reset();
        return result;
    }

    template<class Read,class Write>
    VoiceControlRuntime::ControlStep resumeControl(const PartControllerState& parts,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write,VoiceControlRuntime::ControlSlice slice=VoiceControlRuntime::ControlSlice::pass)
    {
        using Progress=VoiceControlRuntime::ControlProgress;
        if(failed()) return {Progress::failed};
        if(runtime.voiceCompletionPending() && voiceCommandsPending()) return {Progress::deferred};
        if(!importPendingVoiceOperations()) return {Progress::deferred};
        const auto result=slice==VoiceControlRuntime::ControlSlice::phase
            ? runtime.resumeControlPhase(installation,parts,notes.allocator,data,conversion,waves,read,write,!voiceCommandsPending())
            : runtime.resumeControlPass(installation,parts,notes.allocator,data,conversion,waves,read,write,!voiceCommandsPending());
        if(result.status!=Progress::failed) exportControlChanges(result.changedMask);
        return result;
    }

    template<class Read,class Write>
    VoiceControlRuntime::ScheduledResult serviceControl(const PartControllerState& parts,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write,ControlTaskClock* capturedClock = nullptr,
        VoiceControlRuntime::ControlSlice slice=VoiceControlRuntime::ControlSlice::pass)
    {
        using Status = VoiceControlRuntime::ScheduledStatus;
        if (failed()) return {Status::failed};
        if(runtime.voiceCompletionPending() && voiceCommandsPending()) return {Status::deferred};
        if(!importPendingVoiceOperations()) return {Status::deferred};
        const auto result = runtime.serviceControl(capturedClock ? *capturedClock : clock,installation,parts,notes.allocator,
            data,conversion,waves,read,write,slice,!voiceCommandsPending());
        if (result.status != Status::failed) exportControlChanges(result.updatedMask);
        return result;
    }
private:
    std::optional<ControlTaskClock> periodicClock_;
    std::optional<PendingAdmission> admission;
    void applyMaster(const Configuration& config,VoiceControlInputs& input) const noexcept
    {
        input.level.master = config.master.volume;
        input.spatial.masterPan = config.master.pan;
        input.pitch.masterTune = config.master.tune;
    }
    void applyPart(const Configuration& config,unsigned part,VoiceControlInputs& input,unsigned map = 255,unsigned key = 0) const noexcept
    {
        applyMaster(config,input);
        input.glideRate=mono[part].glideRate;
        ApplyChannelOutputControls(config.parts.parts[part].controls,input.level,input.spatial);
        input.pitch.partTune=uint16_t(config.parts.parts[part].controls.finePitch());
        input.correctionSource=config.parts.parts[part].fineTune;
        const auto& tone=config.parts.parts[part].controls.tone.values;
        input.amplitude={tone[4],tone[5],tone[6]};
        input.secondTiming.attack=tone[4]; input.secondTiming.decay=tone[5]; input.secondTiming.release=tone[6];
        input.second.control=tone[2]; input.secondController=tone[3];
        input.level.has_tone_scale = input.spatial.hasToneScale = map < 2;
        if (map < 2)
        {
            const auto output=config.rhythm.output(map,key);
            input.level.tone_scale = output.level;
            input.spatial.panScale = output.pan;
            input.spatial.reverbScale = output.reverb;
            input.spatial.chorusScale = output.chorus;
        }
        if (!config.effectsAvailable) input.spatial.reverb = input.spatial.chorus = 0;
    }
    void applyToneModulation(const Configuration& config,unsigned part,FirstModulationInputs& input) const noexcept
    {
        const auto& tone=config.parts.parts[part].controls.tone.values;
        input.rateControl=tone[0]; input.depthControl=tone[1]; input.delayControl=tone[7];
    }
    template<class Read,class Write>
    void prepareAdmission(const Configuration& config,const PartControllerState& controllers,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write,AdmissionResult& outcome)
    {
        if(!admission || runtime.startupPending() || operationsPending()) return;
        auto event = admission->event();
        const auto originalNote=event.first;
        const auto part = admission->request.part;
        if(part>=16) { outcome.failed=true; return; }
        const auto& settings = config.parts.parts[part];
        const auto& channel = settings.controls;
        if (config.parts.routing[part].noteFlags&0x10)
        {
            startRhythm(event,part,config,controllers,data,conversion,waves,read,write,outcome);
            return;
        }
        const bool high=event.first>=125;
        if (!high && !(config.parts.routing[part].noteFlags&0x80)) { startMonoOrSource(event,part,false,config,controllers,data,conversion,waves,read,write,outcome); return; }
        if(!high && mono[part].source<128) { startMonoOrSource(event,part,true,config,controllers,data,conversion,waves,read,write,outcome); return; }
        MelodicAllocationInputs allocation{settings.bank,false,0,part,0x80,255,
            {},0,{},admission->request.tone};
        if(high) {
            const auto* patch=admission->request.tone ? data.patch(*admission->request.tone) : nullptr;
            const auto mapping=patch ? MapHighNote(event.first,patch->common) : std::nullopt;
            if(!mapping) { outcome.failed=true; return; }
            if(mapping->tone&0x8000) { admission.reset(); return; }
            event.first=mapping->note;
            allocation.resolvedTone=mapping->tone; allocation.groupFlags=0x81;
            allocation.groupNote=originalNote;
            allocation.keyRange={}; allocation.velocityAdjustment={};
        }
        const auto preview=previewMelodicAdmission(event,channel,allocation,
            config.parts.routing[part].noteFlags,data,read,write);
        if(!preview) { outcome.failed=true; return; }
        const auto& selected=*preview;
        using Allocated = MelodicAllocationResult::Status;
        if (selected.status == Allocated::needsCapacity)
        {
            // The MIDI event is already owned by admission; a reclaim is never
            // replayed as a queue callback returning deferred.
            const auto count = selected.selection->partials.candidates.count;
            const auto capacity = ensureCapacity(part,count,config.capacity,read,write);
            if (!capacity) outcome.failed = true;
            else if (!*capacity) admission.reset();
            return;
        }
        if (selected.status == Allocated::keyRangeRejected || selected.status == Allocated::velocityRejected)
        { admission.reset(); return; }
        if (selected.status != Allocated::allocated) { outcome.unsupported=true; admission.reset(); return; }
        const auto key = high ? event.first : TransposeMasterKey(
            TransposePartKey(event.first,settings.keyShift,uint8_t(channel.coarseTuning)),config.master.keyShift);
        // Validate lookup before committing allocation. Negative sample IDs
        // take113e's return-to-free-list path; they are not synthetic waves.
        const auto& patch = *data.patch(selected.selection->tone);
        for (unsigned partial = 0; partial < 2; ++partial)
            if (selected.dispatch[partial].prepare)
            {
                const auto plan = PreparePartialSample(patch.partial[partial],*data.samples(),
                    key,event.first,event.first,settings.scale,!high && mono[part].portamento ? preparation.partKeys[part][partial] : uint8_t(255),high ? 0x81 : 0,event.first);
                if (!plan)
                { outcome.unsupported=true; admission.reset(); return; }
            }
        const PartialSampleInstallInputs sample{settings.scale,key,event.first,event.first,255,0,event.first,0,160};
        std::array<PartialSampleInstallInputs,2> samples{sample,sample};
        VoiceControlInputs controls;
        applyPart(config,part,controls);
        std::array<NormalPartialDspInputs,2> dsp{};
        for (unsigned partial = 0; partial < 2; ++partial)
        {
            // Ordinary poly preparation leaves A1CC/C974 atff. A MIDI key
            // here would manufacture a glide that cancels tuning at startup.
            dsp[partial] = {255,false,0,controls,{},{}};
            if(high) {
                samples[partial].flags=255; samples[partial].mode=0x81;
                dsp[partial].sourceKey=255; dsp[partial].unoffsetStart=true;
            } else if(mono[part].portamento) {
                samples[partial].flags=255;
                samples[partial].minimumKey=preparation.partKeys[part][partial];
                dsp[partial].sourceKey=preparation.partKeys[part][partial];
                dsp[partial].unoffsetStart=true;
            }
            applyToneModulation(config,part,dsp[partial].firstControls);
            const auto slot = selected.dispatch[partial].voice;
            if (slot < voiceCapacity)
            {
                dsp[partial].previousPitch = preparation.previousPitch[slot];
                if (runtime.voices[slot])
                    dsp[partial].previousPitch.glide = runtime.voices[slot]->pitch.glide;
            }
        }
        const auto result = startRoutedMelodicNote(event,channel,allocation,samples,dsp,
            controllers,data,conversion,waves,read,write);
        using Start = VoiceControlRuntime::MelodicStartResult::Status;
        if (result.status == Start::deferred || result.status == Start::needsCapacity) return;
        admission.reset();
        if (result.status != Start::started && result.status != Start::preparedOnly) { outcome.failed = true; return; }
        if(!rememberPreparedNote(part,result,event.first)) { outcome.failed=true; return; }
    }

    template<class Read,class Write>
    void startMonoOrSource(MidiDecoder::Event event,unsigned part,bool polySource,
        const Configuration& config,const PartControllerState& controllers,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write,AdmissionResult& outcome)
    {
        auto& state=mono[part];
        const auto& settings=config.parts.parts[part];
        const auto tone=admission->request.tone;
        if (!tone) { admission.reset(); return; }
        const auto selection=PrepareMappedNoteVelocity(event,*tone,settings.controls.softPedal,0,data);
        if (!selection) { outcome.failed=true; return; }
        if (!selection->partials.candidates.count) {
            if(!polySource && !admission->isHeldReturn()) state.held.set(event.first,true);
            if (!admission->isHeldReturn()) state.source=255;
            if (admission->isHeldReturn() && notes.allocator.partHead[part]<voiceCapacity) {
                if (!notes.allocator.releaseMonoGroup(part)
                    || !runtime.publishNoteReleases(notes.allocator)) outcome.failed=true;
            }
            admission.reset(); return;
        }
        auto& allocator=notes.allocator;
        uint8_t group=allocator.partHead[part];
        if(polySource) {
            const auto found=admission->sourceReuse==PendingAdmission::SourceReuse::fresh ? std::optional<uint8_t>(255)
                : allocator.findSourceGroup(part,state.source);
            if(!found) { outcome.failed=true; return; }
            group=*found;
        }
        if (reuseInvalidated(part) && (!polySource || group<voiceCapacity)) {
            reuseInvalidation_&=uint16_t(~(1u<<part));
            if(polySource && group<voiceCapacity) {
                if(!stopGroup(part,group,
                    read,write)) outcome.failed=true;
                // H8 jumps straight to fresh admission after invalidating
                // this source. Do not search and steal another older source
                // group when the native continuation resumes after PCM work.
                admission->sourceReuse=PendingAdmission::SourceReuse::fresh;
            } else if (!stopPartGroups(part,read,write)) outcome.failed=true;
            return; // Task4 and PCM must settle before the new allocation.
        }
        const bool reuse=group<voiceCapacity;
        const auto source=admission->isHeldReturn() ? uint8_t(255) : state.source;
        const auto reuseFlags=monoReuseFlags(part,group,admission->isHeldReturn(),polySource);
        if (!reuseFlags) { outcome.failed=true; return; }
        const auto flags=*reuseFlags;
        const MelodicAllocationInputs allocation{settings.bank,false,0,uint8_t(part),0x80,255,
            {},0,{},tone};
        // CC84 without a matching source group uses the same fresh-note
        // retirement as ordinary poly (00:0fab -> 17b8), before capacity.
        if(!reuse && !retireAdmission(allocation.groupFlags,config.parts.routing[part].noteFlags,
            read,write)) { outcome.failed=true; return; }
        auto probe=allocator;
        const auto selected=reuse ? PrepareMonoReuseAllocation(*selection,part,data,probe,group)
            : AllocateMelodicNote(event,settings.controls,allocation,data,probe);
        if (selected.status==MelodicAllocationResult::Status::needsCapacity) {
            const auto capacity=ensureCapacity(part,selection->partials.candidates.count,config.capacity,
                read,write);
            if (!capacity) outcome.failed=true;
            else if (!*capacity) {
                if(!polySource && !admission->isHeldReturn()) state.held.set(event.first,true);
                admission.reset(); // Valid note, protected capacity: discard only this admission.
            }
            return;
        }
        if (selected.status!=MelodicAllocationResult::Status::allocated) { outcome.failed=true; return; }
        const auto key=TransposeMasterKey(TransposePartKey(event.first,settings.keyShift,
            uint8_t(settings.controls.coarseTuning)),config.master.keyShift);
        const PartialSampleInstallInputs sample{settings.scale,key,event.first,event.first,255,0,event.first,0,flags};
        std::array<PartialSampleInstallInputs,2> samples{sample,sample};
        VoiceControlInputs controls; applyPart(config,part,controls); controls.glideRate=state.glideRate;
        std::array<NormalPartialDspInputs,2> dsp{};
        for (unsigned partial=0;partial<2;++partial) {
            const auto slot=selected.dispatch[partial].voice;
            dsp[partial]={state.portamento ? preparation.partKeys[part][partial] : uint8_t(255),(flags&128)!=0,0,controls,{},{}};
            if (source<128 && selected.dispatch[partial].prepare) {
                const auto& raw=data.patch(selection->tone)->partial[partial].raw;
                const auto origin=PreparePortamentoSourceKey(TransposeMasterKey(
                    TransposePartKey(source,settings.keyShift,uint8_t(settings.controls.coarseTuning)),
                    config.master.keyShift),preparation.reference,raw[10],raw[13]);
                if (!origin) { outcome.failed=true; return; }
                dsp[partial].sourceKey=*origin;
            }
            samples[partial].minimumKey=dsp[partial].sourceKey;
            applyToneModulation(config,part,dsp[partial].firstControls);
            if (slot<voiceCapacity) dsp[partial].previousPitch=preparation.previousPitch[slot];
        }
        const auto result=reuse
            ? startReusedMelodicNote(*selection,part,samples,dsp,controllers,data,conversion,waves,
                read,write,group)
            : startRoutedMelodicNote(event,settings.controls,allocation,samples,dsp,
                controllers,data,conversion,waves,read,write);
        using Status=VoiceControlRuntime::MelodicStartResult::Status;
        if (result.status==Status::deferred || result.status==Status::needsCapacity) return;
        if (result.status!=Status::started && result.status!=Status::preparedOnly) { outcome.failed=true; return; }
        if(!polySource) { state.current=event.first; state.velocity=event.second; state.tone=tone; }
        if(!polySource && !admission->isHeldReturn()) state.held.set(event.first,true);
        const auto committedGroup=selected.group->group;
        if(polySource && result.requests->count) {
            allocator.noteGroups[committedGroup].key=event.first;
            allocator.noteGroups[committedGroup].status=0;
        }
        if (!admission->isHeldReturn()) state.source=255;
        if(!rememberPreparedNote(part,result,event.first)) { outcome.failed=true; return; }
        admission.reset();
    }

    template<class Read,class Write>
    void startRhythm(MidiDecoder::Event event,unsigned part,
        const Configuration& config,const PartControllerState& controllers,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write,AdmissionResult& outcome)
    {
        if(!config.rhythmPresets) { outcome.unsupported=true; admission.reset(); return; }
        const auto& settings = config.parts.parts[part];
        const auto flags = config.parts.routing[part].noteFlags;
        const unsigned mapIndex = (flags&0x20) ? 0 : 1;
        const auto& map = config.rhythm.map(mapIndex);
        // Receive-time rejection already belongs to the accepted NoteRequest.
        // A later invalid kit must not cancel an earlier accepted request;
        // 00:0c3c reads the live drum map, not the current AB06 receiver gate.
        const auto selected = PrepareRhythmNoteVelocity(event,settings.controls.program,map,
            config.rhythmPresets->program127Accumulators,0,settings.controls.softPedal,data);
        if (!selected) { outcome.unsupported=true; admission.reset(); return; }
        const auto keys = PrepareRhythmInitialKeys(event.first,map,settings.keyShift,uint8_t(settings.controls.coarseTuning));
        if (!keys) { outcome.failed = true; return; }
        VoiceControlInputs controls;
        applyPart(config,part,controls,mapIndex,event.first);
        NormalPartialDspInputs dsp{keys->sourceKey,false,0,controls,{},{}};
        applyToneModulation(config,part,dsp.firstControls);
        const auto result = startRoutedRhythmNote(*selected,part,flags,map,settings.keyShift,
            uint8_t(settings.controls.coarseTuning),settings.scale,{0,0},{160,160},config.capacity,{dsp,dsp},
            controllers,data,conversion,waves,read,
            write);
        using Status = NativeVoiceEngine::RhythmStartResult::Status;
        if (result.status == Status::deferred) return;
        admission.reset();
        if (result.status == Status::failed || result.status == Status::invalidInput) { outcome.failed = true; return; }
        if (result.status != Status::started) return;
        if(!rememberPreparedNote(part,*result.start,keys->sourceKey,uint8_t(mapIndex),event.first)) outcome.failed=true;
    }

    // Program changes invalidate the next applicable mono/source reuse,
    // even when a second change restores the original tone.
    uint16_t reuseInvalidation_=0;
    uint16_t pendingPartStops_=0;
    bool resetDraining_=false;
    std::optional<uint64_t> activationWake_;
    VoiceSet pendingBoundaries_=0;
    bool importPendingVoiceOperations() noexcept
    {
        if(!periodicOwnersReady()) return false;
        // Admission and periodic control see the same published lifecycle.
        // Preserve requests for their consumer: no early stop completion,
        // preparation, key-on or reconstruction of an uninstalled DSP owner.
        for(unsigned slot=0;slot<voiceCapacity;++slot) if((lifecycle[slot].pendingOperation != VoiceOperation::none) && runtime.voices[slot]) {
            runtime.voices[slot]->lifecycle=lifecycle[slot];
            runtime.first[slot].firstStage=runtime.second[slot].firstStage=lifecycle[slot].stages[0];
        }
        return true;
    }
    void exportControlChanges(VoiceSet changed) noexcept
    {
        for (unsigned slot=0;slot<voiceCapacity;++slot)
            if((changed&(VoiceSet::single(slot))) && runtime.voices[slot]) lifecycle[slot]=runtime.voices[slot]->lifecycle;
    }
    std::array<VoiceStopState,voiceCapacity> admissionLifecycle() const noexcept
    {
        // Pending installation/stop owns the handoff until its consumer runs.
        // Otherwise the live EG owner supersedes the installation snapshot.
        auto current=lifecycle;
        for (unsigned slot=0;slot<voiceCapacity;++slot)
            if (runtime.voices[slot] && (lifecycle[slot].pendingOperation == VoiceOperation::none))
                current[slot]=runtime.voices[slot]->lifecycle;
        return current;
    }
    bool rhythmFailed_ = false;
};
}
