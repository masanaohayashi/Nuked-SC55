#pragma once
#include "sc55_voice_engine.h"
#include "pcm.h"
#include "sc55_sysex.h"
#include "sc55_part_settings.h"
#include "sc55_rhythm_settings.h"
#include "sc55_system_defaults.h"
#include "sc55_effects_control.h"
#include "sc55_synth_state.h"
#include "sc55_midi_receive.h"
#include "sc55_parameter_reply.h"
#include "sc55_bulk_reply.h"
#include "sc55_display_events.h"
#include "sc55_voice_commands.h"
#include "sc55_system_settings.h"

namespace sc55
{
// Experimental native melodic/rhythm player. Owns the serialized MIDI/control
// loop, not SoundData or PCM. Those objects must outlive it. Construction is
// setup-time; push/service/step neither allocate nor execute H8 instructions.
// GS/GM reset drains voices and restores imported configuration and effects.
// Mono group reuse/held-key returns are connected; general GS and complete
// portamento-controller compatibility remain outside this preview's scope.
// Rhythm requires setup-time imported presets; an absent table keeps that path
// explicitly unsupported. The historical class name is retained for callers.
// Without an imported melodic table only capital-bank selection is supported.
class NativeMelodicPlayer
{
public:
    // ROM-backed setup. Legacy/custom research callers may still supply only
    // PartSettings; product preview uses this complete imported config source.
    NativeMelodicPlayer(const SoundData& data,pcm_t& pcm,const SystemDefaults& defaults,
        RhythmPresetTable rhythm,MelodicPresetTable melodic,std::optional<EffectsTables> effects = {})
        : NativeMelodicPlayer(data,pcm,defaults.parts(),std::move(rhythm),std::move(melodic))
    {
        defaults_ = defaults;
        system_.master = defaults.master();
        transferSettings_.reset(defaults.bytes);
        std::copy_n(defaults.bytes.begin()+8,16,system_.name.begin());
        std::copy_n(defaults.bytes.begin()+0x18,16,system_.capacity.reserves.begin());
        system_.capacity.startPartControl=defaults.bytes[0x28];
        for(unsigned part=0;part<16;++part) seedPitchHistory(part);
        controllers_ = defaults.controllers();
        effectsTables_=std::move(effects);
        if (effectsTables_) requestDefaultEffects();
    }

    NativeMelodicPlayer(const SoundData& data, pcm_t& pcm,PartSettings settings = {},
        std::optional<RhythmPresetTable> rhythm = {},std::optional<MelodicPresetTable> melodic = {})
        : data_(data), elapsedCycles_(pcm.cycles), pcm_(pcm), parts_(std::move(settings)), rhythm_(std::move(rhythm)), melodic_(std::move(melodic))
    {
        if (!data.pitchTiming() || data.patchCount() < 224
            || !engine_.notes.allocator.initializeTables())
        { failed_ = true; return; }
        if (rhythm_)
        {
            const auto program = rhythm_->resolve(0);
            if (!program) { failed_ = true; return; }
            rhythmSettings_.reset(rhythm_->records[rhythm_->programs[*program]]);
        }
        for (unsigned part = 0; part < 16; ++part)
        {
            parts_.parts[part].bankSelect = parts_.parts[part].bank;
            selectMelodicTone(part);
            seedPitchHistory(part);
            auto& controls = controllers_.parts[part];
            controls.sensitivity = {64,64,64,64,0,0,0,64,0,0,0};
            controls.sourceSensitivity.fill(controls.sensitivity);
            controls.sourceSensitivity[1][0] = 66; // Preview bend range: two semitones.
            controls.assignedControllers = {16,17};
        }
        for (unsigned slot = 0; slot < 24; ++slot)
            engine_.runtime.first[slot].firstStage = engine_.runtime.second[slot].firstStage = 22;
        // 3c=0 emits one frame per PCM pass. Keep the public sample-rate
        // query consistent so the product resampler receives32kHz, not64kHz.
        pcm_.enable_oversampling=false;
        write(0x3c,0); // Undithered PCM output; effects are configured separately.
        write(0x3d,0xb7); // 24 PCM slots, mk1 wave-bank addressing.
        // 04:2643..264b seeds the channel30 LFSR. Zero is an absorbing
        // state, so omitting boot leaves random LFO/tuning reads constant.
        write(0x3e,30); write(0x34,0xff); write(0x35,0xff);
    }

