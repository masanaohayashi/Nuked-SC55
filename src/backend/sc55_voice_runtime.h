#pragma once
#include "sc55_note_start.h"
#include "sc55_voice_control.h"
#include "sc55_voice_prepare.h"
#include "sc55_control_clock.h"
#include "sc55_midi_queue.h"

namespace sc55
{
// Prepared physical voices and their periodic DSP owners. This is not the
// MIDI note allocator/scheduler: serialize installation, MIDI changes and PCM
// access with it. Prepared startup is resumed by the caller as PCM time passes.
class VoiceControlRuntime
{
public:
    std::array<std::optional<VoiceControlState>,24> voices{};
    std::array<VoiceControlInputs,24> inputs{};
    std::array<FirstModulationInputs,24> firstInputs{};
    std::array<FirstModulationVoice,24> first{};
    std::array<VoiceModulation,24> second{};
    std::array<uint8_t,24> secondSources;
    std::array<VoiceControllerState,24> controllers{};
    std::array<std::optional<VoiceControlResult>,24> lastResults{};
    std::array<std::optional<VoicePcmUpdateResult>,24> lastWrites{};

    VoiceControlRuntime() noexcept { secondSources.fill(24); }
    bool failed() const noexcept { return failed_; }
    bool controlPending() const noexcept { return controlPending_; }

    // Serialize delivery with startup/periodic DSP. The supplied receiver owns
    // MIDI interpretation and may start a prepared batch before accepting an
    // event. Later events remain queued until that activation completes.
    template<std::size_t Capacity,class Sink>
    MidiDispatchResult serviceMidi(MidiEventQueue<Capacity>& queue,uint64_t now,Sink&& sink)
    {
        if (failed_ || queue.failed()) { failed_ = true; return MidiDispatchResult::failed; }
        if (startupPending()) return MidiDispatchResult::deferred;
        const auto result = queue.dispatchOne(now,sink);
        if (result == MidiDispatchResult::failed) failed_ = true;
        return result;
    }

    using StartStatus = PreparedVoiceBatch::Status;
    bool preparationPending() const noexcept { return preparation_.has_value() || dspPreparation_.has_value(); }
    bool startupAwaitingKeyLatch() const noexcept
    { return startup_.status()==StartStatus::waitingForKeyLatch; }
    bool startupPending() const noexcept
    {
        return preparationPending() || startup_.status() == StartStatus::waitingForReuse
            || startup_.status() == StartStatus::waitingForKeyLatch;
    }
#if defined(SC55_NATIVE_IO_AUDIT)
    struct StartupAudit { StartStatus status; uint32_t channels; };
    StartupAudit startupAudit() const noexcept
    {
        uint32_t channels=0;
        if(startupPending()) for(unsigned i=0;i<pendingStart_.count;++i) channels|=1u<<startSlots_[i];
        return {startup_.status(),channels};
    }
#endif

    enum class StopTaskStatus { idle, deferred, needsPreparation, completed, failed };
    struct StopTaskResult { StopTaskStatus status; uint8_t slot = 255; };

    // Service at most one task4 in firmware priority order. Task2 is left for
    // preparation, not skipped to find a lower-priority stop. The external
    // lifecycle holds the result of StopPreparedVoice; publish that result to
    // the periodic owner without restarting its envelopes or touching PCM.
    // Subsequent periodic passes poll the stop level and reclaim the voice.
    StopTaskResult serviceStopTask(std::array<VoiceStopState,24>& lifecycle,VoiceAllocator& allocator)
    {
        if (failed_) return {StopTaskStatus::failed};
        if (startupPending()) return {StopTaskStatus::deferred};
        auto next = lifecycle; auto activity = allocator.activity;
        const auto task = DispatchNextVoiceTask(next,allocator.pcmLinks,activity);
        if (!task) { failed_ = true; return {StopTaskStatus::failed}; }
        if (task->kind == VoiceTaskDispatch::Kind::idle) return {StopTaskStatus::idle};
        if (task->kind == VoiceTaskDispatch::Kind::prepare) return {StopTaskStatus::needsPreparation,task->slots[0]};
        const auto slot = task->slots[0];
        if (slot >= 24 || !voices[slot]) { failed_ = true; return {StopTaskStatus::failed}; }
        lifecycle = next; allocator.activity = activity;
        voices[slot]->lifecycle = next[slot];
        first[slot].firstStage = second[slot].firstStage = next[slot].stages[0];
        return {StopTaskStatus::completed,slot};
    }

    struct MelodicStartResult
    {
        enum class Status { invalidInput, deferred, needsCapacity, keyRangeRejected, velocityRejected, started, preparedOnly, failed, preparing };
        Status status;
        std::optional<DispatchedNormalVoiceInputs> requests;
        std::optional<PreparedNormalVoiceBatch> prepared;
        PartialPitchHistoryUpdate pitchHistory{};
    };

