#pragma once
#include "sc55_voice_runtime.h"
#include "sc55_note_fanout.h"

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
    bool failed() const noexcept { return runtime.failed() || noteOn.status() == NoteOnFanout::Status::failed; }

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
            if (probe[slot].fieldCAF4 == 4)
            {
                if (!runtime.voices[slot]) return StopRequest::failed;
                if (lifecycle[slot].fieldCAF4 != 0) return StopRequest::deferred;
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
        if (runtime.startupPending() || lifecycle[slot].fieldCAF4 != 0) return StopRequest::deferred;
        // The running DSP state, not an obsolete installation snapshot, is
        // authoritative for cached words/progress before a physical stop.
        auto stopped = runtime.voices[slot]->lifecycle;
        if (!StopPreparedVoice(slot,stopped,read,write)) return StopRequest::invalidInput;
        stopped.fieldCAF4 = 4;
        lifecycle[slot] = stopped;
        return StopRequest::queued;
    }

    VoiceControlRuntime::StopTaskResult serviceStopTask()
    { return failed() ? VoiceControlRuntime::StopTaskResult{VoiceControlRuntime::StopTaskStatus::failed}
                      : runtime.serviceStopTask(lifecycle,notes.allocator); }

    template<class Read,class Write>
    VoiceControlRuntime::StartStatus pollStart(Read&& read,Write&& write)
    { return failed() ? VoiceControlRuntime::StartStatus::cancelled : runtime.pollPreparedStart(lifecycle,mask,read,write); }

    template<class Read,class Write>
    VoiceControlRuntime::ScheduledResult serviceControl(const PartControllerState& parts,
        const SoundData& data,const PitchConversion& conversion,const LfoWaveformTables& waves,
        Read&& read,Write&& write)
    {
        using Status = VoiceControlRuntime::ScheduledStatus;
        if (failed()) return {Status::failed};
        // Do not run stale DSP over a queued physical stop/preparation. Preserve
        // the clock event until the owner services its pending tasks. This is
        // an intermediate serialization boundary, not H8 task-latency fidelity.
        for (const auto& voice : lifecycle)
            if (voice.fieldCAF4 != 0) return {Status::deferred};
        const auto result = runtime.serviceControl(clock,installation,parts,notes.allocator,
            data,conversion,waves,read,write);
        if (result.status == Status::updated)
            for (unsigned slot = 0; slot < 24; ++slot)
                if ((result.updatedMask&(1u<<slot)) && runtime.voices[slot])
                    lifecycle[slot] = runtime.voices[slot]->lifecycle;
        return result;
    }
};
}
