#pragma once
#include "sc55_voice_runtime.h"
#include "sc55_note_fanout.h"
#include "sc55_rhythm_admission.h"
#include "sc55_voice_commands.h"

namespace sc55
{
// Serialized native voice-engine state. Not the host adapter or GS router.
// Public state supports explicit boot/import policy; once processing starts,
// only the serialized owner may mutate it. Sound-data/PCM remain caller-owned.
// No implicit reset, MIDI defaults, clock epoch or scheduling priority here.
class NativeVoiceEngine
{
public:
    VoiceControlRuntime runtime;
    PartNoteState notes;
    VoiceInstallationState installation;
    std::array<VoiceStopState,24> lifecycle{};
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
        std::array<PreparedPartPitch,24> previousPitch{};
        std::array<uint8_t,24> drumMap,drumKey{};
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
    std::optional<PendingAdmission> admission;

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
            if(result.requests->entries[i].slot>=24 || !result.prepared || !result.prepared->voices[i]) return false;
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
        if (group>=24 || (!state.portamento && source>=128)) return uint8_t(0xff);
        const auto plan=notes.allocator.prepareGroupReuse(group,{0xff,{255,255}});
        if (!plan) return std::nullopt;
        auto flags=plan->flags;
        if (!heldReturn && !sourceReuse) {
            bool restart=!state.held.highest().has_value();
            for (const auto slot:plan->voices)
                if (slot<24 && runtime.voices[slot])
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
            && notes.allocator.partHead[part]<24 && !notes.allocator.releaseMonoGroup(part)) return std::nullopt;
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
        for (unsigned n=0;notes.allocator.partHead[part]<128 && n<24;++n)
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
        for (unsigned count = 0; count < 24; ++count)
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
        std::array<VoiceStopState,24> probe{};
        if (!StopRhythmExclusiveGroups(checked,probe,part,selector,
            [](uint8_t) { return uint8_t(0); },[](uint8_t,uint8_t) {})) return StopRequest::invalidInput;
        auto stopped = lifecycle;
        for (unsigned slot = 0; slot < 24; ++slot)
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
        if (slot >= 24 || !runtime.voices[slot]) return StopRequest::invalidInput;
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
        if(operationsPending() || notes.allocator.freeCount!=24) return ResetProgress::waiting;
        for(unsigned slot=0;slot<24;++slot) if(runtime.voices[slot]) {
            const auto ready=PollVoiceReuse(uint8_t(slot),false,read,write);
            if(!ready || *ready==VoiceReuseReadiness::cancelled) return ResetProgress::failed;
            if(*ready!=VoiceReuseReadiness::ready) return ResetProgress::waiting;
        }
        // Logical release alone is insufficient: PCM must have finished its
        // ramps before discarding the old owners. Preserve receive-time work,
        // the shared clock, key mask and cross-note pitch history.
        const auto retainedClock=clock;
        const auto enabled=mask.enabled;
        const auto retainedKeys=preparation.partKeys;
        const auto reference=preparation.reference;
        const auto receivedCommands=commands;
        const auto receivedAdmission=admission;
        *this=NativeVoiceEngine{};
        commands=receivedCommands; admission=receivedAdmission;
        clock=retainedClock; mask.enabled=enabled;
        preparation.partKeys=retainedKeys; preparation.reference=reference;
        if(!notes.allocator.initializeTables()) return ResetProgress::failed;
        for(unsigned slot=0;slot<24;++slot)
            runtime.first[slot].firstStage=runtime.second[slot].firstStage=22;
        return ResetProgress::completed;
    }

    template<class Read,class Write>
    StopRequest stopSoundingParts(uint16_t parts,Read&& read,Write&& write)
    {
        if(failed()) return StopRequest::failed;
        if(runtime.startupPending()) return StopRequest::deferred;
        bool deferred=false;
        for(unsigned slot=0;slot<24;++slot)
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
        for(unsigned count=0;count<24 && !runtime.startupPending();++count)
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
        if(slot>=24 || !acceptsPcmBoundary()) return false;
        pendingBoundaries_|=1u<<slot;
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
        for(unsigned slot=24;slot-- >0;) if(pendingBoundaries_&(1u<<slot)) {
            pendingBoundaries_&=~(1u<<slot);
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
        if(failed() || runtime.startupPending() || slot>=24) return false;
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
        for(unsigned slot=0;slot<24;++slot) {
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
    uint16_t pendingPartStops_=0;
    bool resetDraining_=false;
    std::optional<uint64_t> activationWake_;
    uint32_t pendingBoundaries_=0;
    bool importPendingVoiceOperations() noexcept
    {
        if(!periodicOwnersReady()) return false;
        // Admission and periodic control see the same published lifecycle.
        // Preserve requests for their consumer: no early stop completion,
        // preparation, key-on or reconstruction of an uninstalled DSP owner.
        for(unsigned slot=0;slot<24;++slot) if((lifecycle[slot].pendingOperation != VoiceOperation::none) && runtime.voices[slot]) {
            runtime.voices[slot]->lifecycle=lifecycle[slot];
            runtime.first[slot].firstStage=runtime.second[slot].firstStage=lifecycle[slot].stages[0];
        }
        return true;
    }
    void exportControlChanges(uint32_t changed) noexcept
    {
        for (unsigned slot=0;slot<24;++slot)
            if((changed&(1u<<slot)) && runtime.voices[slot]) lifecycle[slot]=runtime.voices[slot]->lifecycle;
    }
    std::array<VoiceStopState,24> admissionLifecycle() const noexcept
    {
        // Pending installation/stop owns the handoff until its consumer runs.
        // Otherwise the live EG owner supersedes the installation snapshot.
        auto current=lifecycle;
        for (unsigned slot=0;slot<24;++slot)
            if (runtime.voices[slot] && (lifecycle[slot].pendingOperation == VoiceOperation::none))
                current[slot]=runtime.voices[slot]->lifecycle;
        return current;
    }
    bool rhythmFailed_ = false;
};
}