    // Complete normal restarted Note On entry for one already-routed part.
    // Routing, GS policy, previous-key ownership and capacity policy stay with
    // the engine. No wait or PCM time advancement: started reserves an async
    // activation, and the caller consumes that MIDI event exactly once.
    // Deferred/needsCapacity/invalidInput have no allocation or PCM side effects.
    // Once allocation succeeds, any later failure latches: do not retry the
    // note, since group, PCM or DSP state may already have changed.
    template<class Read,class Write>
    MelodicStartResult startRoutedMelodicNote(const MidiDecoder::Event& event,
        const ChannelControls::Channel& channel,const MelodicAllocationInputs& allocationInput,
        const std::array<PartialSampleInstallInputs,2>& sampleInputs,
        const std::array<NormalPartialDspInputs,2>& dspInputs,
        VoiceAllocator& allocator,VoiceInstallationState& installation,
        std::array<VoiceStopState,24>& lifecycle,VoiceKeyMask& mask,
        const PartControllerState& parts,const SoundData& data,const PitchConversion& conversion,
        const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        using Status = MelodicStartResult::Status;
        if (failed_) return {Status::failed,{},{}};
        if (startupPending() || mask.prepared != 0) return {Status::deferred,{},{}};
        bool pendingTask = false;
        for (const auto& voice : lifecycle)
        {
            if (voice.fieldCAF4 != 0 && voice.fieldCAF4 != 2 && voice.fieldCAF4 != 4)
            { failed_ = true; return {Status::failed,{},{}}; }
            pendingTask |= voice.fieldCAF4 != 0;
        }
        if (pendingTask) return {Status::deferred,{},{}};
        const auto allocation = AllocateMelodicNote(event,channel,allocationInput,data,allocator);
        using Allocated = MelodicAllocationResult::Status;
        if (allocation.status == Allocated::invalidInput) return {Status::invalidInput,{},{}};
        if (allocation.status == Allocated::keyRangeRejected) return {Status::keyRangeRejected,{},{}};
        if (allocation.status == Allocated::needsCapacity) return {Status::needsCapacity,{},{}};
        if (allocation.status == Allocated::velocityRejected) return {Status::velocityRejected,{},{}};
        return beginAllocatedNote(allocation,allocationInput.part,sampleInputs,dspInputs,allocator,installation,lifecycle,mask,
            parts,data,conversion,waves,read,write);
    }

    // Existing-group entry: selection and invalidation/restart policy belong
    // to the MIDI part owner. Installation and DSP use the same transaction
    // as a fresh note, but no group/voice is allocated or silently substituted.
    template<class Read,class Write>
    MelodicStartResult startReusedMelodicNote(const MelodicNoteVelocity& selection,unsigned part,
        const std::array<PartialSampleInstallInputs,2>& sampleInputs,
        const std::array<NormalPartialDspInputs,2>& dspInputs,
        VoiceAllocator& allocator,VoiceInstallationState& installation,
        std::array<VoiceStopState,24>& lifecycle,VoiceKeyMask& mask,
        const PartControllerState& parts,const SoundData& data,const PitchConversion& conversion,
        const LfoWaveformTables& waves,Read&& read,Write&& write,uint8_t group=255)
    {
        using Status=MelodicStartResult::Status;
        if (failed_) return {Status::failed,{},{}};
        if (startupPending() || mask.prepared) return {Status::deferred,{},{}};
        for (const auto& state:lifecycle)
            if (state.fieldCAF4) return {Status::deferred,{},{}};
        const auto allocation=PrepareMonoReuseAllocation(selection,part,data,allocator,group);
        if (allocation.status==MelodicAllocationResult::Status::velocityRejected)
            return {Status::velocityRejected,{},{}};
        if (allocation.status!=MelodicAllocationResult::Status::allocated)
            return {Status::invalidInput,{},{}};
        for (const auto& destination:allocation.dispatch)
            if (destination.prepare && destination.voice<24 && !voices[destination.voice])
                return {Status::invalidInput,{},{}};
        return beginAllocatedNote(allocation,part,sampleInputs,dspInputs,allocator,installation,lifecycle,mask,
            parts,data,conversion,waves,read,write);
    }

    enum class PreparationSlice { note, partial };

    // All admitted notes (fresh, reused and rhythm) enter this owned operation.
    // No caller-local sample/DSP inputs survive a yield. A partial slice stops
    // between installation/DSP steps; the normal path drains these exact steps.
    // Admission is already committed, so preparation failure is terminal.
    template<class Read,class Write>
    MelodicStartResult beginAllocatedNote(const MelodicAllocationResult& allocation,unsigned part,
        const std::array<PartialSampleInstallInputs,2>& samples,
        const std::array<NormalPartialDspInputs,2>& dsp,
        VoiceAllocator& allocator,VoiceInstallationState& installation,
        std::array<VoiceStopState,24>& lifecycle,VoiceKeyMask& mask,
        const PartControllerState& parts,const SoundData& data,const PitchConversion& conversion,
        const LfoWaveformTables& waves,Read&& read,Write&& write,
        PreparationSlice slice=PreparationSlice::note)
    {
        using Status=MelodicStartResult::Status;
        if(failed_) return {Status::failed,{},{}};
        if(startupPending() || mask.prepared) return {Status::deferred,{},{}};
        const auto plan=MelodicSampleInstallation::prepare(allocation,part,samples,data,allocator,installation);
        if(!plan) { failed_=true; return {Status::failed,{},{}}; }
        preparation_.emplace(NotePreparation{*plan,*allocation.selection,dsp});
        for(unsigned step=0;step<5;++step) {
            auto result=resumeNotePreparation(allocator,installation,lifecycle,mask,
                parts,data,conversion,waves,read,write);
            if(slice==PreparationSlice::partial || result.status!=Status::preparing) return result;
        }
        failed_=true;
        return {Status::failed,{},{}};
    }

