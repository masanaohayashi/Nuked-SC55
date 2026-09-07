#pragma once
#include "sc55_voice_engine.h"
#include "pcm.h"

namespace sc55
{
// Experimental capital-bank melodic player. Owns the serialized MIDI/control
// loop, not SoundData or PCM. Those objects must outlive it. Construction is
// setup-time; push/service/step neither allocate nor execute H8 instructions.
// Deliberately not a GS reset implementation: drums, bank variations, SysEx,
// mono/portamento and effects setup remain unsupported and are counted.
class NativeMelodicPlayer
{
public:
    NativeMelodicPlayer(const SoundData& data, pcm_t& pcm) : data_(data), pcm_(pcm)
    {
        if (!data.pitchTiming() || data.patchCount() < 224
            || !engine_.notes.allocator.initializeTables())
        { failed_ = true; return; }
        scale_.fill(64);
        for (unsigned part = 0; part < 16; ++part)
        {
            routing_[part] = {uint8_t(part),0x6ea2,0x80};
            auto& controls = controllers_.parts[part];
            controls.sensitivity = {64,64,64,64,0,0,0,64,0,0,0};
            controls.sourceSensitivity.fill(controls.sensitivity);
            controls.sourceSensitivity[1][0] = 66; // Preview bend range: two semitones.
            controls.assignedControllers = {16,17};
        }
        for (unsigned slot = 0; slot < 24; ++slot)
            engine_.runtime.first[slot].firstStage = engine_.runtime.second[slot].firstStage = 22;
        write(0x3c,0); // Dry, undithered preview; no fabricated effects RAM.
        write(0x3d,0xb7); // 24 PCM slots, mk1 wave-bank addressing.
    }

    bool failed() const noexcept { return failed_ || engine_.failed() || queue_.failed(); }
    uint64_t unsupportedEvents() const noexcept { return unsupported_; }
    unsigned freeVoices() const noexcept { return engine_.notes.allocator.freeCount; }
    std::size_t queuedEvents() const noexcept { return queue_.size() + (pending_ ? 1 : 0); }
    std::size_t push(std::span<const uint8_t> bytes) noexcept
    {
        if (failed()) return 0;
        return queue_.push(bytes,pcm_.cycles).consumed;
    }

    // One bounded slice; never run past a control deadline. The caller splits
    // host buffers at MIDI offsets and keeps fractional resampling state.
    void step() noexcept
    {
        service();
        const auto duration = std::min<uint32_t>(625,engine_.clock.untilNextExpiration());
        PCM_Update(pcm_,pcm_.cycles + duration);
        engine_.clock.advance(duration);
        service();
    }

private:
    uint8_t read(uint8_t address) noexcept { return PCM_Read(pcm_,address); }
    void write(uint8_t address,uint8_t value) noexcept { PCM_Write(pcm_,address,value); }
    bool tasksPending() const noexcept
    {
        for (const auto& state : engine_.lifecycle)
            if (state.fieldCAF4 != 0) return true;
        return false;
    }
    void refreshChannel(unsigned channel) noexcept
    {
        for (unsigned slot = 0; slot < 24; ++slot)
            if (engine_.runtime.voices[slot]
                && engine_.installation.voices[slot].input.part == channel)
            {
                auto& input = engine_.runtime.inputs[slot];
                ApplyChannelOutputControls(channels_.channel(channel),input.level,input.spatial);
                input.spatial.reverb = input.spatial.chorus = 0;
            }
    }
    MidiDispatchResult receive(const MidiDecoder::Event& event) noexcept
    {
        if (event.kind == MidiDecoder::Kind::realtime) return MidiDispatchResult::accepted;
        if (event.kind != MidiDecoder::Kind::message)
        {
            if (event.kind == MidiDecoder::Kind::sysexBegin) ++unsupported_;
            return MidiDispatchResult::accepted;
        }
        const auto kind = event.status & 0xf0, channel = event.status & 15;
        if (kind == 0x90 && event.second != 0)
        {
            if (channel == 9 || bank_[channel] != 0) ++unsupported_;
            else pending_ = event;
            return MidiDispatchResult::accepted;
        }
        if (kind == 0x80 || (kind == 0x90 && event.second == 0)
            || (kind == 0xb0 && (event.first == 64 || event.first == 66)))
        {
            const auto result = engine_.receiveReleaseMidi(event,routing_,{});
            using Status = NativeVoiceEngine::ReleaseMidiResult::Status;
            return result.status == Status::accepted ? MidiDispatchResult::accepted
                : result.status == Status::deferred ? MidiDispatchResult::deferred : MidiDispatchResult::failed;
        }
        if (kind == 0xb0 && event.first == 0)
        { bank_[channel] = event.second; return MidiDispatchResult::accepted; }
        if (kind == 0xb0 && (event.first == 120 || event.first == 123))
        {
            // Preview panic is a bounded hard stop. CC123 hold semantics await
            // the GS channel-mode implementation; do not claim equivalence.
            const auto load = [&](uint8_t a) { return read(a); };
            const auto store = [&](uint8_t a,uint8_t v) { write(a,v); };
            for (unsigned slot = 0; slot < 24; ++slot)
                if (engine_.runtime.voices[slot]
                    && !(engine_.notes.allocator.status[slot] & 0x80)
                    && engine_.installation.voices[slot].input.part == channel)
                    if (engine_.requestStop(slot,load,store) == NativeVoiceEngine::StopRequest::failed)
                        return MidiDispatchResult::failed;
            return MidiDispatchResult::accepted;
        }
        const bool scalar = channels_.apply(event);
        const bool contribution = controllers_.receiveControlContributions(event,routing_);
        const bool pressure = controllers_.receivePolyPressure(event,routing_)
            || controllers_.receiveChannelPressure(event,routing_)
            || controllers_.receivePitchBend(event,routing_);
        if (scalar) refreshChannel(unsigned(channel));
        const bool mappedContribution = contribution
            && (event.first == 1 || event.first == 16 || event.first == 17);
        if (!scalar && !mappedContribution && !pressure) ++unsupported_;
        return MidiDispatchResult::accepted;
    }

