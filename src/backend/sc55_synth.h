#pragma once
#include "sc55_native_player.h"
#include "sc55_sound_data_import.h"
#include "sc55_synth_state.h"
#include "sc55_synth_command.h"
#include "sc55_tone_generator.h"
#include <memory>
#include <stdexcept>
#include <algorithm>

namespace sc55
{
// Native v1.21 sound generator. No Emulator, MCU, peripheral timers or LCD.
// Construct off the audio thread; MIDI and render belong to one audio thread.
// The host splits render spans at MIDI timestamps. Control time is continuous
// across those spans and belongs to the synth, never to individual voices.
class NativeSynth
{
public:
    NativeSynth(std::span<const uint8_t> encoded,
        const std::vector<uint8_t>& rom1,const std::vector<uint8_t>& rom2,
        std::span<const uint8_t> wave1,std::span<const uint8_t> wave2,
        std::span<const uint8_t> wave3,unsigned maximumVoices=24)
    {
        if(!CanImportSoundData(rom1,rom2) || !data_.loadEncoded(encoded))
            throw std::runtime_error("Native synth requires v1.21 sound data");
        for(const auto wave:{wave1,wave2,wave3})
            if(wave.size()!=ToneGenerator::waveBankSize) throw std::runtime_error("Invalid v1.21 waveform data");
        if(maximumVoices<24 || maximumVoices>voiceCapacity || maximumVoices%4)
            throw std::runtime_error("Invalid native voice limit");
        tone_=std::make_unique<ToneGenerator>(wave1,wave2,wave3,maximumVoices);
        player_=std::make_unique<NativeMelodicPlayer>(data_,*tone_,
            ImportSystemDefaults(rom1,rom2),ImportRhythmPresets(rom1,rom2),
            ImportMelodicPresets(rom1,rom2),ImportEffectsTables(rom1,rom2));
        if(player_->failed()) throw std::runtime_error("Native synth initialization failed");
    }
    NativeSynth(const NativeSynth&)=delete;
    NativeSynth& operator=(const NativeSynth&)=delete;
    static constexpr unsigned sampleRate=32000;
    std::size_t push(std::span<const uint8_t> midi) noexcept { return player_->push(midi); }
    void setMidiInputSettings(MidiInputSettings settings) noexcept {player_->setMidiInputSettings(settings);}
    MidiInputSettings midiInputSettings() const noexcept {return player_->midiInputSettings();}
    // Same audio owner as push/render; the plug-in has no host MIDI-out yet.
    bool popParameterReply(ParameterReply& reply) noexcept { return player_->popParameterReply(reply); }
    bool setParameterOutputConnected(bool connected) noexcept {return player_->setParameterOutputConnected(connected);}
    bool takeParameterReply(ParameterReply& reply) noexcept {return player_->takeParameterReply(reply);}
    bool completeParameterReplyTransmission() noexcept {return player_->completeParameterReplyTransmission();}
    bool setBulkOutputConnected(bool connected) noexcept { return player_->setBulkOutputConnected(connected); }
    bool takeBulkReply(BulkReplyTransfer::Packet& packet) noexcept { return player_->takeBulkReply(packet); }
    bool completeBulkReplyTransmission() noexcept { return player_->completeBulkReplyTransmission(); }
    bool requestAllSettingsDump() noexcept { return player_->requestAllSettingsDump(); }
    bool requestSettingsDump(BulkReplyTransfer::PanelScope scope) noexcept
    { return player_->requestSettingsDump(scope); }
    bool failed() const noexcept { return outputFailed_ || player_->failed(); }
    // Audio-owner application only. Selection, button mapping and option
    // toggles have already been resolved by the message-thread panel owner.
    // False retains the command at the head of the caller's bounded queue.
    bool applyCommand(const SynthCommand& command) noexcept
    {
        using Kind=SynthCommand::Kind;
        const auto part=std::min<unsigned>(command.part,15);
        // Each command carries the sound-control target resolved by the UI,
        // including commands queued before the next audio render.
        if(selectedPart_!=part || allSelected_!=command.all) {
            selectedPart_=uint8_t(part); allSelected_=command.all;
            player_->selectDisplayPart(gsPart(part),command.all);
        }
        switch(command.kind) {
        case Kind::focus:
            selectedPart_=uint8_t(part); allSelected_=command.all;
            player_->cancelDisplayMessages(command.cancelBitmap);
            // Focus affects which voices are admitted/stopped in SOLO.
            player_->selectDisplayPart(gsPart(part),command.all); break;
        case Kind::adjust:
            return command.all ? player_->adjustMaster(command.parameter,command.delta)
                : player_->adjustPart(gsPart(part),command.parameter,command.delta);
        case Kind::toggleMute: player_->toggleMute(gsPart(part),command.all); break;
        case Kind::solo: player_->setSolo(command.enabled); break;
        case Kind::standby: player_->setStandby(command.enabled); break;
        default: {
            auto settings=player_->midiInputSettings();
            if(command.kind==Kind::receiveExclusive) settings.receiveExclusive=command.enabled;
            else if(command.kind==Kind::receiveReset) settings.receiveReset=command.enabled;
            else if(command.kind==Kind::ignoreChecksum) settings.ignoreChecksum=command.enabled;
            else if(command.kind==Kind::receiveProgramChanges) settings.receiveProgramChanges=command.enabled;
            player_->setMidiInputSettings(settings); break;
        }
        }
        return true;
    }
    void selectPart(int delta) noexcept
    {
        player_->cancelDisplayMessages(false);
        selectedPart_=uint8_t(std::clamp(int(selectedPart_)+delta,0,15));
        player_->selectDisplayPart(gsPart(selectedPart_),allSelected_);
    }
    bool adjustSelectedPart(PartParameter parameter,int delta) noexcept
    {
        // Acceptance, not immediate application: the controller serializes
        // this edit after any in-flight note admission. False is backpressure.
        if(allSelected_) return player_->adjustMaster(parameter,delta);
        return player_->adjustPart(gsPart(selectedPart_),parameter,delta);
    }
    void toggleAll() noexcept
    {
        player_->cancelDisplayMessages(true);
        allSelected_=!allSelected_; player_->selectDisplayPart(gsPart(selectedPart_),allSelected_);
    }
    void toggleMute() noexcept { player_->toggleMute(gsPart(selectedPart_),allSelected_); }
    void toggleSolo() noexcept { player_->setSolo(!player_->soloEnabled()); }
    // Semantic audio-owner operation. The plug-in's settings button is not
    // automatically remapped to the hardware POWER switch.
    void setStandby(bool enabled) noexcept { player_->setStandby(enabled); }
    bool standby() const noexcept { return player_->standby(); }
    bool setFastDisplayScroll(bool enabled) noexcept { return player_->setFastDisplayScroll(enabled); }
    bool fastDisplayScroll() const noexcept { return player_->fastDisplayScroll(); }
    // Called by the audio owner, only when the UI requests a new snapshot.
    SynthState state() const noexcept
    {
        SynthState result;
        result.renderedFrames=tone_->renderedFrames();
        result.keyedVoices=tone_->keys();
        result.activeVoiceMask=result.keyedVoices.lowWord();
        result.failed=failed();
        result.selectedPart=selectedPart_;
        result.allSelected=allSelected_; result.globalMuted=player_->globallyMuted();
        result.soloEnabled=player_->soloEnabled();
        result.masterVolume=player_->masterControls().volume;
        result.masterPan=player_->masterControls().pan;
        result.midiInput=player_->midiInputSettings();
        result.masterKeyShift=player_->masterControls().keyShift;
        result.reverbLevel=player_->effectsSettings().reverb[2];
        result.chorusLevel=player_->effectsSettings().chorus[1];
        result.displayEvents=player_->displayEvents();
        result.voiceLevels=player_->voiceLevels();
        const auto& settings=player_->partSettings();
        for(unsigned part=0;part<16;++part) {
            const auto& source=settings.parts[part];
            const auto& controls=source.controls;
            result.parts[part]={settings.routing[part].channel,controls.program,source.bank,
                controls.volume,controls.expression,controls.pan,controls.reverb,controls.chorus,
                source.keyShift,uint8_t(player_->partVoiceCount(part)),
                bool(settings.routing[part].noteFlags&0x10)};
            result.parts[part].muted=player_->partMuted(part);
            result.parts[part].name=player_->instrumentName(part);
        }
        return result;
    }

    // Always fills the destination, silencing only a failed suffix. No retained
    // host pointers, allocations or firmware execution on this path.
    void render(std::span<AudioFrame<int32_t>> frames) noexcept
    {
        std::size_t offset=0;
        if(!failed()) player_->renderFrames(frames.size(),[&](std::size_t count) noexcept {
            const auto rendered=tone_->render(frames.subspan(offset,count));
            offset+=rendered;
            return rendered;
        });
        if(offset!=frames.size()) outputFailed_=true;
        std::fill(frames.begin()+offset,frames.end(),AudioFrame<int32_t>{});
    }
private:
    static unsigned gsPart(unsigned displayPart) noexcept
    { return displayPart==9 ? 0 : displayPart<9 ? displayPart+1 : displayPart; }
    uint8_t selectedPart_=0;
    bool allSelected_=false;
    SoundData data_;
    std::unique_ptr<ToneGenerator> tone_;
    std::unique_ptr<NativeMelodicPlayer> player_;
    bool outputFailed_=false;
};
}