    template<class Read,class Write>
    MelodicStartResult resumeNotePreparation(VoiceAllocator& allocator,VoiceInstallationState& installation,
        std::array<VoiceStopState,24>& lifecycle,VoiceKeyMask& mask,
        const PartControllerState& parts,const SoundData& data,const PitchConversion& conversion,
        const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        using Status=MelodicStartResult::Status;
        if(failed_) return {Status::failed,{},{}};
        if(pendingDspNote_) {
            const auto step=resumeNormalPreparation(lifecycle,mask,data,conversion,waves,read,write);
            if(step.status==NormalVoiceDspPreparation::Progress::failed) {
                failed_=true; return {Status::failed,{},{}};
            }
            if(!step.prepared) return {Status::preparing,{},{}};
            const auto note=*pendingDspNote_;
            pendingDspNote_.reset();
            return {Status::started,note.requests,step.prepared,note.history};
        }
        if(!preparation_) return {Status::invalidInput,{},{}};
        const auto progress=preparation_->samples.resume(allocator,installation,lifecycle,read,write);
        if(progress==MelodicSampleInstallation::Progress::failed) {
            failed_=true; return {Status::failed,{},{}};
        }
        if(progress==MelodicSampleInstallation::Progress::advanced) return {Status::preparing,{},{}};
        // Transfer ownership directly into DSP preparation/startup reservation.
        // Clearing the installation phase must not expose an external callback.
        const auto completed=*preparation_;
        preparation_.reset();
        return beginInstalledNote(completed.selection,*completed.samples.result(),completed.dsp,
            allocator,lifecycle,mask,parts,data,conversion,waves,read,write,PreparationSlice::partial);
    }

    // Shared post-install boundary for melodic and rhythm notes. Admission and
    // installation must already have committed exactly once. A busy startup
    // defers without consuming task flags; caller retains the installed note.
    // Other preparation failures latch, since earlier PCM writes may exist.
    template<class Read,class Write>
    MelodicStartResult beginInstalledNote(const PreparedNoteVelocity& selection,
        const InstalledPartialSamples& samples,const std::array<NormalPartialDspInputs,2>& dspInputs,
        VoiceAllocator& allocator,std::array<VoiceStopState,24>& lifecycle,VoiceKeyMask& mask,
        const PartControllerState& parts,const SoundData& data,const PitchConversion& conversion,
        const LfoWaveformTables& waves,Read&& read,Write&& write,PreparationSlice slice=PreparationSlice::note)
    {
        using Status = MelodicStartResult::Status;
        if (failed_) return {Status::failed,{},{}};
        if (startupPending() || mask.prepared != 0) return {Status::deferred,{},{}};
        const auto fail = [&]() -> MelodicStartResult { failed_ = true; return {Status::failed,{},{}}; };
        for(const auto& sample:samples) if(sample && !sample->installed && sample->slot<128) {
            const auto slot=sample->slot;
            if(slot>=24 || !(sample->sample.sampleId&0x8000)) return fail();
            bool overwritten=false;
            for(const auto& other:samples)
                overwritten |= other && other->installed && other->slot==slot;
            // Positive installation retains task2 metadata regardless of its
            // order relative to the return. A later positive install also
            // clears the free status; only a return-only slot must be free.
            if(overwritten) continue;
            if(!(allocator.allocations[slot].status&128)) return fail();
            if(lifecycle[slot].fieldCAF4) return fail();
            // 113e's return does not destroy the old synthesis owner. Keep
            // its stopped stages/caches from53e6 so the next control pass
            // cannot resurrect the old envelopes or ramp commands.
            if(voices[slot]) {
                voices[slot]->lifecycle=lifecycle[slot];
                voices[slot]->release.pending=0;
                first[slot].firstStage=second[slot].firstStage=lifecycle[slot].stages[0];
            }
        }
        bool anySample=false,onlyUnassigned=true;
        for(const auto& sample:samples) if(sample) {
            anySample=true;
            onlyUnassigned &= !sample->installed && (sample->slot>=128 || (sample->sample.sampleId&0x8000));
        }
        if(anySample && onlyUnassigned) {
            // 124e/12cf updated part history, but125a/12db found no PCM
            // destination. There is no task2 or key-on to manufacture.
            for(const auto& state:lifecycle) if(state.fieldCAF4) return fail();
            return {Status::preparedOnly,DispatchedNormalVoiceInputs{},PreparedNormalVoiceBatch{},
                CapturePartialPitchHistory(samples)};
        }
        auto requests = DispatchNormalVoiceInputs(selection,samples,dspInputs,
            lifecycle,allocator.pcmLinks,allocator.activity);
        if (!requests) return fail();
        if(slice==PreparationSlice::partial) {
            if(!beginNormalPreparation(std::span(requests->entries.data(),requests->count),mask,parts,data)) return fail();
            pendingDspNote_=DspNoteHandoff{*requests,CapturePartialPitchHistory(samples)};
            return {Status::preparing,{},{}};
        }
        auto prepared = prepareAndBeginNormalStart(std::span(requests->entries.data(),requests->count),
            lifecycle,mask,parts,data,conversion,waves,read,write);
        if (!prepared) return fail();
        return {Status::started,requests,prepared,CapturePartialPitchHistory(samples)};
    }

