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
    bool startupPending() const noexcept
    {
        return startup_.status() == StartStatus::waitingForReuse
            || startup_.status() == StartStatus::waitingForKeyLatch;
    }

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
        enum class Status { invalidInput, deferred, needsCapacity, keyRangeRejected, velocityRejected, started, failed };
        Status status;
        std::optional<DispatchedNormalVoiceInputs> requests;
        std::optional<PreparedNormalVoiceBatch> prepared;
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
        const auto fail = [&]() -> MelodicStartResult { failed_ = true; return {Status::failed,{},{}}; };
        const auto samples = PrepareAndInstallMelodicSamples(allocation,allocationInput.part,sampleInputs,
            data,allocator,installation,lifecycle,read,write);
        if (!samples) return fail();
        return beginInstalledNote(*allocation.selection,*samples,dspInputs,allocator,lifecycle,mask,
            parts,data,conversion,waves,read,write);
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
        const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        using Status = MelodicStartResult::Status;
        if (failed_) return {Status::failed,{},{}};
        if (startupPending() || mask.prepared != 0) return {Status::deferred,{},{}};
        const auto fail = [&]() -> MelodicStartResult { failed_ = true; return {Status::failed,{},{}}; };
        auto requests = DispatchNormalVoiceInputs(selection,samples,dspInputs,
            lifecycle,allocator.pcmLinks,allocator.activity);
        if (!requests) return fail();
        auto prepared = prepareAndBeginNormalStart(std::span(requests->entries.data(),requests->count),
            lifecycle,mask,parts,data,conversion,waves,read,write);
        if (!prepared) return fail();
        return {Status::started,requests,prepared};
    }

    // Native Note On's final handoff, after allocation, sample selection,
    // installation and task dispatch. Preparation and startup reservation must
    // be one serialized operation so a queued event cannot prepare twice while
    // waiting for PCM. Returned state is a preparation snapshot, not live DSP.
    // Busy/invalid slot preflight has no I/O. Once DSP preparation starts, a
    // failure latches the runtime: modulation or PCM may already have changed.
    template<class Read,class Write>
    std::optional<PreparedNormalVoiceBatch> prepareAndBeginNormalStart(
        std::span<const NormalVoicePreparationEntry> entries,
        std::array<VoiceStopState,24>& lifecycle,VoiceKeyMask& mask,
        const PartControllerState& parts,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        if (failed_ || startupPending() || mask.prepared != 0 || entries.empty() || entries.size() > 2)
            return std::nullopt;
        std::array<uint8_t,2> slots{};
        for (unsigned i = 0; i < entries.size(); ++i)
        {
            if (entries[i].slot >= 24 || (i != 0 && entries[i].slot == entries[0].slot))
                return std::nullopt;
            slots[i] = uint8_t(entries[i].slot);
        }
        auto prepared = PrepareNormalVoicesDsp(entries,parts,first,second,secondSources,
            data,conversion,waves,read,write);
        if (!prepared || !beginPreparedStart(std::span(slots.data(),entries.size()),*prepared,lifecycle,mask))
        { failed_ = true; return std::nullopt; }
        return prepared;
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
        if (!startupPending()) return startup_.status();
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
            if (allocator.fieldA3E0[slot] && !voices[slot]) return false;
        allocator.publishReleaseRequests([&](uint8_t slot,uint8_t request) {
            if (voices[slot]) voices[slot]->release.pending = request;
        });
        return true;
    }

    enum class ScheduledStatus { idle, deferred, updated, failed };
    struct ScheduledResult
    {
        ScheduledStatus status;
        uint8_t elapsed = 0;
        uint32_t updatedMask = 0;
    };

    // Consume the kernel event only when a DSP pass may actually run. The
    // engine advances this same clock whenever PCM device time advances,
    // including startup waits. At most one aggregated pass per call; do not
    // replay one pass per expiration (the firmware uses an 8-bit counter).
    // A wrapped counter of zero still represents a pending event. No event
    // is consumed while startup is pending or after a latched failure.
    template<class Read,class Write>
    ScheduledResult serviceControl(ControlTaskClock& clock,const VoiceInstallationState& installed,
        const PartControllerState& parts,VoiceAllocator& allocator,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        if (failed_) return {ScheduledStatus::failed};
        if (startupPending()) return {ScheduledStatus::deferred};
        const auto elapsed = clock.consume();
        if (!elapsed) return {ScheduledStatus::idle};
        const auto updated = advance(*elapsed,installed,parts,allocator,data,conversion,waves,read,write);
        if (!updated) return {ScheduledStatus::failed,*elapsed};
        return {ScheduledStatus::updated,*elapsed,*updated};
    }

    // One serialized pass; no allocation, clock advancement or event parsing.
    // Failure can follow partial DSP/device mutation. Reconstruct/reprepare the
    // runtime after failure; repeated calls never replay an incomplete pass.
    template<class Read,class Write>
    std::optional<uint32_t> advance(uint16_t ticks,const VoiceInstallationState& installed,
        const PartControllerState& parts,VoiceAllocator& allocator,const SoundData& data,
        const PitchConversion& conversion,const LfoWaveformTables& waves,Read&& read,Write&& write)
    {
        if (failed_ || startupPending()) return std::nullopt;
        const auto fail = [&]() -> std::optional<uint32_t> { failed_ = true; return std::nullopt; };
        if (!data.modulationPreparation() || !data.modulationRates()) return fail();
        auto& links = allocator.pcmLinks;
        const auto& depths = data.modulationPreparation()->depths.pitch;
        std::array<uint16_t,24> stages;
        for (unsigned slot = 0; slot < 24; ++slot)
        {
            stages[slot] = voices[slot] ? voices[slot]->lifecycle.stages[0] : 18;
            first[slot].firstStage = second[slot].firstStage = stages[slot];
            inputs[slot].ticks = ticks;
        }
        lastResults.fill(std::nullopt); lastWrites.fill(std::nullopt);
        uint32_t updated = 0;
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
                controllers[slot].apply(input.level,input.second,input.pitch,first[slot].block,second[slot].block);
            }
            return true;
        };
        const auto firstUpdate = [&](unsigned slot) {
            const auto& input = firstInputs[slot];
            FirstVoiceModulationUpdate task;
            using Result = FirstVoiceModulationUpdate::Result;
            auto result = task.begin(slot,first,input.pitchDepth,input.depthControl,depths);
            if (result == Result::ready)
                result = task.resume(first,ticks,input.pitchDepth,input.rateControl,input.depthControl,
                    depths,*data.modulationRates(),waves,read,write);
            return result == Result::updated || result == Result::shared;
        };
        const auto pairedUpdate = [&](unsigned destination,unsigned source) {
            const auto& input = firstInputs[destination];
            return UpdatePairedFirstModulation(first[destination],first[source],input.pitchDepth,input.depthControl,depths);
        };
        const auto update = [&](unsigned slot) {
            auto& voice = *voices[slot];
            const auto result = AdvanceVoiceControl(slot,voice,second,secondSources,first[slot].block,
                inputs[slot],data,conversion,waves,read,write);
            lastResults[slot] = result;
            using Outcome = PeriodicVoiceUpdatePass::UpdateResult;
            if (result == VoiceControlResult::stopped || result == VoiceControlResult::finished)
            {
                const auto termination = result == VoiceControlResult::finished
                    ? (FinishEnvelopeTermination(slot,voice.lifecycle.stages[0],allocator.activity[slot],links,write)
                        ? std::optional{EnvelopeTermination::notifyAllocator} : std::nullopt)
                    : PollEnvelopeTermination(slot,voice.lifecycle.stages[0],allocator.activity[slot],links,read,write);
                if (!termination) return Outcome::invalidInput;
                // Serialize the33dc notification and07d0..07e6 consumer here.
                // This preserves bookkeeping, not firmware task-switch latency.
                if (*termination == EnvelopeTermination::notifyAllocator && !allocator.returnVoice(slot))
                    return Outcome::invalidInput;
            }
            stages[slot] = first[slot].firstStage = second[slot].firstStage = voice.lifecycle.stages[0];
            updated |= 1u<<slot;
            return result == VoiceControlResult::invalidInput ? Outcome::invalidInput
                : result != VoiceControlResult::updated ? Outcome::skipRemaining : Outcome::proceed;
        };
        const auto publish = [&](unsigned slot) {
            auto& voice = *voices[slot];
            const auto result = UpdateVoicePcm(slot,voice.lifecycle,voice.prepared,voice.output,voice.second,write);
            lastWrites[slot] = result;
            return result != VoicePcmUpdateResult::invalidChannel;
        };
        pass_.reset();
        for (unsigned step = 0; step <= 24; ++step)
        {
            const auto result = pass_.step(stages,links,refresh,firstUpdate,pairedUpdate,update,publish);
            if (result == PeriodicVoiceUpdatePass::Result::complete) return updated;
            if (result == PeriodicVoiceUpdatePass::Result::invalidInput) return fail();
        }
        return fail();
    }
private:
    PreparedVoiceBatch startup_;
    PreparedNormalVoiceBatch pendingStart_{};
    std::array<uint8_t,2> startSlots_{};
    PeriodicVoiceUpdatePass pass_;
    bool failed_ = false;
};
}