    bool failed() const noexcept { return failed_ || engine_.failed() || queue_.failed(); }
    // Same serialized owner as MIDI ingestion. UI must use its command queue;
    // never mutate receiver settings concurrently with render/EOX preview.
    void setMidiInputSettings(MidiInputSettings settings) noexcept
    {settings.deviceId=std::min<uint8_t>(settings.deviceId,31);midiInput_=settings;}
    MidiInputSettings midiInputSettings() const noexcept {return midiInput_;}
    bool standby() const noexcept { return standby_; }
    bool fastDisplayScroll() const noexcept { return fastDisplayScroll_; }
    bool setFastDisplayScroll(bool enabled) noexcept
    {
        if(!standby_ || failed()) return false;
        if(fastDisplayScroll_!=enabled) displayEvents_.fastScroll(enabled,elapsedCycles_);
        fastDisplayScroll_=enabled; return true;
    }
    void setStandby(bool enabled) noexcept
    {
        if(failed() || standby_==enabled) return;
        standby_=enabled;
        queue_.discardReceived(); sysex_={}; receiveState_={};
        if(enabled) {
            standbyStopPending_=true;
            panelHead_=panelCount_=0;
            cancelDisplayMessages(true);
        }
        serviceRequested_=true;
    }
    bool partMuted(unsigned part) const noexcept
    { return part<16 && !(parts_.routing[part].flags&0x0200); }
    bool globallyMuted() const noexcept { return globalMuted_; }
    bool soloEnabled() const noexcept { return soloEnabled_; }
    void setSolo(bool enabled) noexcept
    {
        if(failed() || soloEnabled_==enabled) return;
        soloEnabled_=enabled;
        // 3d2f/415d: entering solo silences other parts without editing
        // their Note Receive bits. Exiting restores the existing mute policy.
        if(enabled) {
            if(!displayAll_) engine_.requestPartStops(uint16_t(0xffffu^(1u<<displayPart_)));
        } else for(unsigned part=0;part<16;++part)
            if(globalMuted_ || partMuted(part)) engine_.requestPartStops(uint16_t(1u<<part));
        serviceRequested_=true;
    }
    bool adjustMaster(PartParameter parameter,int delta) noexcept
    { return enqueuePanelEdit({parameter,delta,0,true}); }
    bool adjustPart(unsigned part,PartParameter parameter,int delta) noexcept
    { return part<16 && enqueuePanelEdit({parameter,delta,uint8_t(part),false}); }
private:
    bool applyMasterAdjustment(PartParameter parameter,int delta) noexcept
    {
        if(failed()) return false;
        if(parameter==PartParameter::program) cancelDisplayMessages(false);
        const auto adjusted=[&](unsigned value,int low=0,int high=127) {
            return uint8_t(std::clamp(int(value)+delta,low,high));
        };
        switch(parameter) {
        case PartParameter::channel:
            // ALL + MIDI CH edits the device identifier, not any part's RX
            // channel. Panel record04:4b72 clamps the raw value to0..31.
            midiInput_.deviceId=adjusted(midiInput_.deviceId,0,31); return true;
        case PartParameter::volume: system_.master.volume=adjusted(system_.master.volume); break;
        case PartParameter::pan: system_.master.pan=adjusted(system_.master.pan,1); break;
        case PartParameter::keyShift: system_.master.keyShift=adjusted(system_.master.keyShift,40,88); break;
        case PartParameter::reverb:
            if(!effectsTables_) return false;
            system_.effects.reverb[2]=adjusted(system_.effects.reverb[2]);
            effects_.reverb.request(system_.effects.reverb);
            serviceRequested_=true; return true;
        case PartParameter::chorus:
            if(!effectsTables_) return false;
            system_.effects.chorus[1]=adjusted(system_.effects.chorus[1]);
            effects_.chorus.request(system_.effects.chorus);
            serviceRequested_=true; return true;
        default: return false;
        }
        refreshControls(); serviceRequested_=true;
        return true;
    }
public:
    void toggleMute(unsigned part,bool all) noexcept
    {
        if(part>=16 || failed() || soloEnabled_) return;
        if(all) {
            globalMuted_=!globalMuted_;
            if(globalMuted_) engine_.requestPartStops(0xffff);
        } else {
            parts_.routing[part].flags^=0x0200;
            if(partMuted(part)) engine_.requestPartStops(uint16_t(1u<<part));
        }
        serviceRequested_=true;
    }
    // Front-panel edits are semantic part operations, not emulated button
    // pulses or synthetic UART bytes. Caller is the audio owner.
private:
    bool applyPartAdjustment(unsigned part,PartParameter parameter,int delta) noexcept
    {
        if(part>=16 || failed()) return false;
        if(parameter==PartParameter::program) cancelDisplayMessages(false);
        auto& settings=parts_.parts[part]; auto& controls=settings.controls;
        const auto adjusted=[&](unsigned value,int low=0,int high=127) {
            return uint8_t(std::clamp(int(value)+delta,low,high));
        };
        switch(parameter) {
        case PartParameter::program: {
            auto program=adjusted(controls.program);
            // Panel steps select actual kits; MIDI program changes retain
            // their separate family-fallback/rejection policy in resolve().
            if((parts_.routing[part].noteFlags&0x10) && rhythm_ && delta!=0) {
                int candidate=int(controls.program)+(delta>0 ? 1 : -1);
                program=controls.program;
                for(;candidate>=0 && candidate<128;candidate+=delta>0 ? 1 : -1)
                    if(rhythm_->programs[unsigned(candidate)]<rhythm_->records.size()) {
                        program=uint8_t(candidate);break;
                    }
            }
            commitProgram(part,program,settings.bank); break;
        }
        case PartParameter::volume: controls.volume=adjusted(controls.volume); break;
        case PartParameter::pan: controls.pan=adjusted(controls.pan,1); break;
        case PartParameter::reverb: controls.reverb=adjusted(controls.reverb); break;
        case PartParameter::chorus: controls.chorus=adjusted(controls.chorus); break;
        case PartParameter::keyShift: settings.keyShift=adjusted(settings.keyShift,40,88); break;
        case PartParameter::channel:
            if(!resetPart(part,true)) return false;
            parts_.routing[part].channel=adjusted(parts_.routing[part].channel,0,16); break;
        }
        refreshControls(); serviceRequested_=true;
        return !failed();
    }
public:
    uint8_t portamentoSource(unsigned part) const noexcept
    { return part<16 ? engine_.mono[part].source : uint8_t(255); }
    bool reuseInvalidated(unsigned part) const noexcept
    { return engine_.reuseInvalidated(part); }
    uint64_t unsupportedEvents() const noexcept { return unsupported_; }
    uint64_t pcmBoundaryEvents() const noexcept { return pcmBoundaryEvents_; }
    unsigned freeVoices() const noexcept { return engine_.notes.allocator.freeCount; }
    unsigned partVoiceCount(unsigned part) const noexcept
    { return part < 16 ? engine_.notes.allocator.partVoiceCount[part] : 0; }
    // Audio-owner snapshot of melodic allocation, independent of physical slot
    // numbering. Useful when comparing which notes survive voice stealing.
    std::array<uint8_t,128> partNoteVoices(unsigned part) const noexcept
    {
        std::array<uint8_t,128> result{};
        const auto& allocator=engine_.notes.allocator;
        for(unsigned slot=0;slot<24;++slot)
            if(!(allocator.allocations[slot].status&128) && allocator.allocations[slot].part==part) {
                const auto group=allocator.allocations[slot].noteGroup;
                if(group<24 && allocator.noteGroups[group].key<128) ++result[allocator.noteGroups[group].key];
            }
        return result;
    }
    std::array<SynthState::VoiceLevel,24> voiceLevels() const noexcept
    {
        std::array<SynthState::VoiceLevel,24> levels{};
        const auto active=pcm_.voice_mask&pcm_.voice_mask_pending;
        for(unsigned slot=0;slot<24;++slot)
            // PCM key bits and gain registers can retain the last values
            // after the musical voice has been returned. They are not proof
            // that this slot still belongs to a sounding/releasing note.
            if(((active>>slot)&1) && !engine_.notes.allocator.allocations[slot].free()) {
                const auto gains=PCM_PeekVoiceGainLevels(pcm_,slot);
                levels[slot]={gains[0],gains[1],engine_.installation.voices[slot].input.part,true};
            }
        return levels;
    }
    const MasterControls& masterControls() const noexcept { return system_.master; }
    // Audio-owner view of GS bulk48 settings. No H8 RAM or cached duplicate
    // of live part/controller values. Invalid offsets are explicitly absent.
    std::optional<uint8_t> readSystemConfiguration(unsigned offset) const noexcept
    {
        if(offset>=0x748) return {};
        if(const auto value=transferSettings_.read(offset)) return value;
        if(offset<0x48) {
            if(offset==0) return uint8_t(system_.master.tune>>8);
            if(offset==1) return uint8_t(system_.master.tune);
            if(offset==2) return system_.master.volume;
            if(offset==3) return system_.master.portamentoController;
            if(offset==4) return system_.master.resetCommand;
            if(offset==5) return system_.master.keyShift;
            if(offset==6) return system_.master.pan;
            if(offset>=8 && offset<0x18) return system_.name[offset-8];
            if(offset>=0x18 && offset<0x28) return system_.capacity.reserves[offset-0x18];
            if(offset==0x28) return system_.capacity.startPartControl;
            if(offset==0x2a) return system_.effects.reverbMacro;
            if(offset>=0x2b && offset<0x31) return system_.effects.reverb[offset-0x2b];
            if(offset==0x32) return system_.effects.chorusMacro;
            if(offset>=0x33 && offset<0x3a) return system_.effects.chorus[offset-0x33];
            return {};
        }
        const unsigned index=(offset-0x48)/0x70,field=(offset-0x48)%0x70;
        const auto& p=parts_.parts[index]; const auto& r=parts_.routing[index];
        const auto& c=controllers_.parts[index];
        switch(field) {
        case 0: return p.bank;
        case 1: return p.controls.program;
        case 2: return uint8_t(r.flags>>8);
        case 3: return uint8_t(r.flags);
        case 4: return r.channel;
        case 5: return r.noteFlags;
        case 6: return p.keyShift;
        case 7: return p.fineTune;
        case 8: return p.controls.volume;
        case 9: return p.controls.pan;
        case 10: return p.velocity.depth;
        case 11: return p.velocity.offset;
        case 12: return p.keyRange.low;
        case 13: return p.keyRange.high;
        case 14: return p.controls.chorus;
        case 15: return p.controls.reverb;
        default:
            if(field<0x18) return p.controls.tone.values[field-0x10];
            if(field>=0x1a && field<0x26) return p.scale[field-0x1a];
            if(field==0x26 || field==0x27) return c.assignedControllers[field-0x26];
            if(field>=0x28) {
                const unsigned source=(field-0x28)/12,column=(field-0x28)%12;
                if(column==3) return {};
                const auto target=column>3 ? column-1 : column;
                return source==3 ? c.sensitivity[target] : c.sourceSensitivity[source>3 ? source-1 : source][target];
            }
            return {};
        }
    }
    // Audio-owner consumer only. Host MIDI-out routing is not yet connected.
    bool popParameterReply(ParameterReply& reply) noexcept
    {
        if(parameterOutputConnected_ || !replyCount_) return false;
        reply=replies_[replyHead_]; replyHead_=(replyHead_+1)%replies_.size(); --replyCount_;
        return true;
    }
    uint64_t droppedParameterReplies() const noexcept { return droppedReplies_; }
    // Offline diagnostics may capture replies without a transport. Normal
    // playback has no MIDI output and must not prepare or accumulate packets.
    bool setParameterReplyCapture(bool enabled) noexcept
    {
        if(parameterOutputConnected_ || parameterReplyWaiting_ || replyCount_) return false;
        parameterReplyCapture_=enabled; return true;
    }
    // Explicit transport mode: consuming a packet is not transmission
    // completion. The legacy disconnected capture queue never blocks MIDI.
    bool setParameterOutputConnected(bool connected) noexcept
    {
        if(parameterReplyWaiting_ || replyCount_ || bulkReply_.active()) return false;
        parameterOutputConnected_=connected; return true;
    }
    bool parameterReplyPending() const noexcept {return parameterReplyWaiting_;}
    bool takeParameterReply(ParameterReply& reply) noexcept
    {
        if(!parameterOutputConnected_ || !parameterReplyWaiting_ || parameterReplyTransmitting_ || !replyCount_)
            return false;
        reply=replies_[replyHead_]; replyHead_=(replyHead_+1)%replies_.size(); --replyCount_;
        parameterReplyTransmitting_=true;
        return true;
    }
    bool completeParameterReplyTransmission() noexcept
    {
        if(!parameterReplyTransmitting_) return false;
        parameterReplyTransmitting_=parameterReplyWaiting_=false;
        serviceRequested_=true;
        return true;
    }
    // Audio-owner transport contract. Do not enable without a consumer that
    // acknowledges completed transmission. The current plug-in has no MIDI out.
    bool setBulkOutputConnected(bool connected) noexcept
    {
        if(bulkReply_.active() || parameterReplyWaiting_) return false;
        bulkOutputConnected_=connected; return true;
    }
    bool bulkReplyPending() const noexcept { return bulkReply_.active(); }
    // Audio-owner panel command. Rejection is backpressure; no state changes.
    bool requestAllSettingsDump() noexcept
    { return requestSettingsDump(BulkReplyTransfer::PanelScope::allSettings); }
    bool requestSettingsDump(BulkReplyTransfer::PanelScope scope) noexcept
    {
        // Normal panel selection: global mute excludes all parts; otherwise
        // Note Receive/mute determines membership. Associated maps are live.
        // The separate firmware solo-display mode is not yet exposed here.
        if(failed() || !bulkOutputConnected_ || !rhythm_ || resetPending() || parameterReplyWaiting_
            || replyCount_ || queuedEvents() || panelCount_ || engine_.admissionPending() || tasksPending()
            || engine_.runtime.startupPending()) return false;
        uint16_t selectedParts=0;
        uint8_t drumMaps=0;
        for(unsigned part=0;part<16;++part) if(!globalMuted_ && !partMuted(part)) {
            selectedParts|=uint16_t(1u<<part);
            const auto flags=parts_.routing[part].noteFlags;
            if(flags&0x10) drumMaps|=uint8_t((flags>>5)&3);
        }
        if(bulkReply_.beginPanel(scope,selectedParts,drumMaps)!=BulkReplyTransfer::BeginResult::accepted) return false;
        prepareBulkTransmission();
        // Unlike the MIDI sink, this command is outside queue dispatch.
        queue_.discardReceived();sysex_={};receiveState_={};bulkInputReset_=false;
        serviceRequested_=true;
        return !failed();
    }
    bool takeBulkReply(BulkReplyTransfer::Packet& packet) noexcept
    {
        if(replyCount_ || failed()) return false; // Older normal replies first.
        const auto emitted=bulkReply_.next(packet,[&](uint8_t region,unsigned map,unsigned offset) {
            if(region==0x49) return rhythmSettings_.records()[map][offset];
            const auto value=readSystemConfiguration(offset);
            if(!value) { failed_=true; return uint8_t(0); }
            return *value;
        });
        return emitted && !failed();
    }
    bool completeBulkReplyTransmission() noexcept
    {
        const auto completed=bulkReply_.transmissionComplete();
        if(completed) serviceRequested_=true;
        return completed;
    }
    const VoiceCapacityPolicy& capacityPolicy() const noexcept { return system_.capacity; }
#if defined(SC55_NATIVE_IO_AUDIT)
    void holdVoiceAdmissionsAudit(bool hold) noexcept
    { holdAdmissionsAudit_=hold; serviceRequested_=true; }
    void alignActivationTickAudit(uint64_t cyclesUntilTick) noexcept
    {
        const auto period=ControlTaskClock::kernelTickCycles;
        (void)engine_.clock.alignKernelPhaseAudit(uint32_t((period-cyclesUntilTick%period)%period));
    }
    // Diagnostic causality probe only: replay observed H8 control passes,
    // without teaching the product an empirical delay or instruction cost.
    void useExternalControlClockAudit() noexcept
    { externalControlClockAudit_=true; (void)engine_.clock.reset(); }
    bool signalControlPassAudit(uint8_t elapsed) noexcept
    {
        const bool distinct=!engine_.clock.ready();
        engine_.clock.advance(uint64_t(elapsed ? elapsed : 256u)
            *ControlTaskClock::voicePeriodTicks*ControlTaskClock::kernelTickCycles);
        serviceRequested_=true;
        return distinct;
    }
    bool beginControlGroupsAudit(uint8_t elapsed) noexcept
    {
        if(!externalControlClockAudit_ || failed()
            || engine_.periodicWorkPending() || tasksPending()) return false;
        if(effectsTables_ && !serviceEffects()) return false;
        return engine_.runtime.beginControlPass(elapsed);
    }
    VoiceControlRuntime::ControlStep resumeControlGroupAudit() noexcept
    {
        const auto result=engine_.resumeControl(controllers_,
            data_,conversion_,waves_,[&](uint8_t a) { return read(a); },ControlWriter{pcm_});
        serviceRequested_=true;
        return result;
    }
    // Diagnostic only: align readback/hold separately from group publication.
    // The caller supplies observed events, never a product delay or H8 state.
    VoiceControlRuntime::ControlStep selectControlGroupAudit() noexcept
    {
        if(engine_.runtime.controlPhase()!=PeriodicVoiceUpdatePass::Phase::select)
            return {VoiceControlRuntime::ControlProgress::failed};
        return engine_.resumeControl(controllers_,data_,conversion_,waves_,
            [&](uint8_t a) {return read(a);},ControlWriter{pcm_},VoiceControlRuntime::ControlSlice::phase);
    }
    VoiceControlRuntime::ControlStep readControlVoiceAudit(uint8_t expectedSlot) noexcept
    {
        using Phase=PeriodicVoiceUpdatePass::Phase;
        using Progress=VoiceControlRuntime::ControlProgress;
        uint32_t changed=0;
        for(unsigned step=0;step<20;++step) {
            const auto phase=engine_.runtime.controlPhase();
            const bool reading=phase==Phase::readback;
            if(phase==Phase::publish || (reading && engine_.runtime.controlVoice()!=expectedSlot))
                return {Progress::failed,0,changed};
            const auto result=engine_.resumeControl(controllers_,data_,conversion_,waves_,
                [&](uint8_t a) {return read(a);},ControlWriter{pcm_},VoiceControlRuntime::ControlSlice::phase);
            changed|=result.changedMask;
            if(result.status!=Progress::advancedPhase) return {result.status,result.updatedMask,changed};
            if(reading) {
                serviceRequested_=true;
                return {result.status,result.updatedMask,changed};
            }
        }
        return {Progress::failed,0,changed};
    }
    const VoiceAllocator& allocatorAudit() const noexcept { return engine_.notes.allocator; }
    auto controlPositionAudit() const noexcept
    { return std::pair(engine_.runtime.controlPhase(),engine_.runtime.controlVoice()); }
    auto startupAudit() const noexcept { return engine_.runtime.startupAudit(); }
    auto activationDeadlineAudit() const noexcept {return engine_.activationDeadline(elapsedCycles_);}
    bool protectedCalculationAudit() const noexcept {return engine_.runtime.calculationPending();}
    auto controlReadbackAudit(unsigned slot) const noexcept
    {return std::pair(engine_.runtime.readbackCounts.at(slot),engine_.runtime.readbackStages.at(slot));}
    bool controlPassPendingAudit() const noexcept {return engine_.periodicWorkPending();}
    const PartControllerState& controllerSettingsAudit() const noexcept { return controllers_; }
    const VoiceControlState* voiceControlAudit(unsigned slot) const noexcept
    { return slot<24 && engine_.runtime.voices[slot] ? &*engine_.runtime.voices[slot] : nullptr; }
#endif
    const EffectsSettings& effectsSettings() const noexcept { return system_.effects; }
    const DisplayData& displayData() const noexcept { return display_; }
    // Normal play screen: instrument/part keys cancel text; ALL also
    // cancels the bitmap (04:3a10..3a6a). Other value edits preserve it.
    void cancelDisplayMessages(bool bitmap) noexcept
    {
        displayEvents_.cancel(bitmap,elapsedCycles_);
    }
    void selectDisplayPart(unsigned part,bool all) noexcept
    {
        const auto next=std::min(part,15u);
        // 4131..415c: in solo, changing the selected part stops the old one.
        if(soloEnabled_ && !all && (displayAll_ || displayPart_!=next)) {
            engine_.requestPartStops(uint16_t(0xffffu^(1u<<next)));
            serviceRequested_=true;
        }
        displayPart_=next; displayAll_=all;
    }
    DisplayEvents displayEvents() const noexcept {return displayEvents_.snapshot(elapsedCycles_);}
    uint64_t rejectedSysEx() const noexcept { return rejectedSysEx_; }
    bool resetPending() const noexcept { return resetRequested_; }
    uint64_t completedResets() const noexcept { return completedResets_; }
    uint64_t completedReceiveRecoveries() const noexcept { return completedReceiveRecoveries_; }
    bool effectsSettled() const noexcept
    { return !effectsTables_ || (effects_.reverb.phase==0 && effects_.chorus.phase==0); }
    const PartSettings& partSettings() const noexcept { return parts_; }
    const std::array<RhythmPresetTable::Record,2>& rhythmRecords() const noexcept { return rhythmSettings_.records(); }
    std::optional<uint8_t> currentMonoKey(unsigned part) const noexcept
    { return part<16 && !(parts_.routing[part].noteFlags&0x90) ? std::optional<uint8_t>(engine_.mono[part].current) : std::nullopt; }
    std::optional<uint16_t> selectedTone(unsigned part) const noexcept
    { return part < 16 ? selectedTone_[part] : std::nullopt; }
    // Audio-owner readback, shared by normal LCD and message-scroll return
    // text. Rhythm names belong to the editable map, not the factory preset.
    std::array<char,12> instrumentName(unsigned part) const noexcept
    {
        std::array<char,12> name; name.fill(' ');
        if(part>=16) return name;
        if(parts_.routing[part].noteFlags&0x10) {
            // 04:55f3..5627 uses the display selection latch (CE35), which
            // 04:0998..09a1 updates to the fallback-resolved program.
            const auto program=selectedRhythmProgram_[part];
            if(rhythm_ && rhythm_->programs[program]!=255) {
                const unsigned map=(parts_.routing[part].noteFlags&0x20) ? 0 : 1;
                name=rhythmSettings_.name(map);
            }
        } else if(selectedTone_[part]) {
            if(const auto* patch=data_.patch(*selectedTone_[part]))
                std::copy_n(patch->name.begin(),std::min(name.size(),patch->name.size()),name.begin());
        }
        return name;
    }
    std::size_t queuedEvents() const noexcept
    { return queue_.size() + engine_.commands.size() + (engine_.admissionPending() ? 1 : 0); }
    std::size_t push(std::span<const uint8_t> bytes) noexcept
    {
        if (failed()) return 0;
        // Normal bulk disables RX, then04:2738 drains pending UART bytes
        // before restoring interrupts. Transfer-time input is not replayed.
        if(standby_ || bulkReply_.active()) return bytes.size();
        if (!bytes.empty()) serviceRequested_=true;
        std::size_t consumed=0;
        for (const auto byte:bytes) {
            auto next=receiveState_;
            if (next.receiveByte(byte) && queue_.push(std::span(&byte,1),elapsedCycles_).consumed!=1)
                break;
            receiveState_=next;
            ++consumed;
        }
        return consumed;
    }