    // Native Note On's final handoff, after allocation, sample selection,
    // installation and task dispatch. Preparation and startup reservation must
    // be one serialized operation so a queued event cannot prepare twice while
    // waiting for PCM. Returned state is a preparation snapshot, not live DSP.
    // Busy/invalid slot preflight has no I/O. Once DSP preparation starts, a
    // failure latches the runtime: modulation or PCM may already have changed.
    bool beginNormalPreparation(
        std::span<const NormalVoicePreparationEntry> entries,
        VoiceKeyMask& mask,
        const PartControllerState& parts,const SoundData& data)
    {
        if (failed_ || startupPending() || mask.prepared != 0 || entries.empty() || entries.size() > 2)
            return false;
        std::array<uint8_t,2> slots{};
        std::array<NormalVoicePreparationEntry,2> ownedEntries{};
        for (unsigned i = 0; i < entries.size(); ++i)
        {
            if (entries[i].slot >= 24 || (i != 0 && entries[i].slot == entries[0].slot))
                return false;
            slots[i] = uint8_t(entries[i].slot);
            ownedEntries[i]=entries[i];
            if (!(entries[i].request.installed.flags&128)) {
                if (!voices[slots[i]]) return false;
                ownedEntries[i].continuing=&*voices[slots[i]];
                // The continuing branch skips second-envelope setup entirely.
                // Retain its derived limits/mode/timing from the live owner;
                // a caller's fresh-note defaults are not prepared parameters.
                auto& destination=ownedEntries[i].request.controls;
                const auto& live=inputs[slots[i]];
                destination.secondBypass=live.secondBypass;
                destination.secondTiming=live.secondTiming;
                destination.second=live.second;
                destination.secondBase=live.secondBase;
                destination.secondController=live.secondController;
                destination.secondLimit=live.secondLimit;
            }
        }
        dspPreparation_=NormalVoiceDspPreparation::begin(std::span(ownedEntries.data(),entries.size()),
            parts,first,second,secondSources,data);
        if(!dspPreparation_) { failed_=true; return false; }
        dspSlots_=slots;
        return true;
    }

    struct DspPreparationStep
    {
        NormalVoiceDspPreparation::Progress status;
        std::optional<PreparedNormalVoiceBatch> prepared;
    };
    template<class Read,class Write>
    DspPreparationStep resumeNormalPreparation(std::array<VoiceStopState,24>& lifecycle,VoiceKeyMask& mask,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write)
    {
        using Progress=NormalVoiceDspPreparation::Progress;
        if(failed_ || !dspPreparation_) return {Progress::failed,{}};
        const auto progress=dspPreparation_->resume(first,second,secondSources,data,conversion,waves,read,write);
        if(progress==Progress::failed) { failed_=true; return {progress,{}}; }
        if(progress!=Progress::complete) return {progress,{}};
        const auto prepared=*dspPreparation_->result();
        dspPreparation_.reset();
        if(!beginPreparedStart(std::span(dspSlots_.data(),prepared.count),prepared,lifecycle,mask)) {
            failed_=true; return {Progress::failed,{}};
        }
        return {Progress::complete,prepared};
    }
    template<class Read,class Write>
    std::optional<PreparedNormalVoiceBatch> prepareAndBeginNormalStart(
        std::span<const NormalVoicePreparationEntry> entries,
        std::array<VoiceStopState,24>& lifecycle,VoiceKeyMask& mask,
        const PartControllerState& parts,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        if(!beginNormalPreparation(entries,mask,parts,data)) return std::nullopt;
        for(unsigned phase=0;phase<3;++phase) {
            const auto step=resumeNormalPreparation(lifecycle,mask,data,conversion,waves,read,write);
            if(step.status==NormalVoiceDspPreparation::Progress::failed) return std::nullopt;
            if(step.prepared) return step.prepared;
        }
        failed_=true; return std::nullopt;
    }