    void startPending() noexcept
    {
        if (!pending_ || engine_.runtime.startupPending() || tasksPending()) return;
        const auto event = *pending_;
        const auto part = uint8_t(event.status & 15);
        const auto& channel = channels_.channel(part);
        const auto load = [&](uint8_t a) { return read(a); };
        const auto store = [&](uint8_t a,uint8_t v) { write(a,v); };
        const MelodicAllocationInputs allocation{bank_[part],false,0,part,0x80,1};
        auto probe = engine_.notes.allocator;
        const auto selected = AllocateMelodicNote(event,channel,allocation,data_,probe);
        using Allocated = MelodicAllocationResult::Status;
        if (selected.status == Allocated::needsCapacity)
        {
            // The MIDI event is already owned by pending_; a reclaim is never
            // replayed as a queue callback returning deferred.
            auto life = engine_.lifecycle;
            for (unsigned slot = 0; slot < 24; ++slot)
                if (engine_.runtime.voices[slot]) life[slot] = engine_.runtime.voices[slot]->lifecycle;
            const auto count = selected.selection->partials.candidates.count;
            const auto capacity = EnsureVoiceCapacity(engine_.notes.allocator,life,part,count,{},load,store);
            engine_.lifecycle = life;
            if (!capacity) failed_ = true;
            else if (!*capacity) { ++unsupported_; pending_.reset(); }
            return;
        }
        if (selected.status == Allocated::keyRangeRejected || selected.status == Allocated::velocityRejected)
        { pending_.reset(); return; }
        if (selected.status != Allocated::allocated) { ++unsupported_; pending_.reset(); return; }
        const auto key = uint8_t(std::clamp(int(event.first) + channel.coarseTuning,0,127));
        // A special descriptor requires another synthesis path. Reject it
        // before committing allocation/PCM, rather than poisoning the player.
        const auto& patch = *data_.patch(selected.selection->tone);
        for (unsigned partial = 0; partial < 2; ++partial)
            if (selected.dispatch[partial].prepare)
            {
                const auto plan = PreparePartialSample(patch.partial[partial],*data_.samples(),
                    key,key,event.first,scale_,255,0,event.first);
                if (!plan || (plan->sampleId & 0x8000))
                { ++unsupported_; pending_.reset(); return; }
            }
        const PartialSampleInstallInputs sample{scale_,key,key,event.first,255,0,event.first,0,160};
        VoiceControlInputs controls;
        controls.level.master = 100;
        ApplyChannelOutputControls(channel,controls.level,controls.spatial);
        controls.spatial.reverb = controls.spatial.chorus = 0;
        std::array<NormalPartialDspInputs,2> dsp{};
        for (unsigned partial = 0; partial < 2; ++partial)
        {
            dsp[partial] = {key,false,0,controls,{},{}};
            const auto slot = selected.dispatch[partial].voice;
            if (slot < 24)
            {
                dsp[partial].previousPitch = previousPitch_[slot];
                if (engine_.runtime.voices[slot])
                    dsp[partial].previousPitch.glide = engine_.runtime.voices[slot]->pitch.glide;
            }
        }
        const auto result = engine_.startRoutedMelodicNote(event,channel,allocation,{sample,sample},dsp,
            controllers_,data_,conversion_,waves_,load,store);
        using Start = VoiceControlRuntime::MelodicStartResult::Status;
        if (result.status == Start::deferred || result.status == Start::needsCapacity) return;
        pending_.reset();
        if (result.status != Start::started) { failed_ = true; return; }
        for (unsigned i = 0; i < result.requests->count; ++i)
            previousPitch_[result.requests->entries[i].slot] = result.prepared->voices[i]->partPitch;
    }

    void service() noexcept
    {
        if (failed()) return;
        const auto load = [&](uint8_t a) { return read(a); };
        const auto store = [&](uint8_t a,uint8_t v) { write(a,v); };
        if (engine_.runtime.startupPending()) engine_.pollStart(load,store);
        for (unsigned task = 0; task < 24 && !engine_.runtime.startupPending(); ++task)
        {
            const auto result = engine_.serviceStopTask();
            if (result.status != VoiceControlRuntime::StopTaskStatus::completed) break;
        }
        const auto control = engine_.serviceControl(controllers_,data_,conversion_,waves_,load,store);
        if (control.status == VoiceControlRuntime::ScheduledStatus::failed) { failed_ = true; return; }
        for (unsigned event = 0; event < 64 && !failed(); ++event)
        {
            startPending();
            if (pending_ || tasksPending() || engine_.runtime.startupPending()) break;
            const auto result = engine_.serviceMidi(queue_,pcm_.cycles,
                [&](const auto& message) { return receive(message); });
            if (result != MidiDispatchResult::accepted) break;
        }
    }
    const SoundData& data_;
    pcm_t& pcm_;
    NativeVoiceEngine engine_;
    MidiEventQueue<2048> queue_;
    std::optional<MidiDecoder::Event> pending_;
    ChannelControls channels_;
    PartControllerState controllers_;
    std::array<PartMidiReceive,16> routing_{};
    std::array<uint8_t,16> bank_{};
    std::array<uint8_t,12> scale_{};
    std::array<PreparedPartPitch,24> previousPitch_{};
    PitchConversion conversion_;
    LfoWaveformTables waves_;
    bool failed_ = false;
    uint64_t unsupported_ = 0;
};
}