    uint32_t controlCyclesUntilNextTick() const noexcept
    { return engine_.clock.untilNextExpiration(); }

    // One bounded PCM slice. PCM_Update rounds up to a complete chip pass;
    // charge that actual duration, including any deadline overshoot, to the
    // control clock. The caller splits host buffers at MIDI offsets.
    void step() noexcept
    {
        service();
        const auto duration = std::min<uint32_t>(625,cyclesUntilNextService());
        const auto before=pcm_.cycles;
        PCM_Update(pcm_,pcm_.cycles + duration);
        advanceTime(pcm_.cycles-before);
        service();
    }

    // Block scheduler for the fixed v1.21 24-slot/32kHz PCM configuration.
    // MIDI has already split the host span. Within it, only the common control
    // deadline, a PCM interrupt or an unfinished transition requires a yield.
    void renderFrames(std::size_t frames) noexcept
    {
        renderFrames(frames,[&](std::size_t count) noexcept {
            const auto before=pcm_.cycles;
            PCM_Update(pcm_,before+count*625,true);
            const auto elapsed=pcm_.cycles-before;
            return elapsed%625 ? std::size_t(0) : std::size_t(elapsed/625);
        });
    }

    // The synth owns signal generation and output buffers. The controller only
    // supplies deadlines and consumes the number of frames actually rendered;
    // a waveform event may end a batch early. No retained callable or allocation.
    template<class Render>
    void renderFrames(std::size_t frames,Render&& render) noexcept
    {
        constexpr uint32_t cyclesPerFrame=625;
        while(frames && !failed()) {
            service();
            if(failed()) break;
            const auto untilControl=(cyclesUntilNextService()+cyclesPerFrame-1)/cyclesPerFrame;
            const auto count=serviceRequested_ ? std::size_t(1) : std::min<std::size_t>(frames,untilControl);
            const auto rendered=render(count);
            if(!rendered || rendered>count) {
                failed_=true; break;
            }
            frames-=rendered;
            advanceTime(rendered*cyclesPerFrame);
            service();
        }
    }

private:
    uint32_t cyclesUntilNextService() const noexcept
    {
        auto next=std::min(engine_.clock.untilNextExpiration(),MidiReceiveTimer::periodCycles-receiveTimerPhase_);
        if(const auto ticks=bulkReply_.spacingTicksRemaining())
            next=std::min(next,uint32_t((ticks-1)*ControlTaskClock::kernelTickCycles
                +engine_.clock.untilNextKernelTick()));
        if(const auto activation=engine_.activationDeadline(elapsedCycles_)) next=std::min(next,*activation);
        return next;
    }
    void advanceTime(uint64_t cycles) noexcept
    {
        const auto previousCycles=elapsedCycles_;
        const auto kernelTicks=engine_.clock.kernelTicksIn(cycles);
        elapsedCycles_+=cycles;
        if(engine_.activationWaiting(previousCycles) && !engine_.activationWaiting(elapsedCycles_)) {
            serviceRequested_=true;
        }
        if(bulkReply_.active()) {
            const bool panelTransfer=bulkReply_.panelTransferActive();
            bulkReply_.advanceKernelTicks(unsigned(kernelTicks));
            if(!bulkReply_.active()) {
                // 713e finishes via28c2: discard panel events accumulated
                // during the outer transfer, not between individual requests.
                if(panelTransfer) panelHead_=panelCount_=0;
                serviceRequested_=true;
            }
        }
#if defined(SC55_NATIVE_IO_AUDIT)
        if(externalControlClockAudit_) engine_.clock.advanceWithoutEventAudit(cycles);
        else
#endif
            engine_.clock.advance(cycles);
        receiveTimerPhase_+=uint32_t(cycles);
        while(receiveTimerPhase_>=MidiReceiveTimer::periodCycles) {
            receiveTimerPhase_-=MidiReceiveTimer::periodCycles;
            // A bulk packet awaiting real transport completion keeps TX busy.
            // The post-drain spacing is not itself a busy transmitter.
            if(receiveTimer_.tick(receiveState_,!bulkReply_.transmitting() && !parameterReplyWaiting_,true)) {
                if(!queue_.pushReceiveRecovery(elapsedCycles_)) { failed_=true; return; }
                serviceRequested_=true;
            }
        }
    }
    void seedPitchHistory(unsigned part) noexcept
    { seedPitchHistory(part,selectedTone_[part]); }
    void seedPitchHistory(unsigned part,std::optional<uint16_t> tone) noexcept
    { engine_.seedPitchHistory(part,tone,voiceConfiguration(),data_); }
    bool stopPartGroups(unsigned part) noexcept
    {
        return engine_.stopPartGroups(part,[&](uint8_t a) { return read(a); },ControlWriter{pcm_});
    }
    void setPartModeValue(unsigned part,bool poly) noexcept
    {
        if(poly) parts_.routing[part].noteFlags|=0x80;
        else parts_.routing[part].noteFlags&=0x7f;
    }
    bool applyPartMode(unsigned part,bool poly) noexcept
    {
        if(!engine_.changePartMode(part,poly,[&](uint8_t a) { return read(a); },ControlWriter{pcm_})) return false;
        return true;
    }
    bool changeRhythmMode(unsigned part,uint8_t value) noexcept
    {
        auto& settings=parts_.parts[part]; auto& flags=parts_.routing[part].noteFlags;
        if(!rhythm_) return false;
        //1553: change routing/program latches, not sounding voice ownership.
        // Joining a shared drum map does NOT reload/erase that edited map.
        flags=uint8_t((flags&0x8f)|(value==1 ? 0x30 : value==2 ? 0x50 : 0));
        settings.bank=settings.bankSelect=0;
        settings.controls.program=value ? rhythmMapPrograms_[value-1] : 0;
        // RPN coarse tuning survives this routing change (AB46 is untouched).
        // 04:15c2 clears the receiver's rejected-tone latch unconditionally.
        // Joining a map does not reissue its (possibly invalid) Program Change:
        // the existing editable map remains playable until a new PC rejects it.
        rhythmRejected_[part]=false;
        if(value) {
            const auto resolved=rhythm_->resolve(settings.controls.program);
            selectedRhythmProgram_[part]=resolved.value_or(settings.controls.program);
        } else selectMelodicTone(part);
        return true;
    }
    void requestDefaultEffects() noexcept
    {
        std::copy_n(defaults_->bytes.begin()+0x2b,6,system_.effects.reverb.begin());
        std::copy_n(defaults_->bytes.begin()+0x33,7,system_.effects.chorus.begin());
        system_.effects.reverbMacro=defaults_->bytes[0x2a];
        system_.effects.chorusMacro=defaults_->bytes[0x32];
        // Boot/reset must configure even a default matching the zero cache.
        effects_.reverb.phase=2; effects_.chorus.phase=2;
        effects_.reverb.request(system_.effects.reverb); effects_.chorus.request(system_.effects.chorus);
    }
    void storeBulkSystemByte(unsigned offset,uint8_t value) noexcept
    {
        if(transferSettings_.write(offset,value)) return;
        if(offset<0x48) {
            if(offset==0) system_.master.tune=uint16_t((system_.master.tune&255)|(unsigned(value)<<8));
            else if(offset==1) system_.master.tune=uint16_t((system_.master.tune&0xff00)|value);
            else if(offset==2) system_.master.volume=value;
            else if(offset==3) system_.master.portamentoController=value;
            else if(offset==4) system_.master.resetCommand=value;
            else if(offset==5) system_.master.keyShift=value;
            else if(offset==6) system_.master.pan=value;
            else if(offset>=8 && offset<0x18) system_.name[offset-8]=value;
            else if(offset>=0x18 && offset<0x28) system_.capacity.reserves[offset-0x18]=value;
            else if(offset==0x28) system_.capacity.startPartControl=value;
            else if(offset==0x2a) system_.effects.reverbMacro=value;
            else if(offset>=0x2b && offset<0x31) system_.effects.reverb[offset-0x2b]=value;
            else if(offset==0x32) system_.effects.chorusMacro=value;
            else if(offset>=0x33 && offset<0x3a) system_.effects.chorus[offset-0x33]=value;
            return;
        }
        const unsigned index=(offset-0x48)/0x70,field=(offset-0x48)%0x70;
        auto& part=parts_.parts[index]; auto& route=parts_.routing[index];
        auto& controller=controllers_.parts[index];
        switch(field) {
        case 0: part.bank=part.bankSelect=value; break;
        case 1: part.controls.program=value; break;
        case 2: route.flags=uint16_t((route.flags&255)|(unsigned(value)<<8)); break;
        case 3: route.flags=uint16_t((route.flags&0xff00)|value); break;
        case 4: route.channel=value; break;
        case 5: route.noteFlags=value; break;
        case 6: part.keyShift=value; break;
        case 7: part.fineTune=value; break;
        case 8: part.controls.volume=value; break;
        case 9: part.controls.pan=value; break;
        case 10: part.velocity.depth=value; break;
        case 11: part.velocity.offset=value; break;
        case 12: part.keyRange.low=value; break;
        case 13: part.keyRange.high=value; break;
        case 14: part.controls.chorus=value; break;
        case 15: part.controls.reverb=value; break;
        default:
            if(field<0x18) part.controls.tone.values[field-0x10]=value;
            else if(field>=0x1a && field<0x26) part.scale[field-0x1a]=value;
            else if(field==0x26 || field==0x27) controller.assignedControllers[field-0x26]=value;
            else if(field>=0x28) {
                const unsigned source=(field-0x28)/12,column=(field-0x28)%12;
                // Column3 is unused by the modulation calculations.
                if(column!=3) {
                    const auto target=column>3 ? column-1 : column;
                    if(source==3) controller.sensitivity[target]=value;
                    else controller.sourceSensitivity[source>3 ? source-1 : source][target]=value;
                }
            }
            break;
        }
    }
    bool serviceEffects() noexcept
    {
        struct EffectWriter {
            pcm_t& pcm;
            void configureChorus(const ChorusSetup& setup) const noexcept
            { PCM_ConfigureChorus(pcm,setup); }
            void configureReverb(const ReverbSetup& setup) const noexcept
            { PCM_ConfigureReverb(pcm,setup); }
            void updateEffect(EffectParameter parameter,uint16_t value) const noexcept
            { PCM_UpdateEffect(pcm,parameter,value); }
            void setChorusMix(const std::array<uint8_t,3>& mix) const noexcept
            { PCM_SetChorusMix(pcm,mix); }
            void beginReverbDrain() const noexcept { PCM_BeginReverbDrain(pcm); }
        };
        const EffectWriter word{pcm_};
        if(!effects_.advance(*effectsTables_,system_.effects.reverb[0],word)) {
            failed_=true; return false;
        }
        return true;
    }

