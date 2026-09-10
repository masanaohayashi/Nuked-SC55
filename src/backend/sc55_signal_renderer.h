#pragma once
#include "pcm_sim.h"
#include "pcm_effects.h"
#include "sc55_audio_buses.h"
#include "audio_frame.h"

namespace sc55
{
// Audio-owner signal generator: no firmware, chip registers, ROM loader or
// host callbacks. Waveform windows and settings are prepared by its owner.
// The existing pipeline emits the previous mix, then processes its effect
// sends and computes the next mix. Preserve that one-frame relationship.
class SignalRenderer
{
public:
    struct Frame { AudioBuses output; int boundaryVoice=-1; };

    PCMSimVoices voices{};
    PCMEffects effects{};

    // Setup/legacy import only. Normal frame generation owns this progression,
    // including the initial non-updating pass and effect-key latch.
    void adoptFrameState(bool effectsActive,bool update) noexcept
    { effectsActive_=effectsActive; update_=update; }
    void enableEffects() noexcept { effectsActive_=true; }
    bool updatesEnvelopes() const noexcept { return update_; }
    bool lastFrameUpdated() const noexcept { return lastUpdate_; }
    bool effectsEnabled() const noexcept { return effectsActive_; }
    AudioFrame<int32_t> nextFrame() noexcept
    {
        const auto output=render(effectsActive_,update_).output;
        lastUpdate_=update_;
        if(update_) effectsActive_=true;
        update_=true;
        return {int32_t(uint32_t(output.left)<<12),int32_t(uint32_t(output.right)<<12)};
    }

    void setPitchSource(unsigned source,uint16_t increment) noexcept
    {
        if(source>=pitch_.size()) return;
        pitch_[source]=increment;
        for(unsigned slot=0;slot<24;++slot)
            if(pitchSource_[slot]==source) voices.phase_step[slot]=increment;
        if(chorusPitchSource_==source) effects.chorus.increment=increment;
    }
    void setVoicePitchSource(unsigned slot,unsigned source) noexcept
    {
        if(slot>=24 || source>=pitch_.size()) return;
        pitchSource_[slot]=uint8_t(source);
        voices.phase_step[slot]=pitch_[source];
    }
    void setChorusPitchSource(unsigned source) noexcept
    {
        if(source>=pitch_.size()) return;
        chorusPitchSource_=uint8_t(source);
        effects.chorus.increment=pitch_[source];
    }
    void updateVoice(unsigned slot,const VoiceRenderUpdate& update) noexcept
    {
        if(slot>=24) return;
        PCMSim_ApplyVoiceUpdate(voices,slot,update);
        setPitchSource(slot,update.phaseIncrement);
        voices.phase_step[slot]=pitch_[pitchSource_[slot]];
    }
    // Install geometry using the oscillator/EG histories already owned here.
    // Key enable remains separate so paired voices begin together. The previous
    // active pass retains phase/direction; an inactive pass exposes zero phase.
    void installVoice(unsigned slot,const PCMSimWaveform& waveform,uint32_t address,
        unsigned pitchSource) noexcept
    {
        if(slot>=24) return;
        const bool active=voices.gate[slot]!=0;
        const uint16_t phase=active ? uint16_t(voices.sub_phase[slot]&0x3fff) : 0;
        const bool reverse=active && voices.reverse_mask[slot]!=0;
        if(!active) voices.boundaryLatched&=~(1u<<slot);
        PCMSim_SetWaveform(voices,slot,waveform);
        setVoicePitchSource(slot,pitchSource);
        prepareVoiceStart(slot,address,phase,reverse);
    }

