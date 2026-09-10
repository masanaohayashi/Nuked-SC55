#pragma once
#include "sc55_voice_engine.h"
#include "pcm.h"
#include "sc55_sysex.h"
#include "sc55_part_settings.h"
#include "sc55_rhythm_presets.h"
#include "sc55_system_defaults.h"
#include "sc55_effects_control.h"
#include "sc55_synth_state.h"
#include "sc55_midi_receive.h"
#include "sc55_parameter_reply.h"
#include "sc55_bulk_reply.h"
#include "sc55_display.h"
#include "sc55_voice_commands.h"

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
        master_ = defaults.master();
        transferSettings_.reset(defaults.bytes);
        std::copy_n(defaults.bytes.begin()+8,16,systemName_.begin());
        std::copy_n(defaults.bytes.begin()+0x18,16,capacityPolicy_.reserves.begin());
        capacityPolicy_.startPartControl=defaults.bytes[0x28];
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
            rhythmRecords_.fill(rhythm_->records[rhythm_->programs[*program]]);
            rhythmMaps_.fill(RhythmPresetTable::decode(rhythmRecords_[0]));
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
    bool partMuted(unsigned part) const noexcept
    { return part<16 && !(parts_.routing[part].flags&0x0200); }
    bool globallyMuted() const noexcept { return globalMuted_; }
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
        case PartParameter::volume: master_.volume=adjusted(master_.volume); break;
        case PartParameter::pan: master_.pan=adjusted(master_.pan,1); break;
        case PartParameter::keyShift: master_.keyShift=adjusted(master_.keyShift,40,88); break;
        case PartParameter::reverb:
            if(!effectsTables_) return false;
            effectSettings_.reverb[2]=adjusted(effectSettings_.reverb[2]);
            effects_.reverb.request(effectSettings_.reverb);
            serviceRequested_=true; return true;
        case PartParameter::chorus:
            if(!effectsTables_) return false;
            effectSettings_.chorus[1]=adjusted(effectSettings_.chorus[1]);
            effects_.chorus.request(effectSettings_.chorus);
            serviceRequested_=true; return true;
        default: return false;
        }
        refreshControls(); serviceRequested_=true;
        return true;
    }