    void requestReset() noexcept
    {
        if (!defaults_ || !rhythm_ || !melodic_) { ++unsupported_; return; }
        resetRequested_ = true;
    }
    // Runs after normal stop-task/periodic service, never instead of PCM time
    // progression. Later MIDI remains owned by queue_ until this completes.
    void serviceReset() noexcept
    {
        const auto load = [&](uint8_t a) { return read(a); };
        const auto store = ControlWriter{pcm_};
        const auto progress=engine_.resetVoices(load,store);
        using Progress=NativeVoiceEngine::ResetProgress;
        if(progress==Progress::failed) { failed_=true; return; }
        if(progress==Progress::waiting) return;
        if(progress==Progress::stopped)
        {
            for (unsigned part=0;part<16;++part)
                if (!resetPart(part,false)) { failed_=true; return; }
            return;
        }
        // GS/GM reset restores sound settings, not front-panel solo/global mute
        // or selection. Physical-panel + reset + subsequent-note comparison:
        // --native-panel-solo. Do not clear those independently owned modes here.
        parts_=defaults_->parts(true); system_.master=defaults_->master(); controllers_=defaults_->controllers();
        transferSettings_.reset(defaults_->bytes);
        displayEvents_.cancel(true,elapsedCycles_);
        std::copy_n(defaults_->bytes.begin()+8,16,system_.name.begin());
        std::copy_n(defaults_->bytes.begin()+0x18,16,system_.capacity.reserves.begin());
        system_.capacity.startPartControl=defaults_->bytes[0x28];
        rhythmRejected_={}; selectedRhythmProgram_={}; rhythmMapPrograms_={};
        const auto program=rhythm_->resolve(0);
        if (!program) { failed_=true; return; }
        rhythmSettings_.reset(rhythm_->records[rhythm_->programs[*program]]);
        for (unsigned part=0;part<16;++part)
        { selectMelodicTone(part); seedPitchHistory(part); }
        // 04:12b8/12be posts task8 effect requests, then 04:379f completes
        // reset to the MIDI task. FX ramps continue independently; waiting
        // for them here incorrectly stalls all subsequent MIDI, even RQ1.
        if (effectsTables_) requestDefaultEffects();
        resetRequested_=false; ++completedResets_;
    }
    std::optional<uint16_t> resolveMelodicTone(uint8_t bank,uint8_t program) const noexcept
    {
        if(melodic_) {
            const auto selection=melodic_->resolve(bank,program);
            return selection ? std::optional<uint16_t>(selection->tone) : std::nullopt;
        }
        return ResolveV121MelodicPreset(bank,program);
    }
    void selectMelodicTone(unsigned part) noexcept
    {
        const auto& settings=parts_.parts[part];
        selectedTone_[part]=resolveMelodicTone(settings.bank,settings.controls.program);
        updateCapacityMode(part,selectedTone_[part]);
    }
    void updateCapacityMode(unsigned part,std::optional<uint16_t> tone) noexcept
    {
        // v1.21 08da..08f0: selecting a melodic patch derives A040[part]
        // from record+0d bit0. Capacity1737 then chooses FIFO or the existing
        // three-pass candidate selection using this retained per-part mode.
        if(tone)
            if(const auto* patch=data_.patch(*tone))
                system_.capacity.modes[part]=(patch->common[1]&1) ? 2 : 0;
    }
    void applyProgramVoiceState(unsigned part,uint16_t tone) noexcept
    {
        updateCapacityMode(part,tone);
        engine_.programChanged(part,tone,voiceConfiguration(),data_);
    }
    // Shared committed program selection. MIDI gates are checked by receive;
    // GS supplies the addressed part directly, independent of MIDI RX flags.
    void commitProgram(unsigned part,uint8_t program,uint8_t bank) noexcept
    {
        parts_.parts[part].bank = parts_.parts[part].bankSelect = bank;
        if (!(parts_.routing[part].noteFlags&0x10))
        {
            const auto previous=selectedTone_[part];
            parts_.parts[part].controls.program = program;
            selectMelodicTone(part);
            if(selectedTone_[part] && selectedTone_[part]!=previous) {
                applyProgramVoiceState(part,*selectedTone_[part]);
            }
            if (!melodic_ && parts_.parts[part].bank != 0) ++unsupported_;
            return;
        }
        if (!rhythm_) { ++unsupported_; return; }
        if (parts_.parts[part].bank != 0) return;
        parts_.parts[part].controls.program = program;
        const unsigned map = (parts_.routing[part].noteFlags&0x20) ? 0 : 1;
        rhythmMapPrograms_[map]=program;
        const auto resolved = rhythm_->resolve(program);
        selectedRhythmProgram_[part] = resolved.value_or(program);
        rhythmRejected_[part] = !resolved;
        if (!resolved) return;
        rhythmSettings_.selectPreset(map,rhythm_->records[rhythm_->programs[*resolved]]);
        for (unsigned peer = 0; peer < 16; ++peer)
            if ((parts_.routing[peer].noteFlags&0x10)
                && (((parts_.routing[peer].noteFlags&0x20) ? 0u : 1u) == map))
            {
                parts_.parts[peer].controls.program = program;
                selectedRhythmProgram_[peer] = *resolved;
            }
    }
    uint8_t read(uint8_t address) noexcept { return PCM_Read(pcm_,address); }
    void write(uint8_t address,uint8_t value) noexcept { PCM_Write(pcm_,address,value); }
    struct ControlWriter
    {
        pcm_t& target;
        void operator()(uint8_t address,uint8_t value) const noexcept { PCM_Write(target,address,value); }
        void updateVoice(uint8_t channel,const VoiceRenderUpdate& update) const noexcept
        { PCM_ApplyVoiceUpdate(target,channel,update); }
        void installVoice(uint8_t channel,const VoiceRenderStart& start) const noexcept
        { PCM_InstallVoice(target,channel,start); }
        void commitVoiceKeys(uint32_t enabled) const noexcept { PCM_CommitVoiceKeys(target,enabled); }
        std::array<uint16_t,2> voiceGainLevels(uint8_t channel) const noexcept
        { return PCM_VoiceGainLevels(target,channel); }
        uint16_t voiceRampLevel(uint8_t channel,EnvelopeRamp::Stage stage) const noexcept
        { return PCM_VoiceRampLevel(target,channel,stage); }
        void setVoiceRamp(uint8_t channel,EnvelopeRamp::Stage stage,uint16_t command) const noexcept
        { PCM_SetVoiceRamp(target,channel,stage,command); }
        void setVoicePitch(uint8_t channel,uint16_t increment) const noexcept
        { PCM_SetVoicePitch(target,channel,increment); }
        uint16_t randomWord() const noexcept { return PCM_ControlRandomWord(target); }
        bool completeVoiceEnable(uint8_t channel,uint16_t level,uint16_t command) const noexcept
        { return PCM_CompleteVoiceEnable(target,channel,level,command); }
        std::array<uint16_t,3> synchronizeEnvelopes(uint8_t channel,
            const std::array<uint16_t,3>& commands,const std::array<uint16_t,3>& levels) const noexcept
        { return PCM_SynchronizeVoiceEnvelopes(target,channel,commands,levels); }
    };
    bool tasksPending() const noexcept
    { return engine_.operationsPending(); }
    NativeVoiceEngine::Configuration voiceConfiguration() const noexcept
    { return {parts_,system_.master,system_.capacity,rhythmSettings_,rhythm_ ? &*rhythm_ : nullptr,
        effectsTables_.has_value()}; }
    void refreshControls() noexcept
    { engine_.refreshControls(voiceConfiguration()); }
    bool resetVoiceControllers(unsigned part,bool allNotes) noexcept
    {
        if (!engine_.resetVoiceControllers(part,(parts_.routing[part].noteFlags&0x10)!=0,allNotes))
        { failed_ = true; return false; }
        return true;
    }
    void resetControllerValues(unsigned part) noexcept
    {
        auto& controls = parts_.parts[part].controls;
        controls.expression = 127; controls.softPedal = false;
        controls.rpnMsb = controls.rpnLsb = 127; controls.nrpnSelected = false;
        controls.nrpnMsb=controls.nrpnLsb=255;
        controls.fineTuning=0; // 0844 clears the entry latch, not AB76 pitch.
        // Configuration/sensitivities survive; only dynamic contributions and
        // key pressure reset (04:0844). Volume/program/coarse tuning survive.
        controllers_.parts[part].contributions = {};
        controllers_.parts[part].keyPressure.fill(0);
    }
    bool resetPart(unsigned part,bool allNotes) noexcept
    {
        if(!resetVoiceControllers(part,allNotes)) return false;
        resetControllerValues(part);
        return true;
    }
    bool requiresVoiceTransaction(const SysExReceiver::Result& packet) const noexcept
    {
        using Status=SysExReceiver::Status;
        if(packet.status==Status::gmOn) return midiInput_.receiveReset;
        if(packet.status!=Status::roland || packet.model!=0x42) return false;
        // Transfer/reset still own voice-side work.
        // Keep their ordering until those producers use VoiceCommands too.
        // Master/controller/FX values, reserve tables, drum maps and model45
        // display data are receiver-owned; they do not prepare/stop voices.
        if(packet.command==0x11) return true;
        if(packet.command!=0x12 || packet.payload.empty()) return false;
        const auto address=packet.payload;
        if(address[0]!=0x40 || address.size()<2) return false;
        // Use the same bounded scalar parser as application: reset may occur
        // after volume/key/pan/portamento in a multi-record transaction.
        MasterControls preview;
        return midiInput_.receiveReset && preview.write(address)==MasterControls::WriteResult::resetRequested;
    }
    SysExReceiver::Result decodeExclusive(SysExReceiver& receiver,const MidiDecoder::Event& event) const noexcept
    {return receiver.receive(event,midiInput_.deviceId,midiInput_.receiveExclusive,midiInput_.ignoreChecksum);}
    MidiDispatchResult receiveBulkSettings(std::span<const uint8_t> payload) noexcept
    {
        // 04:1617..165a reselects all parts in descending order and publishes
        // ordinary program commands. It does not drain earlier note commands.
        // Preview only the three fields that affect melodic tone selection.
        struct Selection { uint8_t bank,program,noteFlags; };
        std::array<Selection,16> selections{};
        for(unsigned part=0;part<16;++part)
            selections[part]={parts_.parts[part].bank,parts_.parts[part].controls.program,
                parts_.routing[part].noteFlags};
        if(!BulkSystemData::write(payload,[&](unsigned offset,uint8_t value) {
            if(offset<0x48) return;
            auto& selection=selections[(offset-0x48)/0x70];
            switch((offset-0x48)%0x70) {
                case 0: selection.bank=value; break;
                case 1: selection.program=value; break;
                case 5: selection.noteFlags=value; break;
                default: break;
            }
        })) { ++rejectedSysEx_; return MidiDispatchResult::accepted; }
        std::array<VoiceCommand,16> commands{};
        std::array<std::optional<uint16_t>,16> tones{};
        unsigned count=0;
        for(unsigned part=16;part-- >0;) {
            const auto& selection=selections[part];
            if(selection.noteFlags&0x10) continue;
            tones[part]=resolveMelodicTone(selection.bank,selection.program);
            if(tones[part] && tones[part]!=selectedTone_[part])
                commands[count++]=ProgramVoiceRequest{uint8_t(part),*tones[part]};
        }
        if(!engine_.commands.publish(std::span(commands).first(count))) return MidiDispatchResult::deferred;
        BulkSystemData::write(payload,[&](unsigned offset,uint8_t value) {storeBulkSystemByte(offset,value);});
        for(unsigned part=16;part-- >0;) {
            auto& settings=parts_.parts[part];
            if(parts_.routing[part].noteFlags&0x10) {
                // A higher rhythm part can change a lower peer's program.
                // Read the live shared-map state, not the preview snapshot.
                commitProgram(part,settings.controls.program,settings.bank);
            } else {
                settings.bankSelect=settings.bank;
                selectedTone_[part]=tones[part];
                if(!melodic_ && settings.bank!=0) ++unsupported_;
            }
        }
        if(effectsTables_) {
            effects_.reverb.request(system_.effects.reverb);
            effects_.chorus.request(system_.effects.chorus);
        }
        refreshControls();
        return MidiDispatchResult::accepted;
    }
    void prepareBulkTransmission() noexcept
    {
        bulkInputReset_=true;
        // 04:1d6c: performed once for the whole panel sequence, or once for
        // a normal external request. Never reset between its internal requests.
        for(unsigned part=16;part-- >0;)
            if(!stopPartGroups(part)) {failed_=true;return;}
        for(unsigned part=16;part-- >0;)
            if(!resetPart(part,false)) {failed_=true;return;}
        refreshControls();
    }
    MidiDispatchResult receivePartSettings(std::span<const uint8_t> payload) noexcept
    {
        // A table write may publish channel reset + notes-off + mode change.
        // Decode once into a candidate plus semantic side effects. Preserve
        // valid-prefix semantics even on a later unsupported record, but do
        // not publish anything until the whole voice-command batch fits.
        auto preview=parts_;
        const auto part=unsigned(payload[1]&15);
        auto assigned=controllers_.parts[part].assignedControllers;
        bool resetControls=false,selectProgram=false;
        std::optional<uint8_t> rhythmMode;
        std::array<VoiceCommand,3> commands{};
        unsigned count=0;
        const auto written=preview.write(payload,
            [&](unsigned index) {
                resetControls=true;
                commands[count++]=ControllerResetRequest{uint8_t(index)};
                commands[count++]=PartReleaseRequest{PartReleaseRequest::Kind::notes,uint8_t(index)};
                return true;
            },
            [&](unsigned index,uint8_t bank,uint8_t program) {
                selectProgram=true;
                auto& candidate=preview.parts[index];
                candidate.bank=candidate.bankSelect=bank;
                candidate.controls.program=program;
                if(!(preview.routing[index].noteFlags&0x10)) {
                    const auto tone=resolveMelodicTone(bank,program);
                    if(tone && tone!=selectedTone_[index])
                        commands[count++]=ProgramVoiceRequest{uint8_t(index),*tone};
                }
                return true;
            },&assigned,
            [&](unsigned index,uint8_t address,uint8_t value) {
                if(address==0x13) {
                    if(bool(preview.routing[index].noteFlags&0x80)!=bool(value))
                        commands[count++]=PartModeRequest{uint8_t(index),value!=0};
                    if(value) preview.routing[index].noteFlags|=0x80;
                    else preview.routing[index].noteFlags&=0x7f;
                    return true;
                }
                if(address!=0x15 || !rhythm_) return false;
                rhythmMode=value;
                return true;
            });
        if(!engine_.commands.publish(std::span(commands).first(count))) return MidiDispatchResult::deferred;
        parts_.parts[part]=preview.parts[part];
        parts_.routing[part]=preview.routing[part];
        controllers_.parts[part].assignedControllers=assigned;
        // Dynamic MIDI reset does not alter fields set by the scalar table
        // following channel assignment (volume/pan/tuning configuration etc.).
        if(resetControls) resetControllerValues(part);
        if(selectProgram) {
            const auto& settings=parts_.parts[part];
            if(parts_.routing[part].noteFlags&0x10)
                commitProgram(part,settings.controls.program,settings.bank);
            else {
                selectedTone_[part]=resolveMelodicTone(settings.bank,settings.controls.program);
                if(!melodic_ && settings.bank!=0) ++unsupported_;
            }
        }
        if(rhythmMode && !changeRhythmMode(part,*rhythmMode)) {failed_=true;return MidiDispatchResult::failed;}
        if(written==PartSettings::WriteResult::unsupported) ++unsupported_;
        else if(written==PartSettings::WriteResult::invalidLength) ++rejectedSysEx_;
        refreshControls();
        return MidiDispatchResult::accepted;
    }
    MidiDispatchResult receive(const MidiDecoder::Event& event,bool allowVoiceTransactions=true) noexcept
    {
        if(event.kind==MidiDecoder::Kind::receiveRecovery) {
            receiveRecovery_=true;
            return MidiDispatchResult::accepted;
        }
        if (event.kind == MidiDecoder::Kind::realtime) return MidiDispatchResult::accepted;
        if (event.kind != MidiDecoder::Kind::message)
        {
            if(event.kind==MidiDecoder::Kind::sysexEnd) {
                // Inspect the complete packet without consuming EOX. A
                // deferred sink must leave both queue and receiver unchanged.
                // The fixed-size copy is only needed at this transaction seam;
                // no borrowed packet survives dispatch, allocation or replay.
                auto preview=sysex_;
                const auto packet=decodeExclusive(preview,event);
                if(packet.status==SysExReceiver::Status::roland && packet.model==0x42 && packet.command==0x11) {
                    const bool bulk=!packet.payload.empty()
                        && (packet.payload[0]==0x48 || packet.payload[0]==0x49);
                    const bool outputEnabled=bulk ? bulkOutputConnected_
                        : parameterOutputConnected_ || parameterReplyCapture_;
                    // With no output endpoint this request has no work to do.
                    // Consume EOX before the voice-transaction barrier, so an
                    // ignored query cannot hold up following input settings.
                    if(!outputEnabled) {
                        (void)decodeExclusive(sysex_,event);
                        return MidiDispatchResult::accepted;
                    }
                }
                if(packet.status==SysExReceiver::Status::roland && packet.model==0x42 && packet.command==0x12
                    && packet.payload.size()>=3 && packet.payload[0]==0x48) {
                    const auto result=receiveBulkSettings(packet.payload);
                    if(result==MidiDispatchResult::accepted) (void)decodeExclusive(sysex_,event);
                    return result;
                }
                if(packet.status==SysExReceiver::Status::roland && packet.model==0x42 && packet.command==0x12
                    && packet.payload.size()>=2 && packet.payload[0]==0x40 && (packet.payload[1]&0xf0)==0x10) {
                    const auto result=receivePartSettings(packet.payload);
                    if(result==MidiDispatchResult::accepted) (void)decodeExclusive(sysex_,event);
                    return result;
                }
                if((!allowVoiceTransactions || engine_.admissionPending() || engine_.commands.size() || tasksPending()
                    || engine_.runtime.startupPending()) && requiresVoiceTransaction(packet))
                    return MidiDispatchResult::deferred;
            }
            const auto result = decodeExclusive(sysex_,event);
            using Status = SysExReceiver::Status;
            if (result.status == Status::roland)
            {
                if(result.model==0x45 && result.command==0x12) {
                    if(display_.write(result.payload)!=DisplayData::WriteResult::applied)
                        ++rejectedSysEx_;
                    else displayEvents_.receive(display_,result.payload[1]==1,elapsedCycles_);
                }
                else if(result.model==0x42 && result.command==0x11) {
                    if(result.payload.size()>=1 && (result.payload[0]==0x48 || result.payload[0]==0x49)) {
                        if(!bulkOutputConnected_ || (result.payload[0]==0x49 && !rhythm_)) ++unsupported_;
                        else if(bulkReply_.begin(result.payload)!=BulkReplyTransfer::BeginResult::accepted) ++rejectedSysEx_;
                        else prepareBulkTransmission();
                        return MidiDispatchResult::accepted;
                    }
                    if(!parameterOutputConnected_ && !parameterReplyCapture_)
                        return MidiDispatchResult::accepted;
                    ParameterReply reply;
                    auto status=reply.prepare(result.payload,system_.master,parts_,controllers_);
                    if(defaults_ && result.payload.size()>=3 && result.payload[0]==0x40
                        && (result.payload[1]&0xf0)==0x30)
                        status=reply.prepareInformation(result.payload,defaults_->identity,
                            [&](uint32_t address) {return PCM_ReadROM(pcm_,address);});
                    if(status==ParameterReply::Result::unsupported)
                        status=reply.prepareSystem(result.payload,system_.name,system_.capacity,system_.effects,
                            rhythm_ ? std::span<const RhythmPresetTable::Record>(rhythmSettings_.records()) : std::span<const RhythmPresetTable::Record>{},
                            *readSystemConfiguration(4));
                    if(status==ParameterReply::Result::unsupported) ++unsupported_;
                    else if(status==ParameterReply::Result::invalidLength) ++rejectedSysEx_;
                    else if(replyCount_==replies_.size()) ++droppedReplies_;
                    else {
                        replies_[(replyHead_+replyCount_)%replies_.size()]=reply; ++replyCount_;
                        parameterReplyWaiting_=parameterOutputConnected_;
                    }
                }
                else if (result.model != 0x42 || result.command != 0x12) ++unsupported_;
                else
                {
                    if(result.payload.size()>=3 && result.payload[0]==0x40 && result.payload[1]<=1) {
                        applySystemSettings(result.payload);
                    }
                    else if(result.payload.size()>=2 && result.payload[0]==0x40
                        && (result.payload[1]&0xf0)==0x20) {
                        const auto written=controllers_.writeSettings(result.payload);
                        if(written==PartControllerState::WriteResult::unsupported) ++unsupported_;
                        else if(written==PartControllerState::WriteResult::invalidLength) ++rejectedSysEx_;
                    }
                    else if (result.payload.size() >= 3
                        && (result.payload[0] == 0x41 || result.payload[0] == 0x49) && rhythm_)
                    {
                        const auto written=rhythmSettings_.write(result.payload);
                        if (written == RhythmSettings::WriteResult::unsupported) ++unsupported_;
                        else if (written == RhythmSettings::WriteResult::invalidLength) ++rejectedSysEx_;
                    }
                    else
                    {
                        applySystemSettings(result.payload);
                    }
                    // A later unsupported table record does not roll back an
                    // earlier master write in the same valid transaction.
                    refreshControls();
                }
            }
            else if (result.status == Status::gmOn && midiInput_.receiveReset) requestReset();
            // 04:1dc7 clears only the transient GM-reset request bit, not
            // voices/settings. Reset blocks later MIDI until0746 has already
            // cleared that bit, so there is no persistent "GM mode" to exit.
            else if (result.status == Status::gmOff) {}
            else if (result.status == Status::badChecksum || result.status == Status::tooLong)
                ++rejectedSysEx_;
            return MidiDispatchResult::accepted;
        }
        return receivePerformanceMessage(event);
    }
    void applySystemSettings(std::span<const uint8_t> payload) noexcept
    {
        const auto changes=system_.write(payload,effectsTables_ ? &*effectsTables_ : nullptr);
        if(changes.effects&1) effects_.reverb.request(system_.effects.reverb);
        if(changes.effects&2) effects_.chorus.request(system_.effects.chorus);
        if(changes.reset && midiInput_.receiveReset) requestReset();
        if(changes.unsupported) ++unsupported_;
        if(changes.invalidLength) ++rejectedSysEx_;
    }
    MidiDispatchResult receivePerformanceMessage(const MidiDecoder::Event& event) noexcept
    {
        const auto kind=event.status&0xf0;
        if(const auto result=receiveVoiceController(event)) return *result;
        if(kind==0xc0 && event.dataSize==1) return receiveProgram(event);
        if(kind==0x80 || kind==0x90) return receiveNote(event);
        return receiveController(event);
    }