    // Slots are in firmware dispatcher order, matching the prepared batch.
    // Retain a value copy: the caller's preparation storage may be reused.
    // Lifecycle/mask belong to the serialized engine and must remain the same
    // objects through polling. Do not run MIDI/install/DSP during this interval;
    // the scheduler must retain those events/ticks until completion. PCM alone
    // may advance. This does not model firmware interrupt/task-switch timing.
    bool beginPreparedStart(std::span<const uint8_t> slots,const PreparedNormalVoiceBatch& prepared,
        std::array<VoiceStopState,24>& lifecycle,VoiceKeyMask& mask)
    {
        if (failed_ || startupPending() || slots.empty() || slots.size() > 2
            || slots.size() != prepared.count) return false;
        std::array<PreparedVoiceBatch::Entry,2> entries{};
        for (unsigned i = 0; i < slots.size(); ++i)
        {
            if (slots[i] >= 24 || !prepared.voices[i]) return false;
            const auto& item = *prepared.voices[i];
            entries[i] = {slots[i],item.voice.prepared,item.post};
        }
        if (!startup_.begin(std::span(entries.data(),slots.size()),mask)) return false;
        pendingStart_ = prepared;
        for (unsigned i = 0; i < slots.size(); ++i)
        {
            startSlots_[i] = slots[i];
            lifecycle[slots[i]] = prepared.voices[i]->voice.lifecycle;
        }
        return true;
    }

    template<class Read,class Write>
    StartStatus pollPreparedStart(std::array<VoiceStopState,24>& lifecycle,VoiceKeyMask& mask,
        Read&& read,Write&& write)
    {
        if (failed_) return StartStatus::cancelled;
        if (preparationPending() || !startupPending()) return startup_.status();
        const auto status = startup_.advance(lifecycle,mask,read,write);
        if (status == StartStatus::cancelled) { failed_ = true; return status; }
        if (status != StartStatus::complete) return status;
        // Validate every continuation before publishing either DSP owner.
        for (unsigned i = 0; i < pendingStart_.count; ++i)
            if (!ContinueVoiceControlAfterStart(pendingStart_.voices[i]->voice,lifecycle[startSlots_[i]]))
            { failed_ = true; return StartStatus::cancelled; }
        for (unsigned i = 0; i < pendingStart_.count; ++i)
        {
            const auto slot = startSlots_[i]; const auto& item = *pendingStart_.voices[i];
            voices[slot] = item.voice; inputs[slot] = item.controls;
            firstInputs[slot] = item.firstControls;
        }
        pendingStart_ = {};
        return status;
    }

    // Publish the allocator's complete release snapshot at the caller's event
    // boundary, not on every DSP tick (which would reassert consumed requests).
    // Reject a request for a missing owner before changing any live voice.
    bool publishNoteReleases(const VoiceAllocator& allocator) noexcept
    {
        if (failed_ || startupPending()) return false;
        for (unsigned slot = 0; slot < 24; ++slot)
            if (allocator.allocations[slot].releaseCommand && !voices[slot]) return false;
        allocator.publishReleaseRequests([&](uint8_t slot,uint8_t request) {
            if (voices[slot]) voices[slot]->release.pending = request;
        });
        return true;
    }

    enum class ScheduledStatus { idle, deferred, working, updated, failed };
    enum class ControlSlice { pass, phase };
    struct ScheduledResult
    {
        ScheduledStatus status;
        uint8_t elapsed = 0;
        uint32_t updatedMask = 0; // Changes in this call, not earlier owners in a resumed pass.
    };

    // Consume the kernel event only when a DSP pass may actually run. The
    // engine advances this same clock whenever PCM device time advances,
    // including startup waits. At most one aggregated pass per call; do not
    // replay one pass per expiration (the firmware uses an 8-bit counter).
    // A wrapped counter of zero still represents a pending event. No event
    // is consumed during key-latch protection or after a latched failure.
    // A reuse wait protects its destinations, not all other voice groups.
    template<class Read,class Write>
    ScheduledResult serviceControl(ControlTaskClock& clock,const VoiceInstallationState& installed,
        const PartControllerState& parts,VoiceAllocator& allocator,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write,
        ControlSlice slice=ControlSlice::pass)
    {
        if (failed_) return {ScheduledStatus::failed};
        if (preparationPending() || startupAwaitingKeyLatch()) return {ScheduledStatus::deferred};
        // A caller stepping an explicit pass owns that continuation. The
        // scheduled entry must not steal it or consume another clock event.
        if(controlPending() && !scheduledPass_) return {ScheduledStatus::deferred};
        if (!controlPending()) {
            const auto elapsed = clock.consume();
            if (!elapsed) return {ScheduledStatus::idle};
            if (!beginControlPass(*elapsed)) return {ScheduledStatus::failed,*elapsed};
            scheduledPass_=true;
        }
        const auto elapsed=uint8_t(controlTicks_);
        uint32_t changed=0;
        for(unsigned group=0;group<=24;++group) {
            const auto result=slice==ControlSlice::phase
                ? resumeControlPhase(installed,parts,allocator,data,conversion,waves,read,write)
                : resumeControlPass(installed,parts,allocator,data,conversion,waves,read,write);
            changed|=result.changedMask;
            if(result.status==ControlProgress::complete) return {ScheduledStatus::updated,elapsed,changed};
            if(result.status==ControlProgress::deferred) return {ScheduledStatus::deferred,elapsed,changed};
            if(slice==ControlSlice::phase && (result.status==ControlProgress::advancedPhase
                || result.status==ControlProgress::updatedGroup)) return {ScheduledStatus::working,elapsed,changed};
            if(result.status!=ControlProgress::updatedGroup) return {ScheduledStatus::failed,elapsed,changed};
        }
        failed_=true;
        return {ScheduledStatus::failed,elapsed,changed};
    }