public:
    void toggleMute(unsigned part,bool all) noexcept
    {
        if(part>=16 || failed()) return;
        if(all) {
            globalMuted_=!globalMuted_;
            if(globalMuted_) muteStops_|=0xffff;
        } else {
            parts_.routing[part].flags^=0x0200;
            if(partMuted(part)) muteStops_|=uint16_t(1u<<part);
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
    { return part<16 && (reuseInvalidation_&(1u<<part)); }
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
    uint16_t partEnvelopeLevel(unsigned part) const noexcept
    {
        unsigned level=0;
        const auto active=pcm_.voice_mask&pcm_.voice_mask_pending;
        for(unsigned slot=0;slot<24;++slot)
            if(((active>>slot)&1) && engine_.installation.voices[slot].input.part==part) {
                const auto gains=PCM_PeekVoiceGainLevels(pcm_,slot);
                level=std::max(level,unsigned(gains[0])+gains[1]);
            }
        return uint16_t(std::min(level,65535u));
    }
    const MasterControls& masterControls() const noexcept { return master_; }
    // Audio-owner view of GS bulk48 settings. No H8 RAM or cached duplicate
    // of live part/controller values. Invalid offsets are explicitly absent.
    std::optional<uint8_t> readSystemConfiguration(unsigned offset) const noexcept
    {
        if(offset>=0x748) return {};
        if(const auto value=transferSettings_.read(offset)) return value;
        if(offset<0x48) {
            if(offset==0) return uint8_t(master_.tune>>8);
            if(offset==1) return uint8_t(master_.tune);
            if(offset==2) return master_.volume;
            if(offset==3) return master_.portamentoController;
            if(offset==5) return master_.keyShift;
            if(offset==6) return master_.pan;
            if(offset>=8 && offset<0x18) return systemName_[offset-8];
            if(offset>=0x18 && offset<0x28) return capacityPolicy_.reserves[offset-0x18];
            if(offset==0x28) return capacityPolicy_.startPartControl;
            if(offset==0x2a) return effectSettings_.reverbMacro;
            if(offset>=0x2b && offset<0x31) return effectSettings_.reverb[offset-0x2b];
            if(offset==0x32) return effectSettings_.chorusMacro;
            if(offset>=0x33 && offset<0x3a) return effectSettings_.chorus[offset-0x33];
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
            || replyCount_ || queuedEvents() || panelCount_ || engine_.admission || tasksPending()
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
            if(region==0x49) return rhythmRecords_[map][offset];
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
    const VoiceCapacityPolicy& capacityPolicy() const noexcept { return capacityPolicy_; }
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
            || engine_.runtime.controlPending() || tasksPending() || effectPassClock_) return false;
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
    const PartControllerState& controllerSettingsAudit() const noexcept { return controllers_; }
    const VoiceControlState* voiceControlAudit(unsigned slot) const noexcept
    { return slot<24 && engine_.runtime.voices[slot] ? &*engine_.runtime.voices[slot] : nullptr; }
#endif
    const EffectsSettings& effectsSettings() const noexcept { return effectSettings_; }
    const DisplayData& displayData() const noexcept { return display_; }
    // Normal play screen: instrument/part keys cancel text; ALL also
    // cancels the bitmap (04:3a10..3a6a). Other value edits preserve it.
    void cancelDisplayMessages(bool bitmap) noexcept
    {
        displayControl_.cancelText(); displayTextVisible_=false;
        if(bitmap) displayControl_.cancelBitmap();
    }
    void selectDisplayPart(unsigned part,bool all) noexcept
    { displayPart_=std::min(part,15u); displayAll_=all; }
    const DisplayControl::Line& displayLine() const noexcept {return displayLine_;}
    bool displayTextVisible() const noexcept {return displayTextVisible_;}
    const DisplayControl& displayControl() const noexcept {return displayControl_;}
    uint64_t rejectedSysEx() const noexcept { return rejectedSysEx_; }
    bool resetPending() const noexcept { return resetPhase_ != ResetPhase::idle; }
    uint64_t completedResets() const noexcept { return completedResets_; }
    uint64_t completedReceiveRecoveries() const noexcept { return completedReceiveRecoveries_; }
    bool effectsSettled() const noexcept
    { return !effectsTables_ || (effects_.reverb.phase==0 && effects_.chorus.phase==0); }
    const PartSettings& partSettings() const noexcept { return parts_; }
    const std::array<RhythmPresetTable::Record,2>& rhythmRecords() const noexcept { return rhythmRecords_; }
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
                std::copy_n(rhythmRecords_[map].begin()+0x480,name.size(),name.begin());
            }
        } else if(selectedTone_[part]) {
            if(const auto* patch=data_.patch(*selectedTone_[part]))
                std::copy_n(patch->name.begin(),std::min(name.size(),patch->name.size()),name.begin());
        }
        return name;
    }
    std::size_t queuedEvents() const noexcept
    { return queue_.size() + engine_.commands.size() + (engine_.admission ? 1 : 0); }
    std::size_t push(std::span<const uint8_t> bytes) noexcept
    {
        if (failed()) return 0;
        // Normal bulk disables RX, then04:2738 drains pending UART bytes
        // before restoring interrupts. Transfer-time input is not replayed.
        if(bulkReply_.active()) return bytes.size();
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
        if(displayControl_.textActive() || displayControl_.bitmapActive() || displayTextVisible_) {
            next=std::min(next,uint32_t(displayTimerCycles-elapsedCycles_%displayTimerCycles));
            next=std::min(next,uint32_t(displayServiceCycles-elapsedCycles_%displayServiceCycles));
        }
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
        if(displayControl_.textActive() || displayControl_.bitmapActive() || displayTextVisible_
            || displayControl_.scrollTicks()) {
            displayControl_.timerTicks(unsigned(elapsedCycles_/displayTimerCycles-previousCycles/displayTimerCycles));
            if(elapsedCycles_/displayServiceCycles!=previousCycles/displayServiceCycles) {
                displayTextVisible_=displayControl_.textActive();
                if(displayTextVisible_) displayLine_=displayControl_.service(normalDisplayLine());
            }
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
    DisplayControl::Line normalDisplayLine() const noexcept
    {
        DisplayControl::Line line; line.fill(' ');
        if(displayAll_) {std::copy_n("ALL PARTS",9,line.begin()+4);return line;}
        const auto& part=parts_.parts[displayPart_];
        const unsigned program=unsigned(part.controls.program)+1;
        line[0]=program>=100 ? uint8_t('0'+program/100) : uint8_t(' ');
        line[1]=program>=10 ? uint8_t('0'+program/10%10) : uint8_t(' ');
        line[2]=uint8_t('0'+program%10);
        const auto name=instrumentName(displayPart_);
        std::copy(name.begin(),name.end(),line.begin()+4);
        return line;
    }
    void seedPitchHistory(unsigned part) noexcept
    { seedPitchHistory(part,selectedTone_[part]); }
    void seedPitchHistory(unsigned part,std::optional<uint16_t> tone) noexcept
    {
        if(!tone) return;
        const auto* patch=data_.patch(*tone);
        if(!patch) return;
        const auto& settings=parts_.parts[part];
        const auto key=TransposeMasterKey(TransposePartKey(60,settings.keyShift,
            uint8_t(settings.controls.coarseTuning)),master_.keyShift);
        for(unsigned p=0;p<2;++p) if(patch->partial[p].used)
            engine_.preparation.partKeys[part][p]=TransposePartialKey(key,patch->partial[p].raw[10]);
    }
    bool stopPartGroups(unsigned part) noexcept
    {
        return engine_.stopPartGroups(part,[&](uint8_t a) { return read(a); },ControlWriter{pcm_});
    }
    enum class ResetPhase { idle, requested, draining };
    void setPartModeValue(unsigned part,bool poly) noexcept
    {
        if(poly) parts_.routing[part].noteFlags|=0x80;
        else parts_.routing[part].noteFlags&=0x7f;
    }
    bool applyPartMode(unsigned part,bool poly) noexcept
    {
        if(!stopPartGroups(part)) return false;
        if(!poly) engine_.mono[part].held={};
        engine_.mono[part].current=60; engine_.mono[part].tone.reset();
        reuseInvalidation_&=uint16_t(~(1u<<part));
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
        std::copy_n(defaults_->bytes.begin()+0x2b,6,effectSettings_.reverb.begin());
        std::copy_n(defaults_->bytes.begin()+0x33,7,effectSettings_.chorus.begin());
        effectSettings_.reverbMacro=defaults_->bytes[0x2a];
        effectSettings_.chorusMacro=defaults_->bytes[0x32];
        // Boot/reset must configure even a default matching the zero cache.
        effects_.reverb.phase=2; effects_.chorus.phase=2;
        effects_.reverb.request(effectSettings_.reverb); effects_.chorus.request(effectSettings_.chorus);
    }
    void storeBulkSystemByte(unsigned offset,uint8_t value) noexcept
    {
        if(transferSettings_.write(offset,value)) return;
        if(offset<0x48) {
            if(offset==0) master_.tune=uint16_t((master_.tune&255)|(unsigned(value)<<8));
            else if(offset==1) master_.tune=uint16_t((master_.tune&0xff00)|value);
            else if(offset==2) master_.volume=value;
            else if(offset==3) master_.portamentoController=value;
            else if(offset==5) master_.keyShift=value;
            else if(offset==6) master_.pan=value;
            else if(offset>=8 && offset<0x18) systemName_[offset-8]=value;
            else if(offset>=0x18 && offset<0x28) capacityPolicy_.reserves[offset-0x18]=value;
            else if(offset==0x28) capacityPolicy_.startPartControl=value;
            else if(offset==0x2a) effectSettings_.reverbMacro=value;
            else if(offset>=0x2b && offset<0x31) effectSettings_.reverb[offset-0x2b]=value;
            else if(offset==0x32) effectSettings_.chorusMacro=value;
            else if(offset>=0x33 && offset<0x3a) effectSettings_.chorus[offset-0x33]=value;
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
        if(!effects_.advance(*effectsTables_,effectSettings_.reverb[0],word)) {
            failed_=true; return false;
        }
        return true;
    }

    void requestReset() noexcept
    {
        if (!defaults_ || !rhythm_ || !melodic_) { ++unsupported_; return; }
        resetPhase_ = ResetPhase::requested;
    }
    // Runs after normal stop-task/periodic service, never instead of PCM time
    // progression. Later MIDI remains owned by queue_ until this completes.
    void serviceReset() noexcept
    {
        const auto load = [&](uint8_t a) { return read(a); };
        const auto store = ControlWriter{pcm_};
        if (resetPhase_ == ResetPhase::requested)
        {
            if (engine_.runtime.startupPending() || tasksPending()) return;
            for (unsigned slot=0;slot<24;++slot)
                if (engine_.runtime.voices[slot] && !(engine_.notes.allocator.allocations[slot].status&0x80))
                    if (engine_.requestStop(slot,load,store) != NativeVoiceEngine::StopRequest::queued)
                    { failed_=true; return; }
            for (unsigned part=0;part<16;++part)
                if (!resetPart(part,false)) { failed_=true; return; }
            resetPhase_=ResetPhase::draining;
            return;
        }
        if (tasksPending() || freeVoices()!=24) return;
        for (unsigned slot=0;slot<24;++slot)
            if (engine_.runtime.voices[slot])
            {
                const auto ready=PollVoiceReuse(uint8_t(slot),engine_.lifecycle[slot].fieldCAF4,load,store);
                if (!ready || *ready==VoiceReuseReadiness::cancelled) { failed_=true; return; }
                if (*ready!=VoiceReuseReadiness::ready) return;
            }
        // Logical free slots alone are not enough: retain old DSP owners until
        // the PCM envelopes above have actually reached reuse readiness.
        const auto clock=engine_.clock;
        const auto enabled=engine_.mask.enabled;
        const auto retainedKeys=engine_.preparation.partKeys;
        const auto retainedReference=engine_.preparation.reference;
        // Reset rebuilds the sounding owners, not already received commands.
        const auto commands=engine_.commands;
        const auto admission=engine_.admission;
        engine_=NativeVoiceEngine{};
        engine_.commands=commands; engine_.admission=admission;
        engine_.clock=clock; engine_.mask.enabled=enabled;
        engine_.preparation.partKeys=retainedKeys;
        engine_.preparation.reference=retainedReference;
        if (!engine_.notes.allocator.initializeTables()) { failed_=true; return; }
        for (unsigned slot=0;slot<24;++slot)
            engine_.runtime.first[slot].firstStage=engine_.runtime.second[slot].firstStage=22;
        parts_=defaults_->parts(true); master_=defaults_->master(); controllers_=defaults_->controllers();
        transferSettings_.reset(defaults_->bytes);
        displayControl_.cancelText(); displayControl_.cancelBitmap(); displayTextVisible_=false;
        std::copy_n(defaults_->bytes.begin()+8,16,systemName_.begin());
        std::copy_n(defaults_->bytes.begin()+0x18,16,capacityPolicy_.reserves.begin());
        capacityPolicy_.startPartControl=defaults_->bytes[0x28];
        rhythmRejected_={}; selectedRhythmProgram_={}; rhythmMapPrograms_={};
        reuseInvalidation_=0;
        const auto program=rhythm_->resolve(0);
        if (!program) { failed_=true; return; }
        rhythmRecords_.fill(rhythm_->records[rhythm_->programs[*program]]);
        rhythmMaps_.fill(RhythmPresetTable::decode(rhythmRecords_[0]));
        for (unsigned part=0;part<16;++part)
        { selectMelodicTone(part); seedPitchHistory(part); }
        // 04:12b8/12be posts task8 effect requests, then 04:379f completes
        // reset to the MIDI task. FX ramps continue independently; waiting
        // for them here incorrectly stalls all subsequent MIDI, even RQ1.
        if (effectsTables_) requestDefaultEffects();
        resetPhase_=ResetPhase::idle; ++completedResets_;
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
                capacityPolicy_.modes[part]=(patch->common[1]&1) ? 2 : 0;
    }
    void applyProgramVoiceState(unsigned part,uint16_t tone) noexcept
    {
        updateCapacityMode(part,tone);
        reuseInvalidation_|=uint16_t(1u<<part);
        seedPitchHistory(part,tone);
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
        rhythmRecords_[map] = rhythm_->records[rhythm_->programs[*resolved]];
        rhythmMaps_[map] = RhythmPresetTable::decode(rhythmRecords_[map]);
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
    {
        for (const auto& state : engine_.lifecycle)
            if (state.fieldCAF4 != 0) return true;
        return false;
    }
    void applyMaster(VoiceControlInputs& input) const noexcept
    {
        input.level.master = master_.volume;
        input.spatial.masterPan = master_.pan;
        input.pitch.masterTune = master_.tune;
    }
    void applyPart(unsigned part,VoiceControlInputs& input,unsigned map = 255,unsigned key = 0) const noexcept
    {
        applyMaster(input);
        input.glideRate=engine_.mono[part].glideRate;
        ApplyChannelOutputControls(parts_.parts[part].controls,input.level,input.spatial);
        input.pitch.partTune=uint16_t(parts_.parts[part].controls.finePitch());
        input.correctionSource=parts_.parts[part].fineTune;
        const auto& tone=parts_.parts[part].controls.tone.values;
        input.amplitude={tone[4],tone[5],tone[6]};
        input.secondTiming.attack=tone[4]; input.secondTiming.decay=tone[5]; input.secondTiming.release=tone[6];
        input.second.control=tone[2]; input.secondController=tone[3];
        input.level.has_tone_scale = input.spatial.hasToneScale = map < 2;
        if (map < 2)
        {
            const auto& record = rhythmRecords_[map];
            input.level.tone_scale = record[0x100+key];
            input.spatial.panScale = record[0x280+key];
            input.spatial.reverbScale = record[0x380+key];
            input.spatial.chorusScale = record[0x300+key];
        }
        if (!effectsTables_) input.spatial.reverb = input.spatial.chorus = 0;
    }
    void refreshControls() noexcept
    {
        for (unsigned slot = 0; slot < 24; ++slot)
            if (engine_.runtime.voices[slot]) {
                applyPart(engine_.installation.voices[slot].input.part,engine_.runtime.inputs[slot],
                    engine_.preparation.drumMap[slot],engine_.preparation.drumKey[slot]);
                applyToneModulation(engine_.installation.voices[slot].input.part,engine_.runtime.firstInputs[slot]);
            }
    }
    void applyToneModulation(unsigned part,FirstModulationInputs& input) const noexcept
    {
        const auto& tone=parts_.parts[part].controls.tone.values;
        input.rateControl=tone[0]; input.depthControl=tone[1]; input.delayControl=tone[7];
    }
    bool resetVoiceControllers(unsigned part,bool allNotes) noexcept
    {
        if (!engine_.releasePart(part,(parts_.routing[part].noteFlags&0x10)!=0,true,allNotes))
        { failed_ = true; return false; }
        engine_.mono[part].portamento=false;
        engine_.mono[part].source=255;
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
    static bool requiresVoiceTransaction(const SysExReceiver::Result& packet) noexcept
    {
        using Status=SysExReceiver::Status;
        if(packet.status==Status::gmOn) return true;
        if(packet.status!=Status::roland || packet.model!=0x42) return false;
        // Transfer/reset still own voice-side work.
        // Keep their ordering until those producers use VoiceCommands too.
        // Master/controller/FX values, reserve tables, drum maps and model45
        // display data are receiver-owned; they do not prepare/stop voices.
        if(packet.command==0x11) return true;
        if(packet.command!=0x12 || packet.payload.empty()) return false;
        const auto address=packet.payload;
        if(address[0]!=0x40 || address.size()<2) return false;
        return address[1]==0 && address.size()>=3 && address[2]==0x7f;
    }
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
            effects_.reverb.request(effectSettings_.reverb);
            effects_.chorus.request(effectSettings_.chorus);
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
        // Plan with the same decoder, preserving its valid-prefix semantics,
        // and reserve the whole command batch before changing receiver state.
        auto preview=parts_;
        const auto part=unsigned(payload[1]&15);
        auto assigned=controllers_.parts[part].assignedControllers;
        std::array<VoiceCommand,3> commands{};
        unsigned count=0;
        const auto planned=preview.write(payload,
            [&](unsigned index) {
                commands[count++]=ControllerResetRequest{uint8_t(index)};
                commands[count++]=PartReleaseRequest{PartReleaseRequest::Kind::notes,uint8_t(index)};
                return true;
            },
            [&](unsigned index,uint8_t bank,uint8_t program) {
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
                    return true;
                }
                return address==0x15 && rhythm_.has_value();
            });
        if(!engine_.commands.publish(std::span(commands).first(count))) return MidiDispatchResult::deferred;
        const auto written=parts_.write(payload,
            [&](unsigned index) { resetControllerValues(index); return true; },
            [&](unsigned index,uint8_t bank,uint8_t program) {
                auto& settings=parts_.parts[index];
                settings.bank=settings.bankSelect=bank;
                settings.controls.program=program;
                if(parts_.routing[index].noteFlags&0x10) commitProgram(index,program,bank);
                else {
                    selectedTone_[index]=resolveMelodicTone(bank,program);
                    if(!melodic_ && bank!=0) ++unsupported_;
                }
                return true;
            },&controllers_.parts[part].assignedControllers,
            [&](unsigned index,uint8_t address,uint8_t value) {
                if(address==0x13) {setPartModeValue(index,value!=0);return true;}
                return address==0x15 && changeRhythmMode(index,value);
            });
        if(written!=planned) {failed_=true;return MidiDispatchResult::failed;}
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
                const auto packet=preview.receive(event);
                if(packet.status==SysExReceiver::Status::roland && packet.model==0x42 && packet.command==0x12
                    && packet.payload.size()>=3 && packet.payload[0]==0x48) {
                    const auto result=receiveBulkSettings(packet.payload);
                    if(result==MidiDispatchResult::accepted) (void)sysex_.receive(event);
                    return result;
                }
                if(packet.status==SysExReceiver::Status::roland && packet.model==0x42 && packet.command==0x12
                    && packet.payload.size()>=2 && packet.payload[0]==0x40 && (packet.payload[1]&0xf0)==0x10) {
                    const auto result=receivePartSettings(packet.payload);
                    if(result==MidiDispatchResult::accepted) (void)sysex_.receive(event);
                    return result;
                }
                if((!allowVoiceTransactions || engine_.admission || engine_.commands.size() || tasksPending()
                    || engine_.runtime.startupPending()) && requiresVoiceTransaction(packet))
                    return MidiDispatchResult::deferred;
            }
            const auto result = sysex_.receive(event);
            using Status = SysExReceiver::Status;
            if (result.status == Status::roland)
            {
                if(result.model==0x45 && result.command==0x12) {
                    if(display_.write(result.payload)!=DisplayData::WriteResult::applied)
                        ++rejectedSysEx_;
                    else displayControl_.receive(display_);
                }
                else if(result.model==0x42 && result.command==0x11) {
                    if(result.payload.size()>=1 && (result.payload[0]==0x48 || result.payload[0]==0x49)) {
                        if(!bulkOutputConnected_ || (result.payload[0]==0x49 && !rhythm_)) ++unsupported_;
                        else if(bulkReply_.begin(result.payload)!=BulkReplyTransfer::BeginResult::accepted) ++rejectedSysEx_;
                        else prepareBulkTransmission();
                        return MidiDispatchResult::accepted;
                    }
                    ParameterReply reply;
                    auto status=reply.prepare(result.payload,master_,parts_,controllers_);
                    if(defaults_ && result.payload.size()>=3 && result.payload[0]==0x40
                        && (result.payload[1]&0xf0)==0x30)
                        status=reply.prepareInformation(result.payload,defaults_->identity,
                            [&](uint32_t address) {return PCM_ReadROM(pcm_,address);});
                    if(status==ParameterReply::Result::unsupported)
                        status=reply.prepareSystem(result.payload,systemName_,capacityPolicy_,effectSettings_,
                            rhythm_ ? std::span<const RhythmPresetTable::Record>(rhythmRecords_) : std::span<const RhythmPresetTable::Record>{},
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
                    if (result.payload.size() >= 4 && result.payload[0]==0x40
                        && result.payload[1]==0 && result.payload[2]==0x7f && result.payload[3]==0)
                        requestReset();
                    else if(result.payload.size()>=3 && result.payload[0]==0x40
                        && result.payload[1]==1 && result.payload[2]==0)
                    {
                        if(result.payload.size()!=19) ++rejectedSysEx_;
                        else for(unsigned i=0;i<16;++i) systemName_[i]=std::max<uint8_t>(32,result.payload[i+3]);
                    }
                    else if(result.payload.size()>=3 && result.payload[0]==0x40
                        && result.payload[1]==1 && result.payload[2]==0x10)
                    {
                        // v1.21 kind0 handler: one complete sixteen-part table,
                        // accepted atomically only when the total fits24 voices.
                        unsigned total=0;
                        for(unsigned i=3;i<result.payload.size();++i) total+=result.payload[i];
                        if(result.payload.size()!=19 || total>24) ++rejectedSysEx_;
                        else std::copy_n(result.payload.begin()+3,16,capacityPolicy_.reserves.begin());
                    }
                    else if(result.payload.size()>=3 && result.payload[0]==0x40
                        && result.payload[1]==1 && result.payload[2]==0x20)
                    {
                        if(result.payload.size()<4) ++rejectedSysEx_;
                        else capacityPolicy_.startPartControl=std::min<uint8_t>(result.payload[3],15);
                    }
                    else if (result.payload.size()>=3 && result.payload[0]==0x40
                        && result.payload[1]==1 && effectsTables_)
                    {
                        const auto changed=effectSettings_.write(result.payload,*effectsTables_);
                        if (changed.requests&1) effects_.reverb.request(effectSettings_.reverb);
                        if (changed.requests&2) effects_.chorus.request(effectSettings_.chorus);
                        if (changed.unsupported) ++unsupported_;
                        if (changed.invalidLength) ++rejectedSysEx_;
                    }
                    else if(result.payload.size()>=2 && result.payload[0]==0x40
                        && (result.payload[1]&0xf0)==0x20) {
                        const auto written=controllers_.writeSettings(result.payload);
                        if(written==PartControllerState::WriteResult::unsupported) ++unsupported_;
                        else if(written==PartControllerState::WriteResult::invalidLength) ++rejectedSysEx_;
                    }
                    else if(result.payload.size()>=3 && result.payload[0]==0x49 && rhythm_) {
                        const auto written=RhythmPresetTable::writeBulk(rhythmRecords_,result.payload);
                        if(written.status==RhythmPresetTable::WriteResult::applied)
                            rhythmMaps_[written.map]=RhythmPresetTable::decode(rhythmRecords_[written.map]);
                        else if(written.status==RhythmPresetTable::WriteResult::unsupported) ++unsupported_;
                        else ++rejectedSysEx_;
                    }
                    else if (result.payload.size() >= 3 && result.payload[0] == 0x41 && rhythm_)
                    {
                        const unsigned map = result.payload[1]>>4;
                        if (map >= 2) ++unsupported_;
                        else
                        {
                            const auto written = RhythmPresetTable::write(rhythmRecords_[map],
                                result.payload[1]&15,result.payload[2],result.payload.subspan(3));
                            if (written == RhythmPresetTable::WriteResult::unsupported) ++unsupported_;
                            else if (written == RhythmPresetTable::WriteResult::invalidLength) ++rejectedSysEx_;
                            else rhythmMaps_[map] = RhythmPresetTable::decode(rhythmRecords_[map]);
                        }
                    }
                    else
                    {
                        const auto written = master_.write(result.payload);
                        if (written == MasterControls::WriteResult::unsupported) ++unsupported_;
                        else if (written == MasterControls::WriteResult::invalidLength) ++rejectedSysEx_;
                    }
                    // A later unsupported table record does not roll back an
                    // earlier master write in the same valid transaction.
                    refreshControls();
                }
            }
            else if (result.status == Status::gmOn) requestReset();
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
                engine_.mono[part].glideRate=event.second;
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
                if (controls.nrpnMsb==0x18 && rhythm_) {
                    nrpn=rhythm_->writeRelativePitch(rhythmRecords_[map],selectedRhythmProgram_[part],controls.nrpnLsb,event.second) || nrpn;
                    rhythmMaps_[map].pitches[controls.nrpnLsb]=rhythmRecords_[map][0x180+controls.nrpnLsb];
                    continue;
                }
                const auto offset=RhythmPresetTable::nrpnOutputOffset(controls.nrpnMsb,controls.nrpnLsb);
                if (!offset || !rhythm_) continue;
                rhythmRecords_[map][*offset]=event.second;
                nrpn=true;
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
            if(!AcceptNotePart(event.status&15,part,route,{0,0,uint8_t(globalMuted_)})) continue;
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
                {rhythmMaps_[0].flags[event.first],rhythmMaps_[1].flags[event.first]},
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
        const bool source=!fixed && event.first==master_.portamentoController;
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

    void startMonoOrSource(MidiDecoder::Event event,unsigned part,bool polySource=false) noexcept
    {
        auto& mono=engine_.mono[part];
        const auto& settings=parts_.parts[part];
        const auto tone=engine_.admission->request.tone;
        if (!tone) { engine_.admission.reset(); return; }
        const auto selection=PrepareMappedNoteVelocity(event,*tone,settings.controls.softPedal,0,data_);
        if (!selection) { failed_=true; return; }
        if (!selection->partials.candidates.count) {
            if(!polySource && !engine_.admission->isHeldReturn()) mono.held.set(event.first,true);
            if (!engine_.admission->isHeldReturn()) mono.source=255;
            if (engine_.admission->isHeldReturn() && engine_.notes.allocator.partHead[part]<24) {
                if (!engine_.notes.allocator.releaseMonoGroup(part)
                    || !engine_.runtime.publishNoteReleases(engine_.notes.allocator)) failed_=true;
            }
            engine_.admission.reset(); return;
        }
        auto& allocator=engine_.notes.allocator;
        uint8_t group=allocator.partHead[part];
        if(polySource) {
            const auto found=engine_.admission->sourceReuse==PendingAdmission::SourceReuse::fresh ? std::optional<uint8_t>(255)
                : allocator.findSourceGroup(part,mono.source);
            if(!found) { failed_=true; return; }
            group=*found;
        }
        if (reuseInvalidated(part) && (!polySource || group<24)) {
            reuseInvalidation_&=uint16_t(~(1u<<part));
            if(polySource && group<24) {
                if(!engine_.stopGroup(part,group,
                    [&](uint8_t a) { return read(a); },ControlWriter{pcm_})) failed_=true;
                // H8 jumps straight to fresh admission after invalidating
                // this source. Do not search and steal another older source
                // group when the native continuation resumes after PCM work.
                engine_.admission->sourceReuse=PendingAdmission::SourceReuse::fresh;
            } else if (!stopPartGroups(part)) failed_=true;
            return; // Task4 and PCM must settle before the new allocation.
        }
        const bool reuse=group<24;
        const auto source=engine_.admission->isHeldReturn() ? uint8_t(255) : mono.source;
        const auto reuseFlags=engine_.monoReuseFlags(part,group,engine_.admission->isHeldReturn(),polySource);
        if (!reuseFlags) { failed_=true; return; }
        const auto flags=*reuseFlags;
        const MelodicAllocationInputs allocation{settings.bank,false,0,uint8_t(part),0x80,255,
            {},0,{},tone};
        // CC84 without a matching source group uses the same fresh-note
        // retirement as ordinary poly (00:0fab -> 17b8), before capacity.
        if(!reuse && !engine_.retireAdmission(allocation.groupFlags,parts_.routing[part].noteFlags,
            [&](uint8_t a) {return read(a);},ControlWriter{pcm_})) { failed_=true; return; }
        auto probe=allocator;
        const auto selected=reuse ? PrepareMonoReuseAllocation(*selection,part,data_,probe,group)
            : AllocateMelodicNote(event,settings.controls,allocation,data_,probe);
        if (selected.status==MelodicAllocationResult::Status::needsCapacity) {
            const auto capacity=engine_.ensureCapacity(part,selection->partials.candidates.count,capacityPolicy_,
                [&](uint8_t a) { return read(a); },ControlWriter{pcm_});
            if (!capacity) failed_=true;
            else if (!*capacity) {
                if(!polySource && !engine_.admission->isHeldReturn()) mono.held.set(event.first,true);
                engine_.admission.reset(); // Valid note, protected capacity: discard only this admission.
            }
            return;
        }
        if (selected.status!=MelodicAllocationResult::Status::allocated) { failed_=true; return; }
        const auto key=TransposeMasterKey(TransposePartKey(event.first,settings.keyShift,
            uint8_t(settings.controls.coarseTuning)),master_.keyShift);
        const PartialSampleInstallInputs sample{settings.scale,key,event.first,event.first,255,0,event.first,0,flags};
        std::array<PartialSampleInstallInputs,2> samples{sample,sample};
        VoiceControlInputs controls; applyPart(part,controls); controls.glideRate=mono.glideRate;
        std::array<NormalPartialDspInputs,2> dsp{};
        for (unsigned partial=0;partial<2;++partial) {
            const auto slot=selected.dispatch[partial].voice;
            dsp[partial]={mono.portamento ? engine_.preparation.partKeys[part][partial] : uint8_t(255),(flags&128)!=0,0,controls,{},{}};
            if (source<128 && selected.dispatch[partial].prepare) {
                const auto& raw=data_.patch(selection->tone)->partial[partial].raw;
                const auto origin=PreparePortamentoSourceKey(TransposeMasterKey(
                    TransposePartKey(source,settings.keyShift,uint8_t(settings.controls.coarseTuning)),
                    master_.keyShift),engine_.preparation.reference,raw[10],raw[13]);
                if (!origin) { failed_=true; return; }
                dsp[partial].sourceKey=*origin;
            }
            samples[partial].minimumKey=dsp[partial].sourceKey;
            applyToneModulation(part,dsp[partial].firstControls);
            if (slot<24) dsp[partial].previousPitch=engine_.preparation.previousPitch[slot];
        }
        const auto result=reuse
            ? engine_.startReusedMelodicNote(*selection,part,samples,dsp,controllers_,data_,conversion_,waves_,
                [&](uint8_t a) { return read(a); },ControlWriter{pcm_},group)
            : engine_.startRoutedMelodicNote(event,settings.controls,allocation,samples,dsp,
                controllers_,data_,conversion_,waves_,[&](uint8_t a) { return read(a); },ControlWriter{pcm_});
        using Status=VoiceControlRuntime::MelodicStartResult::Status;
        if (result.status==Status::deferred || result.status==Status::needsCapacity) return;
        if (result.status!=Status::started && result.status!=Status::preparedOnly) { failed_=true; return; }
        if(!polySource) { mono.current=event.first; mono.velocity=event.second; mono.tone=tone; }
        if(!polySource && !engine_.admission->isHeldReturn()) mono.held.set(event.first,true);
        const auto committedGroup=selected.group->group;
        if(polySource && result.requests->count) {
            allocator.noteGroups[committedGroup].key=event.first;
            allocator.noteGroups[committedGroup].status=0;
        }
        if (!engine_.admission->isHeldReturn()) mono.source=255;
        if(!engine_.rememberPreparedNote(part,result,event.first)) { failed_=true; return; }
        engine_.admission.reset();
    }

    void releaseNote(const NoteRequest& request) noexcept
    {
        const auto part=request.part;
        const auto selected=SelectNoteRelease(request.key,parts_.routing[part].noteFlags);
        if(!selected) { failed_=true; return; }
        if(selected->path==NoteReleaseSelection::Path::group) {
            const MidiDecoder::Event event{MidiDecoder::Kind::message,0x80,request.key,0,2};
            if(!engine_.notes.noteOff(event,part,selected->selector)) { failed_=true; return; }
        } else {
            auto& mono=engine_.mono[part];
            const auto decision=engine_.releaseMonoNote(part,request.key);
            if(!decision) { failed_=true; return; }
            if(decision->action==MonoHeldKeys::ReleaseDecision::Action::replaceKey && mono.velocity) {
                engine_.admission=PendingAdmission{{NoteRequest::Action::on,part,
                    decision->replacement,mono.velocity,selectedTone_[part]},PendingAdmission::Origin::heldKeyReturn};
            }
            return; // The engine publishes the release with its key decision.
        }
        if(!engine_.runtime.publishNoteReleases(engine_.notes.allocator)) failed_=true;
    }

    void serviceVoiceCommand() noexcept
    {
#if defined(SC55_NATIVE_IO_AUDIT)
        if(holdAdmissionsAudit_) return;
#endif
        if (!engine_.serviceVoiceCompletion()) { failed_=true; return; }
        if (engine_.runtime.startupPending() || tasksPending()) return;
        if(!engine_.admission) {
            if(const auto command=engine_.commands.take()) {
                if(const auto* note=std::get_if<NoteRequest>(&*command)) {
                    if(note->action==NoteRequest::Action::on) engine_.admission=PendingAdmission{*note};
                    else releaseNote(*note);
                } else if(const auto* pedal=std::get_if<PedalRequest>(&*command)) {
                    if(pedal->kind==PedalRequest::Kind::portamento) {
                        engine_.mono[pedal->part].portamento=pedal->enabled;
                        if(pedal->enabled) engine_.mono[pedal->part].source=255;
                        refreshControls();
                    } else {
                        auto updated=engine_.notes;
                        const MidiDecoder::Event event{MidiDecoder::Kind::message,0xb0,
                            uint8_t(pedal->kind==PedalRequest::Kind::hold ? 64 : 66),uint8_t(pedal->enabled ? 127 : 0),2};
                        if(!updated.applyPedal(event,pedal->part,true)
                            || !engine_.runtime.publishNoteReleases(updated.allocator)) failed_=true;
                        else engine_.notes=updated;
                    }
                } else if(const auto* source=std::get_if<PortamentoSourceRequest>(&*command))
                    engine_.mono[source->part].source=source->key;
                else if(const auto* release=std::get_if<PartReleaseRequest>(&*command)) {
                    if(release->kind==PartReleaseRequest::Kind::notes) {
                        if(!engine_.releasePart(release->part,(parts_.routing[release->part].noteFlags&0x10)!=0,false,true))
                            failed_=true;
                    } else {
                        for(unsigned slot=0;slot<24;++slot)
                            if(engine_.runtime.voices[slot]
                                && !(engine_.notes.allocator.allocations[slot].status&0x80)
                                && engine_.installation.voices[slot].input.part==release->part)
                                if(engine_.requestStop(slot,[&](uint8_t a) { return read(a); },ControlWriter{pcm_})
                                    ==NativeVoiceEngine::StopRequest::failed) failed_=true;
                    }
                } else if(const auto* reset=std::get_if<ControllerResetRequest>(&*command))
                    (void)resetVoiceControllers(reset->part,false);
                else if(const auto* program=std::get_if<ProgramVoiceRequest>(&*command))
                    applyProgramVoiceState(program->part,program->tone);
                else if(const auto* mode=std::get_if<PartModeRequest>(&*command)) {
                    if(!applyPartMode(mode->part,mode->poly)) failed_=true;
                }
            }
        }
        if (failed() || !engine_.admission) return;
        auto event = engine_.admission->event();
        const auto originalNote=event.first;
        const auto part = engine_.admission->request.part;
        const auto& settings = parts_.parts[part];
        const auto& channel = settings.controls;
        const auto load = [&](uint8_t a) { return read(a); };
        const ControlWriter store{pcm_};
        if (parts_.routing[part].noteFlags&0x10)
        {
            startRhythm(event,part);
            return;
        }
        const bool high=event.first>=125;
        if (!high && !(parts_.routing[part].noteFlags&0x80)) { startMonoOrSource(event,part); return; }
        if(!high && engine_.mono[part].source<128) { startMonoOrSource(event,part,true); return; }
        MelodicAllocationInputs allocation{settings.bank,false,0,part,0x80,255,
            {},0,{},engine_.admission->request.tone};
        if(high) {
            const auto* patch=engine_.admission->request.tone ? data_.patch(*engine_.admission->request.tone) : nullptr;
            const auto mapping=patch ? MapHighNote(event.first,patch->common) : std::nullopt;
            if(!mapping) { failed_=true; return; }
            if(mapping->tone&0x8000) { engine_.admission.reset(); return; }
            event.first=mapping->note;
            allocation.resolvedTone=mapping->tone; allocation.groupFlags=0x81;
            allocation.groupNote=originalNote;
            allocation.keyRange={}; allocation.velocityAdjustment={};
        }
        const auto preview=engine_.previewMelodicAdmission(event,channel,allocation,
            parts_.routing[part].noteFlags,data_,load,store);
        if(!preview) { failed_=true; return; }
        const auto& selected=*preview;
        using Allocated = MelodicAllocationResult::Status;
        if (selected.status == Allocated::needsCapacity)
        {
            // The MIDI event is already owned by engine_.admission; a reclaim is never
            // replayed as a queue callback returning deferred.
            const auto count = selected.selection->partials.candidates.count;
            const auto capacity = engine_.ensureCapacity(part,count,capacityPolicy_,load,store);
            if (!capacity) failed_ = true;
            else if (!*capacity) engine_.admission.reset();
            return;
        }
        if (selected.status == Allocated::keyRangeRejected || selected.status == Allocated::velocityRejected)
        { engine_.admission.reset(); return; }
        if (selected.status != Allocated::allocated) { ++unsupported_; engine_.admission.reset(); return; }
        const auto key = high ? event.first : TransposeMasterKey(
            TransposePartKey(event.first,settings.keyShift,uint8_t(channel.coarseTuning)),master_.keyShift);
        // Validate lookup before committing allocation. Negative sample IDs
        // take113e's return-to-free-list path; they are not synthetic waves.
        const auto& patch = *data_.patch(selected.selection->tone);
        for (unsigned partial = 0; partial < 2; ++partial)
            if (selected.dispatch[partial].prepare)
            {
                const auto plan = PreparePartialSample(patch.partial[partial],*data_.samples(),
                    key,event.first,event.first,settings.scale,!high && engine_.mono[part].portamento ? engine_.preparation.partKeys[part][partial] : uint8_t(255),high ? 0x81 : 0,event.first);
                if (!plan)
                { ++unsupported_; engine_.admission.reset(); return; }
            }
        const PartialSampleInstallInputs sample{settings.scale,key,event.first,event.first,255,0,event.first,0,160};
        std::array<PartialSampleInstallInputs,2> samples{sample,sample};
        VoiceControlInputs controls;
        applyPart(part,controls);
        std::array<NormalPartialDspInputs,2> dsp{};
        for (unsigned partial = 0; partial < 2; ++partial)
        {
            // Ordinary poly preparation leaves A1CC/C974 atff. A MIDI key
            // here would manufacture a glide that cancels tuning at startup.
            dsp[partial] = {255,false,0,controls,{},{}};
            if(high) {
                samples[partial].flags=255; samples[partial].mode=0x81;
                dsp[partial].sourceKey=255; dsp[partial].unoffsetStart=true;
            } else if(engine_.mono[part].portamento) {
                samples[partial].flags=255;
                samples[partial].minimumKey=engine_.preparation.partKeys[part][partial];
                dsp[partial].sourceKey=engine_.preparation.partKeys[part][partial];
                dsp[partial].unoffsetStart=true;
            }
            applyToneModulation(part,dsp[partial].firstControls);
            const auto slot = selected.dispatch[partial].voice;
            if (slot < 24)
            {
                dsp[partial].previousPitch = engine_.preparation.previousPitch[slot];
                if (engine_.runtime.voices[slot])
                    dsp[partial].previousPitch.glide = engine_.runtime.voices[slot]->pitch.glide;
            }
        }
        const auto result = engine_.startRoutedMelodicNote(event,channel,allocation,samples,dsp,
            controllers_,data_,conversion_,waves_,load,store);
        using Start = VoiceControlRuntime::MelodicStartResult::Status;
        if (result.status == Start::deferred || result.status == Start::needsCapacity) return;
        engine_.admission.reset();
        if (result.status != Start::started && result.status != Start::preparedOnly) { failed_ = true; return; }
        if(!engine_.rememberPreparedNote(part,result,event.first)) { failed_=true; return; }
    }

    void startRhythm(MidiDecoder::Event event,unsigned part) noexcept
    {
        const auto& settings = parts_.parts[part];
        const auto flags = parts_.routing[part].noteFlags;
        const unsigned mapIndex = (flags&0x20) ? 0 : 1;
        const auto& map = rhythmMaps_[mapIndex];
        // Receive-time rejection already belongs to the accepted NoteRequest.
        // A later invalid kit must not cancel an earlier accepted request;
        // 00:0c3c reads the live drum map, not the current AB06 receiver gate.
        const auto selected = PrepareRhythmNoteVelocity(event,settings.controls.program,map,
            rhythm_->program127Accumulators,0,settings.controls.softPedal,data_);
        if (!selected) { ++unsupported_; engine_.admission.reset(); return; }
        const auto keys = PrepareRhythmInitialKeys(event.first,map,settings.keyShift,uint8_t(settings.controls.coarseTuning));
        if (!keys) { failed_ = true; return; }
        VoiceControlInputs controls;
        applyPart(part,controls,mapIndex,event.first);
        NormalPartialDspInputs dsp{keys->sourceKey,false,0,controls,{},{}};
        applyToneModulation(part,dsp.firstControls);
        const auto result = engine_.startRoutedRhythmNote(*selected,part,flags,map,settings.keyShift,
            uint8_t(settings.controls.coarseTuning),settings.scale,{0,0},{160,160},capacityPolicy_,{dsp,dsp},
            controllers_,data_,conversion_,waves_,[&](uint8_t a) { return read(a); },
            ControlWriter{pcm_});
        using Status = NativeVoiceEngine::RhythmStartResult::Status;
        if (result.status == Status::deferred) return;
        engine_.admission.reset();
        if (result.status == Status::failed || result.status == Status::invalidInput) { failed_ = true; return; }
        if (result.status != Status::started) return;
        if(!engine_.rememberPreparedNote(part,*result.start,keys->sourceKey,uint8_t(mapIndex),event.first)) failed_=true;
    }

    void serviceMidiInput() noexcept
    {
        if(resetPending() || bulkReply_.active() || parameterReplyWaiting_ || panelCount_) return;
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
        if(serviceRequested_) serviceMidiInput();
        // 5710..573c: after a failed reuse poll, task2 waits for its
        // one-kernel-tick event. Other voice groups may still advance on
        // their own control event; that must not repoll the preparing voice.
        if(engine_.activationWaiting(elapsedCycles_) && !engine_.clock.ready() && !engine_.runtime.controlPending()
            && !effectPassClock_) { serviceRequested_=false; return; }
        // Settled voices need no CPU-side work between shared control ticks.
        // Only incoming MIDI, PCM completion or an unfinished transition wakes
        // the controller. PCM itself continues rendering every sample.
        if (!serviceRequested_ && !engine_.clock.ready() && !engine_.runtime.controlPending()
            && !PCM_HasVoiceBoundary(pcm_)) return;
        serviceWork();
        serviceRequested_=!engine_.activationWaiting(elapsedCycles_) && (engine_.runtime.startupPending() || tasksPending()
            || effectPassClock_.has_value() || resetPending()
            || (!bulkReply_.active() && !parameterReplyWaiting_ && (queuedEvents()!=0 || panelCount_!=0)) || muteStops_!=0);
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
        if(muteStops_ && !engine_.runtime.startupPending()) {
            bool deferred=false;
            for(unsigned slot=0;slot<24;++slot)
                if(engine_.runtime.voices[slot] && !(engine_.notes.allocator.allocations[slot].status&0x80)
                    && (muteStops_&(1u<<engine_.installation.voices[slot].input.part))) {
                    const auto result=engine_.requestStop(slot,load,store);
                    if(result==NativeVoiceEngine::StopRequest::failed || result==NativeVoiceEngine::StopRequest::invalidInput)
                        { failed_=true; return; }
                    deferred|=result==NativeVoiceEngine::StopRequest::deferred;
                }
            if(!deferred) muteStops_=0;
        }
        if (!engine_.runtime.startupPending() && PCM_HasVoiceBoundary(pcm_)) {
            const auto slot=unsigned(PCM_TakeVoiceBoundary(pcm_));
            ++pcmBoundaryEvents_;
            if(!engine_.handlePcmBoundary(slot,conversion_,load,store)) { failed_=true; return; }
        }
        for (unsigned task = 0; task < 24 && !engine_.runtime.startupPending(); ++task)
        {
            const auto result = engine_.serviceStopTask();
            if (result.status != VoiceControlRuntime::StopTaskStatus::completed) break;
        }
        if (effectsTables_ && !engine_.runtime.startupAwaitingKeyLatch() && engine_.periodicOwnersReady())
        {
            if (!effectPassClock_ && engine_.clock.ready())
            {
                // Capture the event before FX readback; new expirations accrue
                // separately while PCM keeps running, as in task8's counter.
                effectPassClock_=engine_.clock;
                (void)engine_.clock.consume();
            }
            if (effectPassClock_ && !engine_.runtime.controlPending() && !serviceEffects()) return;
        }
        const auto control = engine_.serviceControl(controllers_,data_,conversion_,waves_,load,store,
            effectPassClock_ ? &*effectPassClock_ : nullptr);
        if (control.status==VoiceControlRuntime::ScheduledStatus::updated) effectPassClock_.reset();
        if (control.status == VoiceControlRuntime::ScheduledStatus::failed) { failed_ = true; return; }
        if (resetPending()) { serviceReset(); return; }
        if (bulkReply_.active() || parameterReplyWaiting_) return;
        for (unsigned event = 0; event < 64 && !failed(); ++event)
        {
            if (resetPending() || bulkReply_.active() || parameterReplyWaiting_) break;
            serviceVoiceCommand();
            if (engine_.admission || tasksPending() || engine_.runtime.startupPending()) break;
            if (engine_.commands.size()) continue;
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
    uint16_t muteStops_=0;
    pcm_t& pcm_;
    NativeVoiceEngine engine_;
    MidiEventQueue<2048> queue_;
    MidiReceiveState receiveState_;
    MidiReceiveTimer receiveTimer_;
    uint32_t receiveTimerPhase_=0;
    bool receiveRecovery_=false;
    uint64_t completedReceiveRecoveries_=0;
    using PendingAdmission=NativeVoiceEngine::PendingAdmission;
    // A1CE: changed program invalidates the next applicable reuse even if a
    // second program change restores the original tone before the next note.
    uint16_t reuseInvalidation_=0;
    // Shared melodic/rhythm preparation key (A1B4), read by CC84 before the
    // next note installs its own reference. Serialized with all note fanout.
    PartSettings parts_;
    std::optional<SystemDefaults> defaults_;
    std::optional<EffectsTables> effectsTables_;
    EffectsControl effects_;
    std::optional<ControlTaskClock> effectPassClock_;
    EffectsSettings effectSettings_;
    std::optional<RhythmPresetTable> rhythm_;
    std::optional<MelodicPresetTable> melodic_;
    std::array<std::optional<uint16_t>,16> selectedTone_{};
    std::array<RhythmPresetTable::Record,2> rhythmRecords_{};
    std::array<RhythmKeyMap,2> rhythmMaps_{};
    std::array<bool,16> rhythmRejected_{};
    std::array<uint8_t,16> selectedRhythmProgram_{};
    std::array<uint8_t,2> rhythmMapPrograms_{}; // AC10/11, raw shared-map program latches
    SysExReceiver sysex_;
    DisplayData display_;
    // FRT3: 2500 counts * /4 prescaler * two MCU cycles * ten IRQs.
    static constexpr uint64_t displayTimerCycles=2500*4*2*10;
    // task7 normal service waits 20 common kernel ticks at 04:378b.
    static constexpr uint64_t displayServiceCycles=20*ControlTaskClock::kernelTickCycles;
    DisplayControl displayControl_;
    DisplayControl::Line displayLine_{};
    unsigned displayPart_=1;
    bool displayAll_=false,displayTextVisible_=false;
    MasterControls master_;
    UninterpretedSystemSettings transferSettings_;
    std::array<uint8_t,16> systemName_{};
    std::array<ParameterReply,16> replies_{};
    unsigned replyHead_=0,replyCount_=0;
    bool parameterOutputConnected_=false,parameterReplyWaiting_=false,parameterReplyTransmitting_=false;
    uint64_t droppedReplies_=0;
    BulkReplyTransfer bulkReply_;
    bool bulkOutputConnected_=false;
    bool bulkInputReset_=false;
    VoiceCapacityPolicy capacityPolicy_;
    PartControllerState controllers_;
    PitchConversion conversion_;
    LfoWaveformTables waves_;
    bool failed_ = false;
    ResetPhase resetPhase_ = ResetPhase::idle;
    uint64_t completedResets_ = 0;
    uint64_t unsupported_ = 0;
    uint64_t pcmBoundaryEvents_ = 0;
    uint64_t rejectedSysEx_ = 0;
};
}