    // Parser-owned state, independent of physical voice preparation. This
    // does not issue PCM writes, allocate voices or consume lifecycle requests.
    MidiDispatchResult receiveController(const MidiDecoder::Event& event) noexcept
    {
        const auto kind=event.status&0xf0,channel=event.status&15;
        if (kind==0xb0 && event.first==5) {
            for (unsigned part=16;part-- >0;) if (parts_.routing[part].channel==channel
                && (parts_.routing[part].flags&0x0840)==0x0840) {
                engine_.setPortamentoTime(part,event.second);
            }
            refreshControls();
            return MidiDispatchResult::accepted;
        }
        const bool scalar = parts_.applyScalar(event);
        bool nrpn=false;
        if (kind==0xb0 && event.first==6)
            for (unsigned part=16;part-- > 0;)
            {
                const auto& route=parts_.routing[part];
                auto& controls=parts_.parts[part].controls;
                if (route.channel!=channel || !(route.flags&0x0800) || !controls.nrpnSelected) continue;
                if (!(route.flags&0x8000)) { nrpn=true; continue; }
                if (controls.tone.writeNrpn(controls.nrpnMsb,controls.nrpnLsb,event.second))
                { nrpn=true; continue; }
                if (!(route.noteFlags&0x10) || controls.nrpnLsb>=128) continue;
                const unsigned map=(route.noteFlags&0x40) ? 1 : 0;
                if(rhythm_) nrpn=rhythmSettings_.writeNrpn(map,selectedRhythmProgram_[part],
                    controls.nrpnMsb,controls.nrpnLsb,event.second,*rhythm_) || nrpn;
            }
        if (scalar && kind==0xb0 && event.first==6)
            for (unsigned part=0;part<16;++part)
                if (parts_.routing[part].channel==channel && (parts_.routing[part].flags&0x0801)==0x0801)
                {
                    const auto& controls=parts_.parts[part].controls;
                    if (!controls.nrpnSelected && controls.rpnMsb==0 && controls.rpnLsb==0)
                        controllers_.parts[part].sourceSensitivity[1][0]=uint8_t(64+controls.bendRange);
                }
        const bool contribution = controllers_.receiveControlContributions(event,parts_.routing);
        const bool pressure = controllers_.receivePolyPressure(event,parts_.routing)
            || controllers_.receiveChannelPressure(event,parts_.routing)
            || controllers_.receivePitchBend(event,parts_.routing);
        if (scalar || nrpn) refreshControls();
        bool mappedContribution=contribution && event.first==1;
        if(contribution && event.first<121)
            for(unsigned part=0;part<16;++part)
                if(parts_.routing[part].channel==channel
                    && (controllers_.parts[part].assignedControllers[0]==event.first
                        || controllers_.parts[part].assignedControllers[1]==event.first))
                    mappedContribution=true;
        if (!scalar && !nrpn && !mappedContribution && !pressure) ++unsupported_;
        return MidiDispatchResult::accepted;
    }