    // A control pass owns its elapsed count and traversal until completion.
    // Each resume updates one single/linked voice group, including both final
    // PCM publications. The caller may advance PCM between groups, but must
    // keep dependencies alive. MIDI/preparation/stop changes may run between
    // resumes on the same owner thread, never reentrantly inside a group.
    // No CPU addresses, instruction costs or invented delays are retained.
    bool beginControlPass(uint16_t ticks) noexcept
    {
        if (failed_ || preparationPending() || startup_.status()==StartStatus::waitingForKeyLatch || controlPending()) return false;
        lastResults.fill(std::nullopt); lastWrites.fill(std::nullopt);
        controlTicks_ = ticks; controlUpdated_ = 0;
        firstUpdatePending_=false;
        secondUpdate_={};
        pass_.reset(); controlPending_ = true; scheduledPass_ = false;
        return true;
    }

    enum class ControlProgress { idle, advancedPhase, updatedGroup, complete, failed, deferred };
    struct ControlStep
    {
        ControlProgress status;
        uint32_t updatedMask = 0; // Cumulative for this pass, including early exits.
        uint32_t changedMask = 0; // This resume only; prior owners may have been replaced.
    };

    // One serialized pass; no allocation, clock advancement or event parsing.
    // Failure can follow partial DSP/device mutation. Reconstruct/reprepare the
    // runtime after failure; repeated calls never replay an incomplete pass.
    template<class Read,class Write>
    std::optional<uint32_t> advance(uint16_t ticks,const VoiceInstallationState& installed,
        const PartControllerState& parts,VoiceAllocator& allocator,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        // The synchronous caller cannot yield to PCM/startup midway through.
        if (startupPending()) return std::nullopt;
        if (!beginControlPass(ticks)) return std::nullopt;
        for (unsigned step = 0; step <= 24; ++step)
        {
            const auto result = resumeControlPass(installed,parts,allocator,data,conversion,waves,read,write);
            if (result.status == ControlProgress::complete) return result.updatedMask;
            if (result.status != ControlProgress::updatedGroup) return std::nullopt;
        }
        failed_ = true;
        return std::nullopt;
    }

    template<class Read,class Write>
    ControlStep resumeControlPass(const VoiceInstallationState& installed,
        const PartControllerState& parts,VoiceAllocator& allocator,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        return resumeControlWork(false,installed,parts,allocator,data,conversion,waves,read,write);
    }

    PeriodicVoiceUpdatePass::Phase controlPhase() const noexcept { return pass_.phase(); }
    std::optional<uint8_t> controlVoice() const noexcept { return pass_.currentVoice(); }
    VoiceCalculationStage controlCalculationStage() const noexcept { return calculationStage_; }

    // Task1 consumes the completion mailbox before its MIDI receive branch.
    // Serialized with admission; no PCM operation or time advancement here.
    bool consumeVoiceCompletion(VoiceAllocator& allocator) noexcept
    {
        if (failed_) return false;
        // Do not interleave another task1 operation with an admitted note's
        // installation. Keep the mailbox until that operation hands off.
        if (preparationPending()) return true;
        if (pendingReturn_ >= 24) return true;
        if (!allocator.returnVoice(pendingReturn_)) { failed_ = true; return false; }
        pendingReturn_ = 24;
        return true;
    }