    // Setup-time clock adoption. MIDI reset must not restart this shared audio
    // clock or random sequence; it continues across note and effect changes.
    void initializeClock(uint16_t phase,uint16_t random,uint64_t frames=0) noexcept
    { phase_=uint16_t(phase&0x3fff); random_=random; frames_=frames; }
    uint16_t randomWord() const noexcept { return random_; }
    uint64_t renderedFrames() const noexcept { return frames_; }
    uint16_t envelopePhase() const noexcept { return phase_; }
    void setVoiceKeys(uint32_t keys) noexcept { enabled_=keys&0x00ffffffu; }
    uint32_t voiceKeys() const noexcept { return enabled_; }
    void prepareVoiceStart(unsigned slot,uint32_t address,uint16_t phase,bool reverse) noexcept
    {
        if(slot>=24) return;
        starts_[slot]={address,phase,reverse};
        latched_&=~(1u<<slot);
        initialized_&=~(1u<<slot);
    }
    // Compatibility import only: normal note control uses prepareVoiceStart
    // and setVoiceKeys. Importing a dirty control must retain actual gate state.
    void adoptVoiceGate(unsigned slot,bool enabled,bool latched,uint32_t address,uint16_t phase,bool reverse) noexcept
    {
        if(slot>=24) return;
        const auto bit=1u<<slot;
        enabled_=(enabled_&~bit)|(enabled?bit:0);
        latched_=(latched_&~bit)|(latched?bit:0);
        if(!latched) initialized_&=~bit;
        starts_[slot]={address,phase,reverse};
    }
    bool voiceReady(unsigned slot) const noexcept
    { return slot<24 && (enabled_&initialized_&(1u<<slot)); }

    // Advancing the shared clock is part of generating a frame, never a
    // separate caller obligation. Rendering voices/FX cannot forget or double
    // advance the clock, and MIDI/control reads cannot advance the random word.
    Frame render(bool effectsActive,bool update=true,bool canNotify=true) noexcept
    {
        const unsigned bit=((random_>>0)^(random_>>1)^(random_>>7)^(random_>>12))&1;
        random_=uint16_t((random_>>1)|(bit<<15));
        phase_=uint16_t((phase_-1)&0x3fff);
        ++frames_;
        const EnvelopeClock clock{phase_,update};
        for(unsigned slot=0;slot<24;++slot) {
            const auto bit=1u<<slot;
            const bool key=(enabled_&bit)!=0,latched=(latched_&bit)!=0;
            voices.gate[slot]=key&&latched ? 1.0f : 0.0f;
            if(key&&!latched) {
                const auto& start=starts_[slot];
                PCMSim_RestartVoice(voices,slot,start.address,start.phase,start.reverse);
            }
        }
        Frame result{pending_,-1};
        float effectDry[6]{},effectSends[6]{},voiceBuses[4]{};
        effects.process(clock,effectsActive,float(pending_.reverb),float(pending_.chorus),effectDry,effectSends);
        PCMSim_RenderFrame(voices,clock,voiceBuses);
        // A latched key alone is not ready: its first active envelope pass
        // must finish before the controller may replace startup commands.
        if(update) { initialized_|=enabled_&latched_; latched_|=enabled_; }
        result.boundaryVoice=PCMSim_CollectBoundary(voices,clock.update,canNotify && pendingBoundary_<0);
        if(result.boundaryVoice>=0) pendingBoundary_=result.boundaryVoice;
        int dry[6],sends[6];
        for(unsigned i=0;i<6;++i) {
            dry[i]=int(std::lrint(effectDry[i]));
            sends[i]=int(std::lrint(effectSends[i]));
        }
        pending_=MixVoiceAndEffectBuses(voiceBuses,dry,sends);
        return result;
    }

    const AudioBuses& pendingMix() const noexcept { return pending_; }
    bool hasVoiceBoundary() const noexcept { return pendingBoundary_>=0; }
    int takeVoiceBoundary() noexcept
    {
        const int voice=pendingBoundary_;
        pendingBoundary_=-1;
        return voice;
    }

private:
    bool effectsActive_=false,update_=true,lastUpdate_=false;
    std::array<uint16_t,32> pitch_{};
    std::array<uint8_t,24> pitchSource_{};
    uint8_t chorusPitchSource_=0;
    AudioBuses pending_{};
    uint16_t phase_=0,random_=0xffff;
    uint64_t frames_=0;
    struct Start { uint32_t address=0; uint16_t phase=0; bool reverse=false; };
    std::array<Start,24> starts_{};
    uint32_t enabled_=0,latched_=0,initialized_=0;
    // One outstanding event preserves the existing arbitration: other voices
    // remain eligible until the controller acknowledges this boundary.
    int pendingBoundary_=-1;
};
}