    MidiDispatchResult receiveProgram(const MidiDecoder::Event& event) noexcept
    {
        // 04:092e gates incoming instrument changes with CDF6. Panel/DT1
        // edits use their own explicit paths and must not be blocked here.
        if(!midiInput_.receiveProgramChanges) return MidiDispatchResult::accepted;
        std::array<VoiceCommand,16> requests{};
        std::array<std::optional<uint16_t>,16> tones{};
        uint16_t matched=0;
        unsigned count=0;
        for(unsigned part=16;part-- >0;) {
            const auto& route=parts_.routing[part];
            if(route.channel!=(event.status&15) || !(route.flags&0x1000)) continue;
            matched|=uint16_t(1u<<part);
            if(route.noteFlags&0x10) continue;
            tones[part]=resolveMelodicTone(parts_.parts[part].bankSelect,event.first);
            // 04:0a24 only publishes08da when a valid tone actually changes.
            if(tones[part] && tones[part]!=selectedTone_[part])
                requests[count++]=ProgramVoiceRequest{uint8_t(part),*tones[part]};
        }
        if(!engine_.commands.publish(std::span(requests).first(count))) return MidiDispatchResult::deferred;
        for(unsigned part=16;part-- >0;) if(matched&(1u<<part)) {
            auto& settings=parts_.parts[part];
            if(parts_.routing[part].noteFlags&0x10) {
                // 04:0940..0ab3: drum selection edits the shared map in the
                // receiver, without a melodic program-voice command.
                commitProgram(part,event.first,settings.bankSelect);
            } else {
                settings.bank=settings.bankSelect;
                settings.controls.program=event.first;
                selectedTone_[part]=tones[part];
                if(!melodic_ && settings.bank!=0) ++unsupported_;
            }
        }
        refreshControls();
        return MidiDispatchResult::accepted;
    }