    // PCM may advance after any phase. Voice installation/reclamation and
    // MIDI voice commands remain serialized outside an unfinished group; the
    // normal product entry drains the same phases synchronously until a full
    // execution-time scheduler is available. No estimated wait is encoded here.
    template<class Read,class Write>
    ControlStep resumeControlPhase(const VoiceInstallationState& installed,
        const PartControllerState& parts,VoiceAllocator& allocator,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        return resumeControlWork(true,installed,parts,allocator,data,conversion,waves,read,write);
    }
private:
    template<class Read,class Write>
    ControlStep resumeControlWork(bool singlePhase,const VoiceInstallationState& installed,
        const PartControllerState& parts,VoiceAllocator& allocator,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        if (failed_) return {ControlProgress::failed,controlUpdated_};
        // Installation is a higher-priority operation, not a PCM reuse wait.
        // Retain the interrupted scan/ticks until both partials are installed.
        // Once installation hands off to reuse, other voice groups run normally.
        if (preparationPending()) return {ControlProgress::deferred,controlUpdated_};
        if (!controlPending()) return {ControlProgress::idle};
        // The completion notification is consumed before this low-priority
        // control pass resumes its scan. Keep the single mailbox owned here.
        if(pendingReturn_<24) {
            if(!consumeVoiceCompletion(allocator)) return {ControlProgress::failed,controlUpdated_};
            return {ControlProgress::updatedGroup,controlUpdated_,0};
        }
        if (startup_.status()==StartStatus::waitingForKeyLatch) return {ControlProgress::deferred,controlUpdated_};
        const auto fail = [&]() -> ControlStep {
            failed_ = true; return {ControlProgress::failed,controlUpdated_};
        };
        if (!data.modulationPreparation() || !data.modulationRates()) return fail();
        auto& links = allocator.pcmLinks;
        const auto& depths = data.modulationPreparation()->depths.pitch;
        const auto ticks = controlTicks_;
        uint32_t reserved=0,changed=0;
        if(startupPending()) for(unsigned i=0;i<pendingStart_.count;++i) reserved|=1u<<startSlots_[i];
        // Installation, boundary IRQ and release commands can change voice
        // owners between groups. The pass retains traversal, not stale stages
        // or preparation inputs from its beginning.
        std::array<uint16_t,24> stages;
        const bool selecting=pass_.phase()==PeriodicVoiceUpdatePass::Phase::select;
        const bool routing=pass_.phase()==PeriodicVoiceUpdatePass::Phase::firstModulation;
        for(unsigned slot=0;(selecting || routing) && slot<24;++slot) {
            // Prepared destinations waiting for old gain decay are not
            // periodic owners yet. H8 scans their preparation/stop stages
            // (18 or higher) and moves on, rather than waiting at that slot.
            stages[slot]=(reserved&(1u<<slot)) ? 18 : voices[slot] ? voices[slot]->lifecycle.stages[0] : 18;
        }
        if(reserved && pass_.phase()==PeriodicVoiceUpdatePass::Phase::select) {
            // 5710..573e yields while the old gain decays. Other groups may
            // run. Also protect a reserved destination reached indirectly
            // through a live partner's link; do not use its obsolete owner.
            auto visited=pass_.visited();
            const auto selected=SelectNextVoiceUpdate(pass_.cursor(),stages,visited,links);
            if(!selected) return fail();
            for(unsigned i=0;i<selected->count;++i)
                if(reserved&(1u<<selected->slots[i])) return {ControlProgress::deferred,controlUpdated_};
        }
        for(unsigned slot=0;(selecting || routing) && slot<24;++slot) if(!(reserved&(1u<<slot)))
            first[slot].firstStage=second[slot].firstStage=stages[slot];
        const auto refresh = [&](const VoiceUpdateSelection& selected) {
            for (unsigned i = 0; i < selected.count; ++i)
            {
                const auto slot = selected.slots[i];
                if (!voices[slot] || firstInputs[slot].depthControl > 127 || firstInputs[slot].rateControl > 127) return false;
            }
            if (!RefreshSelectedVoiceControllers(selected,installed,parts,controllers)) return false;
            for (unsigned i = 0; i < selected.count; ++i)
            {
                const auto slot = selected.slots[i]; auto& input = inputs[slot];
                input.ticks=ticks;
                controllers[slot].apply(input.level,input.second,input.pitch,first[slot].block,second[slot].block);
            }
            return true;
        };
        const auto firstUpdate = [&](unsigned slot) {
            const auto& input = firstInputs[slot];
            using Result = FirstVoiceModulationUpdate::Result;
            using Outcome = PeriodicVoiceUpdatePass::UpdateResult;
            if(!firstUpdatePending_) {
                const bool wasSharing=first[slot].sharing.sharing!=0;
                firstUpdate_={};
                const auto result=firstUpdate_.begin(slot,first,input.pitchDepth,input.depthControl,depths);
                if(result==Result::shared) return Outcome::proceed;
                if(result!=Result::ready) return Outcome::invalidInput;
                firstUpdatePending_=true;
                // Only detaching a shared source has the H8 lifetime-check
                // window. Preserve the task across it, not a stack temporary.
                if(wasSharing) return Outcome::continueCalculation;
            }
            firstUpdatePending_=false;
            const auto result=firstUpdate_.resume(first,ticks,input.pitchDepth,input.rateControl,input.depthControl,
                depths,*data.modulationRates(),waves,read,write);
            return result==Result::updated || result==Result::stageChanged ? Outcome::proceed : Outcome::invalidInput;
        };
        const auto pairedUpdate = [&](unsigned destination,unsigned source) {
            const auto& input = firstInputs[destination];
            return UpdatePairedFirstModulation(first[destination],first[source],input.pitchDepth,input.depthControl,depths);
        };
        const auto finishUpdate = [&](unsigned slot,VoiceControlResult result) {
            auto& voice = *voices[slot];
            lastResults[slot] = result;
            using Outcome = PeriodicVoiceUpdatePass::UpdateResult;
            if (result == VoiceControlResult::stopped || result == VoiceControlResult::finished)
            {
                const auto termination = result == VoiceControlResult::finished
                    ? (FinishEnvelopeTermination(slot,voice.lifecycle.stages[0],allocator.activity[slot],links,write)
                        ? std::optional{EnvelopeTermination::notifyAllocator} : std::nullopt)
                    : PollEnvelopeTermination(slot,voice.lifecycle.stages[0],allocator.activity[slot],links,read,write);
                if (!termination) return Outcome::invalidInput;
                // 33dc publishes a completion; 07d0 consumes the mailbox.
                // Do not free the voice inside the envelope calculation.
                if (*termination == EnvelopeTermination::notifyAllocator) {
                    if(pendingReturn_<24) return Outcome::invalidInput;
                    pendingReturn_=uint8_t(slot);
                }
            }
            stages[slot] = first[slot].firstStage = second[slot].firstStage = voice.lifecycle.stages[0];
            controlUpdated_ |= 1u<<slot;
            changed |= 1u<<slot;
            return result == VoiceControlResult::invalidInput ? Outcome::invalidInput
                : result != VoiceControlResult::updated ? Outcome::skipRemaining : Outcome::proceed;
        };
        const auto readback = [&](unsigned slot) {
            auto& voice=*voices[slot];
            const bool runsEnvelopeControl=voice.lifecycle.stages[0]<14;
            const auto entry=ReadVoiceControl(slot,voice,second,data,read,write);
            if(entry==VoiceControlReadback::invalidInput)
                return finishUpdate(slot,VoiceControlResult::invalidInput);
            // Activity becomes visible at readback, not at eventual output
            // publication. Release starts from this same held PCM amplitude.
            if(runsEnvelopeControl) allocator.activity[slot]=voice.release.activity;
            if(entry==VoiceControlReadback::stopped)
                return finishUpdate(slot,VoiceControlResult::stopped);
            calculationStage_=VoiceCalculationStage::modulation;
            secondUpdate_={};
            changed|=1u<<slot;
            return PeriodicVoiceUpdatePass::UpdateResult::proceed;
        };
        const auto update = [&](unsigned slot) {
            if(calculationStage_==VoiceCalculationStage::modulation && !secondUpdate_.pending()) {
                const auto source=secondSources[slot];
                if(source<24) second[source].firstStage=voices[source] ? voices[source]->lifecycle.stages[0] : 18;
            }
            const auto result=CalculateVoiceControlStage(calculationStage_,slot,*voices[slot],second,secondSources,
                first[slot].block,inputs[slot],data,conversion,waves,read,write,&secondUpdate_);
            if(result==VoiceControlResult::suspended)
                return PeriodicVoiceUpdatePass::UpdateResult::continueCalculation;
            if(result==VoiceControlResult::updated && calculationStage_!=VoiceCalculationStage::level) {
                calculationStage_=VoiceCalculationStage(unsigned(calculationStage_)+1);
                changed|=1u<<slot;
                return PeriodicVoiceUpdatePass::UpdateResult::continueCalculation;
            }
            return finishUpdate(slot,result);
        };
        const auto publish = [&](unsigned slot) {
            auto& voice = *voices[slot];
            const auto result = UpdateVoicePcm(slot,voice.lifecycle,voice.prepared,voice.output,voice.second,write);
            lastWrites[slot] = result;
            return result != VoicePcmUpdateResult::invalidChannel;
        };
        // Build the group context once in the normal product call, not once
        // per phase. The explicit phase entry uses exactly the same delegates.
        auto result=PeriodicVoiceUpdatePass::Result::advanced;
        // Selection, first routing/update (including detach), paired copy,
        // then two*(readback + five calculations + possible detach + publication).
        for(unsigned phase=0;phase<(singlePhase ? 1u : 20u);++phase) {
            result=pass_.stepPhase(stages,links,refresh,firstUpdate,pairedUpdate,readback,update,publish);
            if(result!=PeriodicVoiceUpdatePass::Result::advanced) break;
        }
        if(!singlePhase && result==PeriodicVoiceUpdatePass::Result::advanced) return fail();
        if(pendingReturn_<24) {
            if(singlePhase) return {ControlProgress::advancedPhase,controlUpdated_,changed};
            // Immediate callers drain the same notification before returning;
            // explicit phase callers observe completion and return separately.
            if(!consumeVoiceCompletion(allocator)) return fail();
        }
        if (result == PeriodicVoiceUpdatePass::Result::complete)
        {
            controlPending_ = false;
            return {ControlProgress::complete,controlUpdated_};
        }
        if (result == PeriodicVoiceUpdatePass::Result::invalidInput) return fail();
        if (result == PeriodicVoiceUpdatePass::Result::advanced)
            return {ControlProgress::advancedPhase,controlUpdated_,changed};
        return {ControlProgress::updatedGroup,controlUpdated_,changed};
    }
private:
    struct NotePreparation
    {
        MelodicSampleInstallation samples;
        PreparedNoteVelocity selection;
        std::array<NormalPartialDspInputs,2> dsp;
    };
    std::optional<NotePreparation> preparation_;
    struct DspNoteHandoff
    {
        DispatchedNormalVoiceInputs requests;
        PartialPitchHistoryUpdate history;
    };
    std::optional<DspNoteHandoff> pendingDspNote_;
    std::optional<NormalVoiceDspPreparation> dspPreparation_;
    std::array<uint8_t,2> dspSlots_{};
    PreparedVoiceBatch startup_;
    PreparedNormalVoiceBatch pendingStart_{};
    std::array<uint8_t,2> startSlots_{};
    PeriodicVoiceUpdatePass pass_;
    VoiceCalculationStage calculationStage_=VoiceCalculationStage::modulation;
    FirstVoiceModulationUpdate firstUpdate_;
    VoiceModulationUpdate secondUpdate_;
    uint8_t pendingReturn_=24;
    bool firstUpdatePending_=false;
    uint16_t controlTicks_ = 0;
    uint32_t controlUpdated_ = 0;
    bool controlPending_ = false;
    bool scheduledPass_ = false;
    bool failed_ = false;
};
}