    MidiDispatchResult receiveNote(const MidiDecoder::Event& event) noexcept
    {
        std::array<VoiceCommand,16> requests{};
        unsigned count=0,unsupported=0;
        const bool on=(event.status&0xf0)==0x90 && event.second!=0;
        for(unsigned part=16;part-- >0;) {
            const auto& route=parts_.routing[part];
            const NoteReceiveMode audition{uint8_t(soloEnabled_ ? (displayAll_ ? 10 : 8) : 0),
                uint8_t(displayPart_),uint8_t(globalMuted_)};
            if(!AcceptNotePart(event.status&15,part,route,audition)) continue;
            if(!on) {
                requests[count++]=NoteRequest{NoteRequest::Action::off,uint8_t(part),event.first,0,
                    (route.noteFlags&0x10) ? std::nullopt : selectedTone_[part]};
                continue;
            }
            const auto& settings=parts_.parts[part];
            const bool drum=(route.noteFlags&0x10)!=0;
            if(drum && !rhythm_) { ++unsupported; continue; }
            if(drum ? (settings.bank!=0 || rhythmRejected_[part]) : !selectedTone_[part]) continue;
            const auto selected=SelectPartNoteOn(part,event.first,event.second,{route.noteFlags,
                {rhythmSettings_.map(0).flags[event.first],rhythmSettings_.map(1).flags[event.first]},
                settings.velocity,settings.keyRange,0});
            if(selected.status==PartNoteOnSelection::Status::invalidInput) return MidiDispatchResult::failed;
            if(selected.status!=PartNoteOnSelection::Status::prepare) continue;
            requests[count++]=NoteRequest{NoteRequest::Action::on,uint8_t(part),event.first,selected.velocity,
                drum ? std::nullopt : selectedTone_[part]};
        }
        if(!engine_.commands.publish(std::span(requests).first(count))) return MidiDispatchResult::deferred;
        unsupported_+=unsupported;
        return MidiDispatchResult::accepted;
    }

    std::optional<MidiDispatchResult> receiveVoiceController(const MidiDecoder::Event& event) noexcept
    {
        if(event.kind!=MidiDecoder::Kind::message || (event.status&0xf0)!=0xb0) return std::nullopt;
        bool fixed=false;
        switch(event.first) {
        case 0: case 1: case 5: case 7: case 10: case 11:
        case 64: case 65: case 66: case 67: fixed=true; break;
        default: break;
        }
        // Fixed controllers precede the user-selected source CC in the H8
        // parser. A source collision with RPN/channel modes takes the source
        // command path instead. Resolve this once, before publishing parts.
        const bool source=!fixed && event.first==system_.master.portamentoController;
        const bool release=!source && (event.first==120 || (event.first>=123 && event.first<=125));
        const bool reset=!source && event.first==121;
        const bool mode=!source && (event.first==126 || event.first==127);
        if(!source && !release && !reset && !mode && event.first!=64 && event.first!=65 && event.first!=66) return std::nullopt;
        if((release || reset) && event.second!=0) return MidiDispatchResult::accepted;
        if(mode && ((event.first==126 && event.second>16) || (event.first==127 && event.second!=0)))
            return MidiDispatchResult::accepted;
        const uint16_t mask=(release || reset || mode) ? 0 : uint16_t(0x0800 | (source ? 0 : event.first==64 ? 0x20 : event.first==65 ? 0x40 : 0x80));
        std::array<VoiceCommand,16> requests{};
        unsigned count=0;
        for(unsigned part=16;part-- >0;) {
            const auto& route=parts_.routing[part];
            if(route.channel!=(event.status&15) || (route.flags&mask)!=mask) continue;
            if(source) requests[count++]=PortamentoSourceRequest{uint8_t(part),event.second};
            else if(reset) requests[count++]=ControllerResetRequest{uint8_t(part)};
            else if(mode) requests[count++]=PartModeRequest{uint8_t(part),event.first==127};
            else if(release) requests[count++]=PartReleaseRequest{event.first==120
                ? PartReleaseRequest::Kind::sound : PartReleaseRequest::Kind::notes,uint8_t(part)};
            else requests[count++]=PedalRequest{event.first==64 ? PedalRequest::Kind::hold
                : event.first==65 ? PedalRequest::Kind::portamento : PedalRequest::Kind::sostenuto,
                uint8_t(part),event.second>=64};
        }
        if(!engine_.commands.publish(std::span(requests).first(count))) return MidiDispatchResult::deferred;
        // 04:0844 resets the parser's values before queuing00:08ba's pedal
        // release. Reserve the entire batch first: a full queue must not
        // repeatedly reset values while returning deferred to its caller.
        if(reset) {
            for(unsigned i=0;i<count;++i) resetControllerValues(std::get<ControllerResetRequest>(requests[i]).part);
            refreshControls();
        }
        // 00:2837/285e changes the receive-side mode before094b/095d
        // consumes the stop/held-key command. Repeated MIDI mode messages
        // still enqueue a command; GS's unchanged-value policy is separate.
        if(mode)
            for(unsigned i=0;i<count;++i) {
                const auto& request=std::get<PartModeRequest>(requests[i]);
                setPartModeValue(request.part,request.poly);
            }
        return MidiDispatchResult::accepted;
    }

    void serviceVoiceCommand() noexcept
    {
#if defined(SC55_NATIVE_IO_AUDIT)
        if(holdAdmissionsAudit_) return;
#endif
        const auto result=engine_.serviceCommand(voiceConfiguration(),selectedTone_,controllers_,data_,conversion_,waves_,
            [&](uint8_t a) { return read(a); },ControlWriter{pcm_});
        if(result.failed) failed_=true;
        if(result.unsupported) ++unsupported_;
        if(result.program) updateCapacityMode(result.program->part,result.program->tone);
    }

    void serviceMidiInput() noexcept
    {
        if(standby_ || standbyStopPending_ || resetPending() || bulkReply_.active() || parameterReplyWaiting_ || panelCount_) return;
        // Decode packets independently of physical voice readiness. Completed
        // voice-changing transactions retain their barrier at EOX; partial or
        // rejected packets must not stall unrelated controller input.
        for(unsigned event=0;event<64;++event) {
            if(resetPending() || bulkReply_.active() || parameterReplyWaiting_ || bulkInputReset_) break;
            const auto result=queue_.dispatchOne(elapsedCycles_,[&](const auto& message) {
                if(message.kind==MidiDecoder::Kind::receiveRecovery) return MidiDispatchResult::deferred;
                return receive(message,false);
            });
            if(result==MidiDispatchResult::failed) failed_=true;
            if(result!=MidiDispatchResult::accepted) break;
        }
    }

    void service() noexcept
    {
        if(failed()) return;
        // Receive the hardware notification before any task-level wait.
        // A latched IRQ must not block notifications from other PCM voices.
        if(engine_.acceptsPcmBoundary() && PCM_HasVoiceBoundary(pcm_)) {
            const auto slot=unsigned(PCM_TakeVoiceBoundary(pcm_));
            if(!engine_.receivePcmBoundary(slot)) {failed_=true;return;}
            serviceRequested_=true;
        }
        if(serviceRequested_) serviceMidiInput();
        // 5710..573c: after a failed reuse poll, task2 waits for its
        // one-kernel-tick event. Other voice groups may still advance on
        // their own control event; that must not repoll the preparing voice.
        if(engine_.activationWaiting(elapsedCycles_) && !engine_.clock.ready() && !engine_.periodicWorkPending())
            { serviceRequested_=false; return; }
        // Settled voices need no CPU-side work between shared control ticks.
        // Only incoming MIDI, PCM completion or an unfinished transition wakes
        // the controller. PCM itself continues rendering every sample.
        if (!serviceRequested_ && !engine_.clock.ready() && !engine_.runtime.controlPending()
            && !PCM_HasVoiceBoundary(pcm_) && !engine_.pcmBoundaryPending()) return;
        serviceWork();
        serviceRequested_=!engine_.activationWaiting(elapsedCycles_) && (engine_.runtime.startupPending() || tasksPending()
            || engine_.controlEventCaptured() || resetPending()
            || (!bulkReply_.active() && !parameterReplyWaiting_ && (queuedEvents()!=0 || panelCount_!=0))
            || engine_.pcmBoundaryPending() || engine_.partStopsPending() || standbyStopPending_);
    }
    void serviceWork() noexcept
    {
        if (failed()) return;
        const auto load = [&](uint8_t a) { return read(a); };
        const ControlWriter store{pcm_};
        if (engine_.runtime.startupPending()) {
            (void)engine_.serviceActivation(elapsedCycles_,load,store);
            // Key-latch/startup attack protection is a different PCM wait:
            // retain its existing per-pass readiness check, not this timer.
        }
        if(!engine_.serviceRetirements(load,store)) { failed_=true; return; }
        // Ready voice-management commands (task1, 07e8..0850) precede the
        // periodic effects/voice pass (task8). A PCM reuse wait still yields
        // to other voices; this does not add a clock delay or relax key latch.
        serviceCommandWork();
        if(failed()) return;
        const auto boundaries=engine_.servicePcmBoundaries(conversion_,load,store);
        if(!boundaries) {failed_=true;return;}
        pcmBoundaryEvents_+=*boundaries;
        // Reaching the bounded command budget is not a task1 wait. Retain
        // the lower-priority event until runnable commands have drained.
        // Actual reuse/capacity/transaction waits still allow control work.
        if(engine_.commands.size() && !engine_.admissionPending() && !tasksPending()
            && !engine_.runtime.startupPending() && !resetPending()
            && !bulkReply_.active() && !parameterReplyWaiting_
#if defined(SC55_NATIVE_IO_AUDIT)
            && !holdAdmissionsAudit_
#endif
            ) return;
        // Run the semantic control update on its common event. H8 instruction
        // budgets are diagnostic data, not delays in the native sound engine.
        // Actual PCM activation/reuse waits remain owned by serviceActivation.
        const auto control = engine_.updateControl(controllers_,data_,conversion_,waves_,load,store,
            effectsTables_.has_value(),[&] { return serviceEffects(); });
        if (control.status == VoiceControlRuntime::ScheduledStatus::failed) { failed_ = true; return; }
        if (resetPending()) { serviceReset(); return; }
    }

    void serviceCommandWork() noexcept
    {
        if (resetPending() || bulkReply_.active() || parameterReplyWaiting_) return;
        for (unsigned event = 0; event < 64 && !failed(); ++event)
        {
            if (resetPending() || bulkReply_.active() || parameterReplyWaiting_) break;
            serviceVoiceCommand();
            if (engine_.admissionPending() || tasksPending() || engine_.runtime.startupPending()) break;
            if (engine_.commands.size()) continue;
            if(standbyStopPending_) {
                // Standby uses the ordinary controller reset and group stop
                // after earlier admissions finish; PCM still drains its ramps.
                for(unsigned part=16;part-- >0;)
                    if(!resetPart(part,false) || !stopPartGroups(part)) {failed_=true;return;}
                refreshControls();
                standbyStopPending_=false;
            }
            if(standby_) break;
            // Value edits may reset controllers/release voices. Apply them
            // only between complete admissions, never inside PCM activation.
            // This queue is owned and consumed on the audio thread.
            while(panelCount_ && !failed()) {
                const auto edit=panelEdits_[panelHead_];
                panelHead_=(panelHead_+1)%panelEdits_.size();--panelCount_;
                if(edit.all) (void)applyMasterAdjustment(edit.parameter,edit.delta);
                else (void)applyPartAdjustment(edit.part,edit.parameter,edit.delta);
            }
            if(failed()) break;
            const auto result = engine_.serviceMidi(queue_,elapsedCycles_,
                [&](const auto& message) { return receive(message); });
            if(bulkInputReset_) {
                // 04:2715 clears RX bytes/decoder state/active-sensing flags.
                // Do this only after dispatch returns, never inside its sink.
                queue_.discardReceived(); sysex_={}; receiveState_={};
                bulkInputReset_=false;
                break;
            }
            if(receiveRecovery_) {
                // 04:08b8 resets controllers, then command18 (00:08ce)
                // stops/reclaims all groups via 168d before releasing pedals.
                // Configuration survives; PCM stop ramps are not bypassed.
                for(unsigned part=16;part-- > 0;)
                    if(!resetPart(part,false)) { failed_=true; return; }
                for(unsigned part=16;part-- > 0;)
                    if(!stopPartGroups(part)) { failed_=true; return; }
                refreshControls();
                queue_.discardReceived(); sysex_={};
                receiveRecovery_=false;
                ++completedReceiveRecoveries_;
                break;
            }
            if (result != MidiDispatchResult::accepted) break;
        }
    }
    const SoundData& data_;
    struct PanelEdit { PartParameter parameter; int delta; uint8_t part; bool all; };
    bool enqueuePanelEdit(PanelEdit edit) noexcept
    {
        if(failed() || panelCount_==panelEdits_.size()) return false;
        panelEdits_[(panelHead_+panelCount_)%panelEdits_.size()]=edit;
        ++panelCount_;serviceRequested_=true;return true;
    }
    std::array<PanelEdit,64> panelEdits_{};
    std::size_t panelHead_=0,panelCount_=0;
    bool serviceRequested_=true;
#if defined(SC55_NATIVE_IO_AUDIT)
    bool externalControlClockAudit_=false;
    bool holdAdmissionsAudit_=false;
#endif
    uint64_t elapsedCycles_=0;
    bool globalMuted_=false;
    bool standby_=false,standbyStopPending_=false;
    bool fastDisplayScroll_=false;
    bool soloEnabled_=false;
    pcm_t& pcm_;
    NativeVoiceEngine engine_;
    MidiEventQueue<2048> queue_;
    MidiReceiveState receiveState_;
    MidiReceiveTimer receiveTimer_;
    uint32_t receiveTimerPhase_=0;
    bool receiveRecovery_=false;
    uint64_t completedReceiveRecoveries_=0;
    // Shared melodic/rhythm preparation key (A1B4), read by CC84 before the
    // next note installs its own reference. Serialized with all note fanout.
    PartSettings parts_;
    std::optional<SystemDefaults> defaults_;
    std::optional<EffectsTables> effectsTables_;
    EffectsControl effects_;
    std::optional<RhythmPresetTable> rhythm_;
    std::optional<MelodicPresetTable> melodic_;
    std::array<std::optional<uint16_t>,16> selectedTone_{};
    RhythmSettings rhythmSettings_;
    std::array<bool,16> rhythmRejected_{};
    std::array<uint8_t,16> selectedRhythmProgram_{};
    std::array<uint8_t,2> rhythmMapPrograms_{}; // AC10/11, raw shared-map program latches
    SysExReceiver sysex_;
    MidiInputSettings midiInput_;
    DisplayData display_;
    DisplayEventLog displayEvents_;
    unsigned displayPart_=1;
    bool displayAll_=false;
    SystemSettings system_;
    UninterpretedSystemSettings transferSettings_;
    std::array<ParameterReply,16> replies_{};
    unsigned replyHead_=0,replyCount_=0;
    bool parameterOutputConnected_=false,parameterReplyWaiting_=false,parameterReplyTransmitting_=false;
    bool parameterReplyCapture_=false;
    uint64_t droppedReplies_=0;
    BulkReplyTransfer bulkReply_;
    bool bulkOutputConnected_=false;
    bool bulkInputReset_=false;
    PartControllerState controllers_;
    PitchConversion conversion_;
    LfoWaveformTables waves_;
    bool failed_ = false;
    bool resetRequested_ = false;
    uint64_t completedResets_ = 0;
    uint64_t unsupported_ = 0;
    uint64_t pcmBoundaryEvents_ = 0;
    uint64_t rejectedSysEx_ = 0;
};
}
