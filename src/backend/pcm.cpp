/*
 * Copyright (C) 2021, 2024 nukeykt
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */
#include "pcm.h"
#include "pcm_effects.h"
#include "sc55_audio_buses.h"
#include "sc55_signal_renderer.h"
#include "pcm_interpolation.h"
#include "sc55_chorus_oscillator.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <cstdint>
#include <cstring>

uint8_t PCM_ReadROM(const pcm_t& pcm, uint32_t address)
{
    int bank;
    if (pcm.config_reg_3d & 0x20)
        bank = (address >> 21) & 7;
    else
        bank = (address >> 19) & 7;
    switch (bank)
    {
        case 0:
            if (pcm.is_mk1)
                return pcm.waverom1[address & 0xfffff];
            else
                return pcm.waverom1[address & 0x1fffff];
        case 1:
            if (!pcm.is_jv880)
                return pcm.waverom2[address & 0xfffff];
            else
                return pcm.waverom2[address & 0x1fffff];
        case 2:
            if (pcm.is_jv880)
                return pcm.waverom_card[address & 0x1fffff];
            else
                return pcm.waverom3[address & 0xfffff];
        case 3:
        case 4:
        case 5:
        case 6:
            if (pcm.is_jv880)
                return pcm.waverom_exp[(address & 0x1fffff) + (uint32_t)((bank - 3) * 0x200000)];
        default:
            break;
    }
    return 0;
}

void PCM_Write(pcm_t& pcm, uint32_t address, uint8_t data)
{
#if defined(SC55_NATIVE_IO_AUDIT)
    ++pcm.auditWrites[address&63];
#endif
    PCM_SynchronizeNativeReadback(pcm);
    address &= 0x3f;
    if (address < 0x4) // voice enable
    {
        switch (address & 3)
        {
            case 0:
                pcm.voice_mask_pending &= ~0xf000000u;
                pcm.voice_mask_pending |= (uint32_t)(data & 0xf) << 24;
                break;
            case 1:
                pcm.voice_mask_pending &= ~0xff0000u;
                pcm.voice_mask_pending |= (uint32_t)(data & 0xff) << 16;
                break;
            case 2:
                pcm.voice_mask_pending &= ~0xff00u;
                pcm.voice_mask_pending |= (uint32_t)(data & 0xff) << 8;
                break;
            case 3:
                pcm.voice_mask_pending &= ~0xffu;
                pcm.voice_mask_pending |= (uint32_t)(data & 0xff) << 0;
                break;
        }
        pcm.voice_mask_updating = true;
    }
    else if (address >= 0x20 && address < 0x24) // wave rom
    {
        switch (address & 3)
        {
            case 1:
                pcm.wave_read_address &= ~0xff0000u;
                pcm.wave_read_address |= (uint32_t)(data & 0xff) << 16;
                break;
            case 2:
                pcm.wave_read_address &= ~0xff00u;
                pcm.wave_read_address |= (uint32_t)(data & 0xff) << 8;
                break;
            case 3:
                pcm.wave_read_address &= ~0xffu;
                pcm.wave_read_address |= (uint32_t)(data & 0xff) << 0;
                pcm.wave_byte_latch = PCM_ReadROM(pcm, pcm.wave_read_address);
                break;
        }
    }
    else if (address == 0x3c)
    {
        pcm.config_reg_3c = data;
        PCM_GetConfig(pcm.config, data);
    }
    else if (address == 0x3d)
    {
        pcm.config_reg_3d = data;
        pcm.config.reg_slots = (data & 31) + 1;
    }
    else if (address == 0x3e)
    {
        pcm.select_channel = data & 0x1f;
    }
    else if ((address >= 0x4 && address < 0x10) || (address >= 0x24 && address < 0x30))
    {
        switch (address & 3)
        {
            case 1:
                pcm.write_latch &= ~0xf0000u;
                pcm.write_latch |= (uint32_t)(data & 0xf) << 16;
                break;
            case 2:
                pcm.write_latch &= ~0xff00u;
                pcm.write_latch |= (uint32_t)(data & 0xff) << 8;
                break;
            case 3:
                pcm.write_latch &= ~0xffu;
                pcm.write_latch |= (uint32_t)(data & 0xff) << 0;
                break;
        }
        if ((address & 3) == 3)
        {
            int ix = 0;
            if (address & 32)
                ix |= 1;
            if ((address & 8) == 0)
                ix |= 4;
            if ((address & 4) == 0)
                ix |= 2;

            pcm.ram1[pcm.select_channel][ix] = pcm.write_latch;
            pcm.sim_dirty |= 1u << pcm.select_channel;
            if(pcm.select_channel==31) pcm.effects_dirty=true;
        }
    }
    else if ((address >= 0x10 && address < 0x20) || (address >= 0x30 && address < 0x38))
    {
        switch (address & 1)
        {
        case 0:
            pcm.write_latch &= ~0xff00u;
            pcm.write_latch |= (uint32_t)(data & 0xff) << 8;
            break;
        case 1:
            pcm.write_latch &= ~0xffu;
            pcm.write_latch |= (uint32_t)(data & 0xff) << 0;
            break;
        }
        if ((address & 1) == 1)
        {
            int ix = (address >> 1) & 7;
            if (address & 32)
                ix |= 8;

            pcm.ram2[pcm.select_channel][ix] = static_cast<uint16_t>(pcm.write_latch);
            if(pcm.select_channel>=28 || ix==0) pcm.effects_dirty=true;

            // ram2[0] is a voice's pitch increment, but ram2[7] bits 0..4 let a
            // voice read another slot's, so a write there can stale any of them.
            pcm.sim_dirty |= (ix == 0) ? 0xffffffffu : (1u << pcm.select_channel);
        }
    }
}

// A pitch source can feed another voice or the chorus oscillator. Changing it
// must not re-import waveform, envelope or gate state into the audio owner.
static void PCM_UpdatePitchConsumers(pcm_t& pcm,unsigned channel) noexcept
{
    const auto increment=pcm.ram2[channel][0];
    if(pcm.native_signal) {
        pcm.native_signal->setPitchSource(channel,increment);
        return;
    }
    if(pcm.use_simulation) {
        auto& voices=pcm.native_signal ? pcm.native_signal->voices : pcm.sim;
        for(unsigned slot=0;slot<pcm.config.reg_slots;++slot)
            if((pcm.ram2[slot][7]&31)==channel) voices.phase_step[slot]=increment;
    } else pcm.sim_dirty=0xffffffffu;
    if(pcm.use_float_effects && pcm.effects) {
        if((pcm.ram2[31][7]&31)==channel) pcm.effects->chorus.increment=increment;
    } else pcm.effects_dirty=true;
}

void PCM_SetVoicePitch(pcm_t& pcm,unsigned channel,uint16_t increment) noexcept
{
    if(channel >= (pcm.native_voice_count ? pcm.native_voice_count : 32)) return;
    pcm.voiceRam2(channel)[0]=increment;
    pcm.select_channel=uint8_t(channel);
    pcm.write_latch=(pcm.write_latch&0xf0000u)|increment;
    PCM_UpdatePitchConsumers(pcm,channel);
}

void PCM_ApplyVoiceUpdate(pcm_t& pcm,unsigned channel,const sc55::VoiceRenderUpdate& update)
{
    if(!pcm.native_signal) PCM_SynchronizeNativeReadback(pcm);
    if(channel >= (pcm.native_voice_count ? pcm.native_voice_count : 24)) return;
    auto* words=pcm.voiceRam2(channel);
    words[0]=update.phaseIncrement;
    words[1]=uint16_t((uint16_t(uint8_t(update.panLeft))<<8)|uint8_t(update.panRight));
    words[2]=uint16_t((uint16_t(uint8_t(update.reverbSend))<<8)|uint8_t(update.chorusSend));
    for(unsigned i=0;i<3;++i) words[3+i]=update.rampCommands[i];
    words[6]=uint16_t((uint16_t(update.resonance)<<8)|update.filterFlags);
    // Keep the remaining legacy transactions' latch contract, but don't send
    // this native update through fifteen byte-level register operations.
    pcm.select_channel=uint8_t(channel);
    pcm.write_latch=(pcm.write_latch&0xf0000u)|update.phaseIncrement;
    if(pcm.native_signal) {
        pcm.native_signal->updateVoice(channel,update);
        return;
    }
    PCM_UpdatePitchConsumers(pcm,channel);
    if(pcm.use_simulation) {
        auto& voices=pcm.native_signal ? pcm.native_signal->voices : pcm.sim;
        PCMSim_ApplyVoiceUpdate(voices,channel,update);
        // Linked voices may take their pitch from another voice. Resolve that
        // dependency directly instead of invalidating every waveform/filter.
        for(unsigned slot=0;slot<pcm.config.reg_slots;++slot)
            voices.phase_step[slot]=pcm.ram2[pcm.ram2[slot][7]&31][0];
    }
}

bool PCM_HasVoiceBoundary(const pcm_t& pcm) noexcept
{
    return pcm.native_signal ? pcm.native_signal->hasVoiceBoundary() : pcm.irq_assert;
}

uint16_t PCM_ControlRandomWord(const pcm_t& pcm) noexcept
{
    return pcm.native_signal ? pcm.native_signal->randomWord() : pcm.ram2[30][10];
}

int PCM_TakeVoiceBoundary(pcm_t& pcm) noexcept
{
    if(pcm.native_signal) return pcm.native_signal->takeVoiceBoundary();
    if(pcm.native_voice_count && pcm.irq_assert) {
        const auto slot=pcm.irq_channel; pcm.irq_assert=false; pcm.postIrq(false); return slot;
    }
    return pcm.irq_assert ? int(PCM_Read(pcm,0x3e)&31) : -1;
}

void PCM_SetVoiceRamp(pcm_t& pcm,unsigned channel,sc55::EnvelopeRamp::Stage stage,uint16_t command) noexcept
{
    if(channel >= (pcm.native_voice_count ? pcm.native_voice_count : 32)) return;
    if(!pcm.native_signal) PCM_SynchronizeNativeReadback(pcm);
    const auto index=unsigned(stage);
    pcm.voiceRam2(channel)[3+index]=command;
    pcm.select_channel=uint8_t(channel);
    pcm.write_latch=(pcm.write_latch&0xf0000u)|command;
    if(pcm.use_simulation && channel<24) {
        auto& voices=pcm.native_signal ? pcm.native_signal->voices : pcm.sim;
        voices.envelopes[channel].ramps[index].command=command;
    }
    else if(channel<32) pcm.sim_dirty|=1u<<channel;
    if(!pcm.native_voice_count && channel>=28) pcm.effects_dirty=true;
}

uint16_t PCM_VoiceRampLevel(pcm_t& pcm,unsigned channel,sc55::EnvelopeRamp::Stage stage) noexcept
{
    if(channel >= (pcm.native_voice_count ? pcm.native_voice_count : 32)) return 0;
    pcm.select_channel=uint8_t(channel);
    pcm.read_latch=pcm.native_signal && channel<24 && !(pcm.sim_dirty&(1u<<channel))
        ? pcm.native_signal->voices.envelopes[channel].ramps[unsigned(stage)].level
        : pcm.voiceRam2(channel)[9+unsigned(stage)];
    return uint16_t(pcm.read_latch);
}

std::array<uint16_t,2> PCM_PeekVoiceGainLevels(const pcm_t& pcm,unsigned channel) noexcept
{
    if(channel >= (pcm.native_voice_count ? pcm.native_voice_count : 24)) return {};
    // Before rendering a pending legacy write, that control view is newer.
    // Otherwise the renderer owns the current levels; do not materialize all
    // voices' wave/filter histories merely to read two gains or draw a meter.
    if(pcm.native_signal && !(pcm.sim_dirty&(1u<<channel))) {
        const auto& ramps=pcm.native_signal->voices.envelopes[channel].ramps;
        return {ramps[0].level,ramps[1].level};
    }
    return {pcm.voiceRam2(channel)[9],pcm.voiceRam2(channel)[10]};
}

void PCM_CommitVoiceKeys(pcm_t& pcm,sc55::VoiceSet enabled)
{
    if(pcm.native_voice_count) pcm.native_keys=enabled;
    PCM_CommitVoiceKeys(pcm,enabled.lowWord());
}

void PCM_CommitVoiceKeys(pcm_t& pcm,uint32_t enabled)
{
    PCM_SynchronizeNativeReadback(pcm);
    // The hardware exposes 28 writable key bits; preserve reserved upper bits
    // exactly as the four-byte transaction does. Rendering owns key edges.
    pcm.voice_mask_pending=(pcm.voice_mask_pending&0xf0000000u)|(enabled&0x0fffffffu);
    pcm.voice_mask=pcm.voice_mask_pending;
    pcm.voice_mask_updating=false;
    if(pcm.native_signal) pcm.native_signal->setVoiceKeys(enabled);
}

std::array<uint16_t,2> PCM_VoiceGainLevels(pcm_t& pcm,unsigned channel)
{
    if(channel >= (pcm.native_voice_count ? pcm.native_voice_count : 24)) return {};
    const auto levels=PCM_PeekVoiceGainLevels(pcm,channel);
    pcm.select_channel=uint8_t(channel);
    pcm.read_latch=levels[1];
    return levels;
}

bool PCM_CompleteVoiceEnable(pcm_t& pcm,unsigned channel,uint16_t level,uint16_t command)
{
    PCM_SynchronizeNativeReadback(pcm);
    if(channel >= (pcm.native_voice_count ? pcm.native_voice_count : 24)) return false;
    pcm.select_channel=uint8_t(channel);
    pcm.read_latch=pcm.voiceRam2(channel)[7];
    if(pcm.native_signal) {
        if(!pcm.native_signal->voiceReady(channel)) return false;
    } else if(!(pcm.read_latch&32)
        || pcm.cycles-pcm.native_voice_install_cycle[channel]<2*625) return false;
    // No audio frame occurs inside the old hold/level/command transaction.
    pcm.voiceRam2(channel)[11]=uint16_t(level>>1);
    pcm.voiceRam2(channel)[5]=command;
    pcm.write_latch=(pcm.write_latch&0xf0000u)|command;
    if(pcm.use_simulation) {
        auto& voices=pcm.native_signal ? pcm.native_signal->voices : pcm.sim;
        voices.envelopes[channel].ramps[2]={command,uint16_t(level>>1)};
    } else if(channel<32) pcm.sim_dirty|=1u<<channel;
    return true;
}

static void PCM_InstallSimulatedVoice(pcm_t& pcm,unsigned channel,const sc55::VoiceRenderStart& start);

void PCM_InstallVoice(pcm_t& pcm,unsigned channel,const sc55::VoiceRenderStart& start)
{
    PCM_SynchronizeNativeReadback(pcm);
    if(channel >= (pcm.native_voice_count ? pcm.native_voice_count : 24)) return;
    pcm.voiceRam2(channel)[7]=start.mode;
    if(pcm.native_voice_count) pcm.native_pitch_source[channel]=start.pitchSource<128 ? start.pitchSource : uint8_t(start.mode&31);
    pcm.native_voice_install_cycle[channel]=pcm.cycles;
    pcm.voiceRam1(channel)[4]=start.start&0xfffffu;
    pcm.voiceRam1(channel)[2]=start.loop&0xfffffu;
    pcm.voiceRam1(channel)[0]=start.end&0xfffffu;
    pcm.write_latch=start.end&0xfffffu;
    PCM_ApplyVoiceUpdate(pcm,channel,start.controls);
    if(pcm.use_simulation) PCM_InstallSimulatedVoice(pcm,channel,start);
    else pcm.sim_dirty=0xffffffffu;
}

std::array<uint16_t,3> PCM_SynchronizeVoiceEnvelopes(pcm_t& pcm,unsigned channel,
    const std::array<uint16_t,3>& commands,const std::array<uint16_t,3>& levels)
{
    auto result=levels;
    if(channel >= (pcm.native_voice_count ? pcm.native_voice_count : 24)) return result;
    if(pcm.native_signal && !(pcm.sim_dirty&(1u<<channel))) {
        auto& envelopes=pcm.native_signal->voices.envelopes[channel];
        result=envelopes.synchronize(commands,levels);
        // Retain compatibility latches for diagnostics/legacy callers, but
        // never read back the whole renderer to perform a segment transition.
        pcm.select_channel=uint8_t(channel);
        for(unsigned i=0;i<3;++i) {
            const bool held=commands[i]==0xff00;
            pcm.voiceRam2(channel)[3+i]=envelopes.ramps[i].command;
            pcm.voiceRam2(channel)[9+i]=envelopes.ramps[i].level;
            const auto value=held ? envelopes.ramps[i].level : uint16_t(0xff00);
            pcm.write_latch=(pcm.write_latch&0xf0000u)|value;
            if(!held) pcm.read_latch=envelopes.ramps[i].level;
        }
        return result;
    }
    PCM_SynchronizeNativeReadback(pcm);
    pcm.select_channel=uint8_t(channel);
    for(unsigned i=0;i<3;++i) {
        const bool held=commands[i]==0xff00;
        const uint16_t value=held ? uint16_t(levels[i]>>1) : uint16_t(0xff00);
        pcm.voiceRam2(channel)[held ? 9+i : 3+i]=value;
        pcm.write_latch=(pcm.write_latch&0xf0000u)|value;
        if(!held) {
            pcm.read_latch=pcm.voiceRam2(channel)[9+i];
            result[i]=uint16_t(pcm.read_latch<<1);
        }
    }
    if(pcm.use_simulation) {
        auto& voices=pcm.native_signal ? pcm.native_signal->voices : pcm.sim;
        for(unsigned i=0;i<3;++i)
            voices.envelopes[channel].ramps[i]={pcm.voiceRam2(channel)[3+i],pcm.voiceRam2(channel)[9+i]};
    } else if(channel<32) pcm.sim_dirty|=1u<<channel;
    return result;
}

// rv: [30][2], [30][3]
// ch: [31][2], [31][5]

uint8_t PCM_Read(pcm_t& pcm, uint32_t address)
{
#if defined(SC55_NATIVE_IO_AUDIT)
    ++pcm.auditReads[address&63];
#endif
    PCM_SynchronizeNativeReadback(pcm);
    address &= 0x3f;
    //fprintf(stderr, "PCM Read: %.2x\n", address);

    if (address < 0x4)
    {
        if (pcm.voice_mask_updating)
            pcm.voice_mask = pcm.voice_mask_pending;
        pcm.voice_mask_updating = false;
    }
    else if (address == 0x3c || address == 0x3e) // status
    {
        uint8_t status = 0;
        if (address == 0x3e && pcm.irq_assert)
        {
            pcm.irq_assert = false;
            pcm.postIrq(false);
        }

        status |= pcm.irq_channel;
        if (pcm.voice_mask_updating)
            status |= 32;

        return status;
    }
    else if (address == 0x3f)
    {
        return pcm.wave_byte_latch;
    }
    else if ((address >= 0x4 && address < 0x10) || (address >= 0x24 && address < 0x30))
    {
        if ((address & 3) == 1)
        {
            int ix = 0;
            if (address & 32)
                ix |= 1;
            if ((address & 8) == 0)
                ix |= 4;
            if ((address & 4) == 0)
                ix |= 2;

            pcm.read_latch = pcm.ram1[pcm.select_channel][ix];
        }
    }
    else if ((address >= 0x10 && address < 0x20) || (address >= 0x30 && address < 0x38))
    {
        if ((address & 1) == 0)
        {
            int ix = (address >> 1) & 7;
            if (address & 32)
                ix |= 8;

            pcm.read_latch = pcm.ram2[pcm.select_channel][ix];
        }
    }
    else if (address >= 0x39 && address <= 0x3b)
    {
        switch (address & 3)
        {
            case 1:
                return (pcm.read_latch >> 16) & 0xf;
            case 2:
                return (pcm.read_latch >> 8) & 0xff;
            case 3:
                return (pcm.read_latch >> 0) & 0xff;
        }
    }

    return 0;
}


// Sign-extends a 20-bit signed integer to a 32-bit signed integer.
constexpr inline int32_t sx20(int32_t in)
{
    return (in << 12) >> 12;
}

inline int32_t addclip20(int32_t add1, int32_t add2, int32_t cin)
{
    return sx20(add1) + sx20(add2) + cin;
}

inline int32_t multi(int32_t val1, int8_t val2)
{
    return sx20(val1) * val2;
}

// Interpolation data is shared with the independent voice renderer.

inline void calc_tv(pcm_t& pcm, int e, int adjust, uint16_t *levelcur, int active, int *volmul)
{
    // int adjust = ram2[3+e];
    // int levelcur = ram2[9+e] & 0x7fff;
    *levelcur &= 0x7fff;
    int speed = adjust & 0xff;
    int target = (adjust >> 8) & 0xff;

                
    const bool w1 = (speed & 0xf0) == 0;
    const bool w2 = w1 || (speed & 0x10) != 0;
    const bool w3 = pcm.nfs &&
        ((speed & 0x80) == 0 || ((speed & 0x40) == 0 && (!w2 || (speed & 0x20) == 0)));

    int type = (int)w2 | ((int)w3 << 3);
    if (speed & 0x20)
        type |= 2;
    if ((speed & 0x80) == 0 || (speed & 0x40) == 0)
        type |= 4;


    bool write = !active;
    int addlow = 0;
    if (type & 4)
    {
        if (pcm.tv_counter & 8)
            addlow |= 1;
        if (pcm.tv_counter & 4)
            addlow |= 2;
        if (pcm.tv_counter & 2)
            addlow |= 4;
        if (pcm.tv_counter & 1)
            addlow |= 8;
        write |= 1;
    }
    else
    {
        switch (type & 3)
        {
        case 0:
            if (pcm.tv_counter & 0x20)
                addlow |= 1;
            if (pcm.tv_counter & 0x10)
                addlow |= 2;
            if (pcm.tv_counter & 8)
                addlow |= 4;
            if (pcm.tv_counter & 4)
                addlow |= 8;
            write |= (pcm.tv_counter & 3) == 0;
            break;
        case 1:
            if (pcm.tv_counter & 0x80)
                addlow |= 1;
            if (pcm.tv_counter & 0x40)
                addlow |= 2;
            if (pcm.tv_counter & 0x20)
                addlow |= 4;
            if (pcm.tv_counter & 0x10)
                addlow |= 8;
            write |= (pcm.tv_counter & 15) == 0;
            break;
        case 2:
            if (pcm.tv_counter & 0x200)
                addlow |= 1;
            if (pcm.tv_counter & 0x100)
                addlow |= 2;
            if (pcm.tv_counter & 0x80)
                addlow |= 4;
            if (pcm.tv_counter & 0x40)
                addlow |= 8;
            write |= (pcm.tv_counter & 63) == 0;
            break;
        case 3:
            if (pcm.tv_counter & 0x800)
                addlow |= 1;
            if (pcm.tv_counter & 0x400)
                addlow |= 2;
            if (pcm.tv_counter & 0x200)
                addlow |= 4;
            if (pcm.tv_counter & 0x100)
                addlow |= 8;
            write |= (pcm.tv_counter & 127) == 0;
            break;
        }
    }

    if ((type & 8) == 0)
    {
        int shift = speed & 15;
        shift = (10 - shift) & 15;

        int sum1 = (target << 11); // 5
        if (e != 2 || active)
            sum1 -= (*levelcur << 4); // 6
        const bool neg = (sum1 & 0x80000) != 0;
        (void)neg; // unused

        int preshift = sum1;

        int shifted = preshift >> shift;
        shifted -= sum1;

        int sum2 = (target << 11) + addlow + shifted;
        if (write && pcm.nfs)
            *levelcur = (sum2 >> 4) & 0x7fff;

        if (e == 0)
        {
            *volmul = (sum2 >> 4) & 0x7ffe;
        }
        else if (e == 1)
        {
            *volmul = (sum2 >> 4) & 0x7ffe;
        }
    }
    else
    {
        int shift = (speed >> 4) & 14;
        shift |= (int)w2;
        shift = (10 - shift) & 15;

        int sum1 = target << 11; // 5
        if (e != 2 || active)
            sum1 -= (*levelcur << 4); // 6
        const bool neg = (sum1 & 0x80000) != 0;
        int preshift = (speed & 15) << 9;
        if (!w1)
            preshift |= 0x2000;
        if (neg)
            preshift ^= ~0x3f;

        int shifted = preshift >> shift;
        int sum2 = shifted;
        if (e != 2 || active)
            sum2 += (*levelcur << 4) | addlow;

        int sum2_l = (sum2 >> 4);

        int sum3 = (target << 11) - (sum2_l << 4);

        const bool neg2 = (sum3 & 0x80000) != 0;
        const bool xnor = !(neg2 ^ neg);

        if (write && pcm.nfs)
        {
            if (xnor)
                *levelcur = sum2_l & 0x7fff;
            else
                *levelcur = (uint16_t)(target << 7);
        }

        if (e == 0)
        {
            *volmul = sum2_l & 0x7ffe;
        }
        else if (e == 1)
        {
            if (xnor)
                *volmul = sum2_l & 0x7ffe;
            else
                *volmul = target << 7;
        }
    }
}

inline int eram_unpack(pcm_t& pcm, uint32_t addr, int type = 0)
{
    addr &= 0x3fff;
    int data = pcm.eram[addr];
    int val = data & 0x3fff;
    int sh = (data >> 14) & 3;

    val <<= 18;
    return val >> (18 - sh * 2 + type);
}

inline void eram_pack(pcm_t& pcm, uint32_t addr, uint32_t val)
{
    if (val) pcm.eram_wrote_nonzero = 1;   // 鳴り止み判定用
    addr &= 0x3fff;
    int sh = 0;
    int top = (val >> 13) & 0x7f;
    if (top & 0x40)
        top ^= 0x7f;
    if (top >= 16)
        sh = 3;
    else if (top >= 4)
        sh = 2;
    else if (top >= 1)
        sh = 1;
    else
        sh = 0;

    int data = (val >> (sh * 2)) & 0x3fff;
    data |= sh << 14;
    pcm.eram[addr] = (uint16_t)data;
}

void PCM_GetConfig(PCM_Config& config, uint8_t config_byte)
{
    if ((config_byte & 0x30) != 0)
    {
        switch ((config_byte >> 2) & 3)
        {
        case 1:
            config.noise_mask = 3;
            break;
        case 2:
            config.noise_mask = 7;
            break;
        case 3:
            config.noise_mask = 15;
            break;
        }
        switch (config_byte & 3)
        {
        case 1:
            config.orval |= 1 << 8;
            break;
        case 2:
            config.orval |= 1 << 10;
            break;
        }
        config.write_mask = 15;
        config.dac_mask   = ~15;
    }
    else
    {
        switch ((config_byte >> 2) & 3)
        {
        case 2:
            config.noise_mask = 1;
            break;
        case 3:
            config.noise_mask = 3;
            break;
        }
        switch (config_byte & 3)
        {
        case 1:
            config.orval |= 1 << 6;
            break;
        case 2:
            config.orval |= 1 << 8;
            break;
        }
        config.write_mask = 3;
        config.dac_mask   = ~3;
    }
    if ((config_byte & 0x80) == 0)
    {
        config.write_mask = 0;
    }
    if ((config_byte & 0x30) == 0x30)
    {
        config.orval |= 1 << 12;
    }
    if (config_byte & 0x40)
    {
        config.oversampling = true;
    }
}


// Runs the voice slots through pcm_sim instead of the transcribed pipeline, and
// puts the results back where the rest of the chip and the firmware expect
// them: the mix and send buses, the per-voice registers the firmware reads, and
// the end-of-sample interrupt that drives its envelope segments. The effect
// network and the output stage above are untouched.
namespace
{
// Stands in for a bank with no ROM behind it, so the read path needs no null
// check: an unmapped voice reads silence instead of branching.
const uint8_t g_silent_rom[32] = {};

// A resolved wave ROM window. Mirrors PCM_ReadROM in pcm.cpp, which works out
// the bank, chases pcm.is_mk1 and runs a switch on every single byte.
struct RomWindow
{
    const uint8_t* base = nullptr;
    uint32_t mask = 0;
};

inline RomWindow ResolveRom(const pcm_t& pcm, uint32_t address)
{
    const int bank = (pcm.config_reg_3d & 0x20) ? ((address >> 21) & 7)
                                                : ((address >> 19) & 7);
    switch (bank)
    {
        case 0:
            return pcm.is_mk1 ? RomWindow{pcm.waverom1, 0xfffff}
                                   : RomWindow{pcm.waverom1, 0x1fffff};
        case 1:
            return pcm.is_jv880 ? RomWindow{pcm.waverom2, 0x1fffff}
                                     : RomWindow{pcm.waverom2, 0xfffff};
        case 2:
            return pcm.is_jv880 ? RomWindow{pcm.waverom_card, 0x1fffff}
                                     : RomWindow{pcm.waverom3, 0xfffff};
        case 3: case 4: case 5: case 6:
            if (pcm.is_jv880)
                return RomWindow{pcm.waverom_exp + (bank - 3) * 0x200000, 0x1fffff};
            break;
        default:
            break;
    }
    return RomWindow{g_silent_rom, 0x1f};
}
}

static void PCM_InstallSimulatedVoice(pcm_t& pcm,unsigned channel,const sc55::VoiceRenderStart& start)
{
    auto& voices=pcm.native_signal ? pcm.native_signal->voices : pcm.sim;
    const uint32_t bank=uint32_t((start.mode>>8)&15)<<20;
    const uint32_t position=start.start&0xfffff;
    const auto samples=ResolveRom(pcm,bank|position);
    const auto exponents=ResolveRom(pcm,bank|(position>>5));
    const PCMSimWaveform waveform{samples.base,exponents.base,samples.mask,exponents.mask,
        int32_t(start.loop&0xfffff),int32_t(start.end&0xfffff),int32_t(bank),
        bool(start.mode&0x40),bool(start.mode&0x80)};
    if(pcm.native_signal) {
        pcm.native_signal->installVoice(channel,waveform,position,start.mode&31);
        pcm.sim_dirty&=~(1u<<channel);
        return;
    }
    PCMSim_SetWaveform(voices,channel,waveform);
    for(unsigned i=0;i<3;++i)
        voices.envelopes[channel].ramps[i]={start.controls.rampCommands[i],pcm.ram2[channel][9+i]};
    const uint32_t bit=1u<<channel;
    voices.boundaryLatched=(voices.boundaryLatched&~bit)|((pcm.ram2[channel][8]&0x4000)?bit:0);
    // Pitch links were updated by ApplyVoiceUpdate. No waveform/filter rebuild
    // of other voices is needed. The paired key-enable still owns oscillator
    // restart; installing geometry must not reset a running voice early.
    pcm.sim_dirty&=~bit;
}

void PCMSim_SyncVoice(PCMSimVoices& voices, const pcm_t& pcm, int slot)
{
    const uint32_t* ram1 = pcm.ram1[slot];
    const uint16_t* ram2 = pcm.ram2[slot];

    for(unsigned envelope=0;envelope<3;++envelope)
        voices.envelopes[slot].ramps[envelope]={ram2[3+envelope],ram2[9+envelope]};

    // Loop and end are control: the firmware sets them when it starts a note
    // and does not touch them again. The read position is ours from then on,
    // so it is deliberately not pulled in here -- see PCMSim_KeyOn.

    // The increment lives in another slot's ram2[0]; ram2[7] bits 0..4 say
    // which. One whole source sample is 0x4000.
    voices.phase_step[slot] = pcm.ram2[ram2[7] & 31][0];

    const auto bank=uint32_t(((ram2[7] >> 8)&15)<<20);
    const uint32_t here = ram1[4] & 0xfffff;
    const RomWindow samples = ResolveRom(pcm,bank | here);
    const RomWindow blocks = ResolveRom(pcm,bank | (here>>5));
    PCMSim_SetWaveform(voices,unsigned(slot),{samples.base,blocks.base,samples.mask,blocks.mask,
        int32_t(ram1[2]&0xfffff),int32_t(ram1[0]&0xfffff),int32_t(bank),
        bool(ram2[7]&0x40),bool(ram2[7]&0x80)});

    voices.svf_q[slot]   = static_cast<float>((ram2[6] >> 8) & 127) * (1.0f / 64.0f);
    voices.svf_tap[slot] = (ram2[6] & 2) ? 1.0f : 0.0f;
    const auto boundaryBit=uint32_t(1)<<slot;
    voices.boundaryEnabled=(voices.boundaryEnabled&~boundaryBit)|((ram2[6]&1)?boundaryBit:0);
    voices.boundaryLatched=(voices.boundaryLatched&~boundaryBit)|((ram2[8]&0x4000)?boundaryBit:0);

    // Pan and the two sends are signed 8 bit gains packed two to a word.
    voices.pan_l[slot]       = static_cast<float>(static_cast<int8_t>((ram2[1] >> 8) & 255)) * (1.0f / 64.0f);
    voices.pan_r[slot]       = static_cast<float>(static_cast<int8_t>(ram2[1] & 255)) * (1.0f / 64.0f);
    voices.send_reverb[slot] = static_cast<float>(static_cast<int8_t>((ram2[2] >> 8) & 255)) * (1.0f / 64.0f);
    voices.send_chorus[slot] = static_cast<float>(static_cast<int8_t>(ram2[2] & 255)) * (1.0f / 64.0f);

    const bool sounding = ((ram2[7] & 0x20) != 0)
                       && (((pcm.voice_mask & pcm.voice_mask_pending) >> slot) & 1) != 0;
    voices.gate[slot] = sounding ? 1.0f : 0.0f;
}

void PCMSim_KeyOn(PCMSimVoices& voices, const pcm_t& pcm, int slot)
{
    PCMSim_RestartVoice(voices,unsigned(slot),pcm.ram1[slot][4],pcm.ram2[slot][8],
        bool(pcm.ram2[slot][8]&0x8000));
}

static void PCM_PrepareSimulatedVoices(pcm_t& pcm,PCMSimVoices& sim)
{
    sim.voice_count = pcm.config.reg_slots;

    const uint32_t keys = pcm.voice_mask & pcm.voice_mask_pending;

    for (int slot = 0; slot < sim.voice_count; slot++)
    {
        uint16_t* ram2 = pcm.ram2[slot];
        const bool okey = (ram2[7] & 0x20) != 0;
        const bool key = ((keys >> slot) & 1) != 0;

        // Only voices the firmware has touched need rebuilding. Whether a voice
        // is sounding is not one of those: it turns on and off from the voice
        // mask and the key bit, both of which move on their own.
        if (pcm.sim_dirty & (1u << slot))
            PCMSim_SyncVoice(sim, pcm, slot);

        sim.gate[slot] = (okey && key) ? 1.0f : 0.0f;

        if (key && !okey)
            PCMSim_KeyOn(sim, pcm, slot);

    }
    pcm.sim_dirty = 0;

}

static void PCM_PublishSimulatedVoices(pcm_t& pcm,PCMSimVoices& sim,bool update)
{
    for (int slot = 0; slot < sim.voice_count; slot++)
    {
        uint32_t* ram1 = pcm.ram1[slot];
        uint16_t* ram2 = pcm.ram2[slot];
        const bool active = sim.gate[slot]!=0;

        for(unsigned envelope=0;envelope<3;++envelope)
            ram2[9+envelope]=sim.envelopes[slot].ramps[envelope].level;

        if (active)
        {
            ram1[4] = (uint32_t)(sim.address[slot] & 0xfffff);
            ram1[5] = (uint32_t)((int32_t)lrintf(sim.reference[slot]) & 0xfffff);
            ram1[1] = (uint32_t)((int32_t)lrintf(sim.svf_band[slot]) & 0xfffff);
            ram1[3] = (uint32_t)((int32_t)lrintf(sim.svf_low[slot]) & 0xfffff);

            ram2[8] = (uint16_t)(((sim.boundaryLatched&(uint32_t(1)<<slot)) ? 0x4000 : 0)
                                 | (sim.sub_phase[slot] & 0x3fff)
                                 | (sim.reverse_mask[slot] ? 0x8000u : 0u));

        }
        else
        {
            if (update)
            {
                ram1[1] = 0;
                ram1[3] = 0;
                ram1[5] = 0;
            }
            ram2[8] = 0;
        }

    }

}

void PCM_SynchronizeNativeReadback(pcm_t& pcm) noexcept
{
    if(!pcm.native_readback_pending) return;
    PCM_PublishSimulatedVoices(pcm,pcm.native_signal->voices,pcm.native_readback_update);
    pcm.native_readback_pending=false;
}

static void PCM_CommitSimulatedFrame(pcm_t& pcm,int boundaryVoice)
{
    if(boundaryVoice>=0) {
        pcm.irq_assert=true;
        pcm.irq_channel=uint8_t(boundaryVoice);
        pcm.postIrq(true);
    }
    if(pcm.nfs) {
        const uint32_t keys=pcm.voice_mask&pcm.voice_mask_pending;
        for(unsigned slot=0;slot<pcm.config.reg_slots;++slot)
            if(keys&(1u<<slot)) pcm.ram2[slot][7]|=0x20;
    }
}

static void PCM_UpdateVoicesSimulated(pcm_t& pcm,const int rcadd[6],const int rcadd2[6])
{
    auto& sim=pcm.sim;
    PCM_PrepareSimulatedVoices(pcm,sim);
    float out[4];
    PCMSim_RenderFrame(sim,{pcm.tv_counter,pcm.nfs},out);
    const auto boundaryVoice=PCMSim_CollectBoundary(sim,pcm.nfs,!pcm.irq_assert);
    PCM_PublishSimulatedVoices(pcm,sim,pcm.nfs);
    PCM_CommitSimulatedFrame(pcm,boundaryVoice);
    const auto mixed=sc55::MixVoiceAndEffectBuses(out,rcadd,rcadd2);
    pcm.accum_l=mixed.left; pcm.accum_r=mixed.right;
    pcm.rcsum[0]=mixed.reverb; pcm.rcsum[1]=mixed.chorus;
}

void PCM_UseSimulation(pcm_t& pcm, bool enable)
{
    PCMSim_Init();
    pcm.use_simulation = enable;
}

static void PCM_SyncFloatEffects(pcm_t& pcm)
{
    if(pcm.effects_dirty) {
        if(pcm.native_signal)
            pcm.native_signal->adoptFrameState(bool(pcm.ram2[31][7]&0x20),pcm.nfs);
#if defined(SC55_NATIVE_IO_AUDIT)
        ++pcm.auditEffectImports;
#endif
        auto& settings=pcm.effects->settings;
        const auto coefficient=PCMEffectsSettings::decode;
        std::copy_n(pcm.ram2[28],12,settings.diffusionTaps.begin());
        std::copy_n(pcm.ram2[29],12,settings.tailTaps.begin());
        settings.reverbInput=coefficient(pcm.ram2[30][1]);
        settings.chorusInput=coefficient(pcm.ram2[31][1]);
        settings.comb=coefficient(pcm.ram2[30][6]);
        for(unsigned i=0;i<2;++i) {
            settings.diffusion[i]=coefficient(pcm.ram2[30][4+i]);
            settings.damping[i]=coefficient(pcm.ram2[30][7+i]);
            settings.reverbReturn[i]=coefficient(pcm.ram2[30][2+i]);
        }
        for(unsigned i=0;i<4;++i) settings.chorusReturn[i]=coefficient(pcm.ram2[31][2+i]);
        auto& fx=*pcm.effects;
        fx.spread={pcm.ram2[30][0],pcm.ram2[30][9]};
        fx.chorus.position=pcm.ram1[31][4];
        fx.chorus.begin=pcm.ram1[31][2]; fx.chorus.end=pcm.ram1[31][0];
        fx.chorus.phase=pcm.ram2[31][8]&0x3fff;
        fx.chorus.increment=pcm.ram2[pcm.ram2[31][7]&31][0];
        if(pcm.native_signal) {
            const auto source=pcm.ram2[31][7]&31;
            pcm.native_signal->setPitchSource(source,pcm.ram2[source][0]);
            pcm.native_signal->setChorusPitchSource(source);
        }
        fx.chorus.descending=(pcm.ram2[31][8]&0x8000)!=0;
        fx.chorus.pingPong=(pcm.ram2[31][7]&0x40)!=0;
        fx.chorus.reverse=(pcm.ram2[31][7]&0x80)!=0;
        fx.phaseMarker=(pcm.ram2[31][8]&0x4000)!=0;
        fx.interpolation={pcm.ram2[31][9],pcm.ram2[31][10]};
        fx.leftTap=pcm.ram2[29][10]; fx.rightTap=pcm.ram2[29][11];
        pcm.effects_dirty=false;
    }
}

void PCM_BeginReverbDrain(pcm_t& pcm) noexcept
{
    PCM_SynchronizeNativeReadback(pcm);
    if(pcm.use_float_effects) {
        PCM_SyncFloatEffects(pcm); pcm.effects->beginReverbDrain();
    }
    pcm.ram2[30][0]=0xb6;
    std::fill_n(pcm.ram2[30]+1,8,0);
    for(unsigned i=0;i<6;++i) pcm.ram2[28][i]=uint16_t(i);
    pcm.ram2[28][8]=6; pcm.ram2[28][9]=7;
    pcm.ram2[29][0]=0x2000; pcm.ram2[29][1]=0x2001;
    pcm.ram2[29][4]=0x2002; pcm.ram2[29][5]=0x2003;
    pcm.select_channel=29;
    pcm.write_latch=(pcm.write_latch&0xf0000u)|0x2003;
    for(unsigned source=28;source<=30;++source) PCM_UpdatePitchConsumers(pcm,source);
    pcm.effects_dirty=!pcm.use_float_effects;
}

void PCM_UpdateEffect(pcm_t& pcm,sc55::EffectParameter parameter,uint16_t value) noexcept
{
    PCM_SynchronizeNativeReadback(pcm);
    if(pcm.use_float_effects) {
        PCM_SyncFloatEffects(pcm);
        pcm.effects->update(parameter,value);
    }
    using P=sc55::EffectParameter;
    unsigned bank=31;
    uint16_t last=value;
    switch(parameter) {
    case P::reverbInput: bank=30; pcm.ram2[30][1]=value; break;
    case P::reverbOutput: bank=30; pcm.ram2[30][2]=pcm.ram2[30][3]=value; break;
    case P::reverbSpread:
        bank=30; pcm.ram2[30][0]=value; PCM_UpdatePitchConsumers(pcm,30); break;
    case P::chorusInput: pcm.ram2[31][1]=value; break;
    case P::chorusLevel:
        pcm.ram2[31][2]=value; last=value&0xff00; pcm.ram2[31][5]=last; break;
    case P::chorusFeedback: pcm.ram2[31][3]=value; break;
    case P::chorusSend: pcm.ram2[31][2]=value; break;
    }
    pcm.select_channel=uint8_t(bank);
    pcm.write_latch=(pcm.write_latch&0xf0000u)|last;
    pcm.sim_dirty|=1u<<bank;
    pcm.effects_dirty=!pcm.use_float_effects;
}

void PCM_SetChorusMix(pcm_t& pcm,const std::array<uint8_t,3>& mix) noexcept
{
    PCM_SynchronizeNativeReadback(pcm);
    if(pcm.use_float_effects) {
        PCM_SyncFloatEffects(pcm); pcm.effects->setChorusMix(mix);
    }
    pcm.ram2[31][2]=uint16_t((mix[0]<<8)|mix[2]);
    pcm.ram2[31][3]=mix[1]; pcm.ram2[31][4]=0;
    pcm.ram2[31][5]=uint16_t(mix[0]<<8);
    pcm.select_channel=31;
    pcm.write_latch=(pcm.write_latch&0xf0000u)|pcm.ram2[31][5];
    pcm.sim_dirty|=1u<<31;
    pcm.effects_dirty=!pcm.use_float_effects;
}

void PCM_ConfigureReverb(pcm_t& pcm,const sc55::ReverbSetup& setup) noexcept
{
    PCM_SynchronizeNativeReadback(pcm);
    if(pcm.use_float_effects) {
        PCM_SyncFloatEffects(pcm);
        pcm.effects->configureReverb(setup);
    }
    std::copy(setup.diffusionTaps.begin(),setup.diffusionTaps.end(),pcm.ram2[28]);
    std::copy(setup.tailTaps.begin(),setup.tailTaps.end(),pcm.ram2[29]);
    for(unsigned i=0;i<2;++i) {
        pcm.ram2[30][4+i]=setup.diffusion[i];
        pcm.ram2[30][7+i]=setup.damping[i];
        pcm.ram2[30][2+i]=setup.output;
    }
    pcm.ram2[30][6]=setup.comb;
    pcm.ram2[30][0]=setup.spreadCommand;
    pcm.select_channel=setup.delayProgram?29:30;
    pcm.write_latch=(pcm.write_latch&0xf0000u)|(setup.delayProgram?setup.tailTaps[8]:setup.spreadCommand);
    for(unsigned source=28;source<=30;++source) PCM_UpdatePitchConsumers(pcm,source);
    pcm.effects_dirty=!pcm.use_float_effects;
}

void PCM_ConfigureChorus(pcm_t& pcm,const sc55::ChorusSetup& setup) noexcept
{
    PCM_SynchronizeNativeReadback(pcm);
    if(pcm.use_float_effects) {
        PCM_SyncFloatEffects(pcm); // Submit earlier reverb changes first.
        pcm.effects->configureChorus(setup);
    }
    // The remaining controller still observes this compatibility view. No
    // byte-level transactions or readback polling are needed on this owner.
    pcm.ram2[29][9]=0x3800;
    pcm.ram2[31][7]=0x7f;
    pcm.ram1[31][2]=setup.begin&0xfffff;
    pcm.ram1[31][0]=setup.end&0xfffff;
    pcm.ram1[31][4]=setup.position&0xfffff;
    pcm.ram2[31][8]=0;
    pcm.ram2[31][0]=setup.increment;
    if(pcm.native_signal) {
        pcm.native_signal->setChorusPitchSource(31);
        pcm.native_signal->enableEffects();
    }
    for(unsigned i=0;i<4;++i) pcm.ram2[31][2+i]=setup.returns[i];
    pcm.select_channel=31;
    pcm.read_latch=0;
    pcm.write_latch=(setup.position&0xf0000u)|setup.returns[3];
    PCM_UpdatePitchConsumers(pcm,31);
    pcm.effects_dirty=!pcm.use_float_effects;
}

static void PCM_PublishFloatEffects(pcm_t& pcm)
{
    auto& fx=*pcm.effects;
    pcm.ram2[30][9]=fx.spread.level;
    pcm.ram2[31][9]=fx.interpolation[0]; pcm.ram2[31][10]=fx.interpolation[1];
    pcm.ram1[31][4]=fx.chorus.position;
    pcm.ram2[31][8]=uint16_t((fx.phaseMarker?0x4000:0)|fx.chorus.phase
        |(fx.chorus.descending?0x8000:0));
    pcm.ram2[29][10]=fx.leftTap; pcm.ram2[29][11]=fx.rightTap;
}

void PCM_PrepareIndependentRender(pcm_t& pcm) noexcept
{
    auto& signal=*pcm.native_signal;
        PCM_SyncFloatEffects(pcm);
        if(pcm.sim_dirty&0x00ffffffu)
            for(unsigned source=0;source<32;++source)
                signal.setPitchSource(source,pcm.ram2[source][0]);
        // Only compatibility control imports remain here. Gate transitions,
        // oscillator restart and first-active-pass readiness belong to signal.
        for(unsigned slot=0;slot<24;++slot) if(pcm.sim_dirty&(1u<<slot)) {
#if defined(SC55_NATIVE_IO_AUDIT)
            ++pcm.auditVoiceImports;
#endif
            PCMSim_SyncVoice(signal.voices,pcm,slot);
            signal.setVoicePitchSource(slot,pcm.ram2[slot][7]&31);
            signal.adoptVoiceGate(slot,bool((pcm.voice_mask&pcm.voice_mask_pending)&(1u<<slot)),
                bool(pcm.ram2[slot][7]&0x20),pcm.ram1[slot][4],pcm.ram2[slot][8],bool(pcm.ram2[slot][8]&0x8000));
        }
        pcm.sim_dirty=0;
}

void PCM_PublishIndependentRender(pcm_t& pcm) noexcept
{
        auto& signal=*pcm.native_signal;
        pcm.ram2[30][10]=signal.randomWord();
        pcm.tv_counter=signal.envelopePhase();
        PCM_PublishFloatEffects(pcm);
        pcm.native_readback_pending=true;
        pcm.native_readback_update=signal.lastFrameUpdated();
        pcm.nfs=signal.lastFrameUpdated();
        PCM_CommitSimulatedFrame(pcm,-1); // Native renderer owns the event, not a chip IRQ.
        const auto next=signal.pendingMix();
        pcm.accum_l=next.left; pcm.accum_r=next.right;
        pcm.rcsum[0]=next.reverb; pcm.rcsum[1]=next.chorus;
        if(signal.effectsEnabled()) pcm.ram2[31][7]|=0x20;
        pcm.nfs=signal.updatesEnvelopes();
        pcm.cycles=signal.renderedFrames()*625; // Compatibility clock, not the owner.
}

AudioFrame<int32_t> PCM_RenderIndependentFrame(pcm_t& pcm) noexcept
{
    // Full legacy diagnostic path retains the two integer input histories.
    // NativeSynth does not consume these chip-only intermediate values.
    for(unsigned input=0;input<2;++input) {
        const auto coefficients=pcm.ram2[30+input][1];
        const auto a=multi(int32_t(pcm.ram1[29][input]),int8_t(coefficients>>8))>>5;
        const auto b=multi(pcm.rcsum[input],int8_t(coefficients))>>5;
        pcm.ram1[29][input]=uint32_t(addclip20(a>>1,b>>1,(a|b)&1));
    }
    PCM_PrepareIndependentRender(pcm);
    const auto output=pcm.native_signal->nextFrame();
    PCM_PublishIndependentRender(pcm);
    return output;
}

// Integer voice arithmetic shared by the chip path and the upcoming expanded
// native voice bank. Voice memory and pitch source are explicit; effect state
// and the common output buses remain owned by the one PCM instance.
static void PCM_RenderIntegerVoice(pcm_t& pcm, uint32_t* ram1, uint16_t* ram2,
    uint16_t phaseIncrement, unsigned slot, bool key, bool lastVoice,
    const int rcadd[6], const int rcadd2[6])
{
    const bool okey = (ram2[7] & 0x20) != 0;

    const bool active = okey && key;
    const bool kon = key && !okey;

    int sampl=0, sampr=0, rc0=0, rc1=0;
    int newnibble=0, old_nibble=0;
    bool usenew=false;
    if (pcm.skip_inactive_voices && pcm.is_mk1 && (slot<24 || pcm.native_voice_count) && !key && pcm.nfs)
    {
        // A disabled slot contributes exactly zero to all four buses.
        // Its sample/filter history and gain readback are cleared by
        // the common epilogue. Cutoff still advances independently.
        // Key-on and non-updating passes retain the full pipeline.
        calc_tv(pcm, 2, ram2[5], &ram2[11], false, nullptr);
    }
    else
    {
    // address generator

    bool b15 = (ram2[8] & 0x8000) != 0; // 0
    const bool b6 = (ram2[7] & 0x40) != 0; // 1
    const bool b7 = (ram2[7] & 0x80) != 0; // 1
    int hiaddr = (ram2[7] >> 8) & 15; // 1
    old_nibble = (ram2[7] >> 12) & 15; // 1

    int address = (int)ram1[4]; // 0
    int address_end = (int)ram1[0]; // 1 or 2
    int address_loop = (int)ram1[2]; // 2 or 1

    int cmp1 = b15 ? address_loop : address_end;
    int cmp2 = address;
    const bool nibble_cmp1 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); // 2
    bool irq_flag = 0;

    // fixme:
    if (kon)
        irq_flag = ((cmp1 + address_loop) & 0x100000) != 0;
    else
        irq_flag = ((address + ((-address_loop) & 0xfffff)) & 0x100000) != 0;
    irq_flag ^= b7;

    int nibble_address = (!b6 && nibble_cmp1) ? address_loop : address; // 3
    const bool address_b4 = (nibble_address & 0x10) != 0;
    int wave_address = nibble_address >> 5;
    const bool xor2 = (address_b4 ^ b7);
    const bool check1 = xor2 && active;
    const bool xor1 = (b15 ^ !nibble_cmp1);
    const bool nibble_add = b6 ? check1 && xor1 : (!nibble_cmp1 && check1);
    const bool nibble_subtract = b6 && !xor1 && active && !xor2;
    if (b7)
        wave_address -= nibble_add - nibble_subtract;
    else
        wave_address += nibble_add - nibble_subtract;
    wave_address &= 0xfffff;

    newnibble = PCM_ReadROM(pcm, (uint32_t)((hiaddr << 20) | wave_address));
    const bool newnibble_sel = address_b4 ^ ((b6 || !nibble_cmp1) && okey);
    if (newnibble_sel)
        newnibble = (newnibble >> 4) & 15;
    else
        newnibble &= 15;

    int sub_phase = (ram2[8] & 0x3fff); // 1
    int interp_ratio = (sub_phase >> 7) & 127;
    sub_phase += phaseIncrement; // 5
    int sub_phase_of = (sub_phase >> 14) & 7;
    if (pcm.nfs)
    {
        ram2[8] &= ~0x3fff;
        ram2[8] |= sub_phase & 0x3fff;
    }


    // address 0
    int address_cnt = address;
    int samp0 = (int8_t)PCM_ReadROM(pcm, (uint32_t)((hiaddr << 20) | address_cnt)); // 18

    cmp1 = address;
    cmp2 = address_cnt;
    const bool nibble_cmp2 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); // 8
    cmp1 = b15 ? address_loop : address_end;
    cmp2 = address_cnt;
    bool address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); // 9

    int next_address = address_cnt; // 11
    usenew = !nibble_cmp2;
    bool next_b15 = b15;

    cmp1 = (!b6 && address_cmp) ? address_loop : address_cnt;
    cmp2 = address_cnt;
    int address_cnt2 = (kon || (!b6 && address_cmp)) ? cmp1 : cmp2;

    bool address_add = (!address_cmp && b6 && !b15) || (!address_cmp && !b6);
    bool address_sub = !address_cmp && b6 && b15;
    if (b7)
        address_cnt2 -= address_add - address_sub;
    else
        address_cnt2 += address_add - address_sub;
    address_cnt = address_cnt2 & 0xfffff; // 11
    b15 = b6 && (b15 ^ address_cmp); // 11

    int samp1 = (int8_t)PCM_ReadROM(pcm, (uint32_t)((hiaddr << 20) | address_cnt)); // 20

    cmp1 = address;
    cmp2 = address_cnt;
    const bool nibble_cmp3 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); // 12
    cmp1 = b15 ? address_loop : address_end;
    cmp2 = address_cnt;
    address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); // 13

    if (sub_phase_of >= 1)
    {
        next_address = address_cnt; // 13
        usenew = !nibble_cmp3;
        next_b15 = b15;
    }

    cmp1 = (!b6 && address_cmp) ? address_loop : address_cnt;
    cmp2 = address_cnt;
    address_cnt2 = (kon || (!b6 && address_cmp)) ? cmp1 : cmp2;

    address_add = (!address_cmp && b6 && !b15) || (!address_cmp && !b6);
    address_sub = !address_cmp && b6 && b15;
    if (b7)
        address_cnt2 -= address_add - address_sub;
    else
        address_cnt2 += address_add - address_sub;
    address_cnt = address_cnt2 & 0xfffff; // 15
    b15 = b6 && (b15 ^ address_cmp); // 15

    int samp2 = (int8_t)PCM_ReadROM(pcm, (uint32_t)((hiaddr << 20) | address_cnt)); // 1

    cmp1 = address;
    cmp2 = address_cnt;
    const bool nibble_cmp4 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); // 16
    cmp1 = b15 ? address_loop : address_end;
    cmp2 = address_cnt;
    address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); // 17

    if (sub_phase_of >= 2)
    {
        next_address = address_cnt; // 17
        usenew = !nibble_cmp4;
        next_b15 = b15;
    }

    cmp1 = (!b6 && address_cmp) ? address_loop : address_cnt;
    cmp2 = address_cnt;
    address_cnt2 = (kon || (!b6 && address_cmp)) ? cmp1 : cmp2;

    address_add = (!address_cmp && b6 && !b15) || (!address_cmp && !b6);
    address_sub = !address_cmp && b6 && b15;
    if (b7)
        address_cnt2 -= address_add - address_sub;
    else
        address_cnt2 += address_add - address_sub;
    address_cnt = address_cnt2 & 0xfffff; // 19
    b15 = b6 && (b15 ^ address_cmp); // 19

    int samp3 = (int8_t)PCM_ReadROM(pcm, (uint32_t)((hiaddr << 20) | address_cnt)); // 5

    cmp1 = address;
    cmp2 = address_cnt;
    const bool nibble_cmp5 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); // 20
    cmp1 = b15 ? address_loop : address_end;
    cmp2 = address_cnt;
    address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); // 21

    if (sub_phase_of >= 3)
    {
        next_address = address_cnt; // 21
        usenew = !nibble_cmp5;
        next_b15 = b15;
    }

    cmp1 = (!b6 && address_cmp) ? address_loop : address_cnt;
    cmp2 = address_cnt;
    address_cnt2 = (kon || (!b6 && address_cmp)) ? cmp1 : cmp2;

    address_add = (!address_cmp && b6 && !b15) || (!address_cmp && !b6);
    address_sub = !address_cmp && b6 && b15;
    if (b7)
        address_cnt2 -= address_add - address_sub;
    else
        address_cnt2 += address_add - address_sub;
    address_cnt = address_cnt2 & 0xfffff; // 23
    // b15 = b6 && (b15 ^ address_cmp); // 23

    cmp1 = address;
    cmp2 = address_cnt;
    const bool nibble_cmp6 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); // 24

    if (sub_phase_of >= 4)
    {
        next_address = address_cnt; // 1
        usenew = !nibble_cmp6;
        // b15 is not updated?
    }

    if (active && pcm.nfs)
        ram1[4] = (uint32_t)next_address;

    if (pcm.nfs)
    {
        ram2[8] &= ~0x8000;
        ram2[8] |= (uint16_t)(next_b15 << 15);
    }

    // dpcm

    // 18
    int reference = (int)ram1[5];

    // 19
    int preshift = samp0 << 10;
    int select_nibble = nibble_cmp2 ? old_nibble : newnibble;
    int shift = (10 - select_nibble) & 15;

    int shifted = (preshift << 1) >> shift;

    if (sub_phase_of >= 1)
        reference = addclip20(reference, shifted >> 1, shifted & 1);

    preshift = samp1 << 10;
    select_nibble = nibble_cmp3 ? old_nibble : newnibble;
    shift = (10 - select_nibble) & 15;

    shifted = (preshift << 1) >> shift;

    if (sub_phase_of >= 2)
        reference = addclip20(reference, shifted >> 1, shifted & 1);

    preshift = samp2 << 10;
    select_nibble = nibble_cmp4 ? old_nibble : newnibble;
    shift = (10 - select_nibble) & 15;

    shifted = (preshift << 1) >> shift;

    if (sub_phase_of >= 3)
        reference = addclip20(reference, shifted >> 1, shifted & 1);

    preshift = samp3 << 10;
    select_nibble = nibble_cmp5 ? old_nibble : newnibble;
    shift = (10 - select_nibble) & 15;

    shifted = (preshift << 1) >> shift;

    if (sub_phase_of >= 4)
        reference = addclip20(reference, shifted >> 1, shifted & 1);

    // interpolation

    int test = (int)ram1[5];

    int step0 = multi(interp_lut[0][interp_ratio] << 6, (int8_t)samp0) >> 8;
    select_nibble = nibble_cmp2 ? old_nibble : newnibble;
    shift = (10 - select_nibble) & 15;
    step0 =  (step0 << 1) >> shift;

    test = addclip20(test, step0 >> 1, step0 & 1);


    int step1 = multi(interp_lut[1][interp_ratio] << 6, (int8_t)samp1) >> 8;
    select_nibble = nibble_cmp3 ? old_nibble : newnibble;
    shift = (10 - select_nibble) & 15;
    step1 = (step1 << 1) >> shift;

    test = addclip20(test, step1 >> 1, step1 & 1);

    int step2 = multi(interp_lut[2][interp_ratio] << 6, (int8_t)samp2) >> 8;
    select_nibble = nibble_cmp4 ? old_nibble : newnibble;
    shift = (10 - select_nibble) & 15;
    step2 = (step2 << 1) >> shift;

    int reg1 = (int)ram1[1];
    int reg3 = (int)ram1[3];
    int reg2_6 = (ram2[6] >> 8) & 127;

    test = addclip20(test, step2 >> 1, step2 & 1);

    int filter = ram2[11];
    int v3;

    if (pcm.is_mk1)
    {
        int mult1 = multi(reg1, (int8_t)(filter >> 8)); // 8
        int mult2 = multi(reg1, (int8_t)((filter >> 1) & 127)); // 9
        int mult3 = multi(reg1, (int8_t)reg2_6); // 10

        int v2 = addclip20(reg3, mult1 >> 6, (mult1 >> 5) & 1); // 9
        int v1 = addclip20(v2, mult2 >> 13, (mult2 >> 12) & 1); // 10
        int subvar = addclip20(v1, (mult3 >> 6), (mult3 >> 5) & 1); // 11

        ram1[3] = (uint32_t)v1;

        v3 = addclip20(test, subvar ^ 0xfffff, 1); // 12

        int mult4 = multi(v3, (int8_t)(filter >> 8));
        int mult5 = multi(v3, (int8_t)((filter >> 1) & 127));
        int v4 = addclip20(reg1, mult4 >> 6, (mult4 >> 5) & 1); // 14
        int v5 = addclip20(v4, mult5 >> 13, (mult5 >> 12) & 1); // 15

        ram1[1] = (uint32_t)v5;
    }
    else
    {
        // hack: use 32-bit math to avoid overflow
        int mult1 = reg1 * (int8_t)(filter >> 8); // 8
        int mult2 = reg1 * (int8_t)((filter >> 1) & 127); // 9
        int mult3 = reg1 * (int8_t)reg2_6; // 10

        int v2 = reg3 + (mult1 >> 6) + ((mult1 >> 5) & 1); // 9
        int v1 = v2 + (mult2 >> 13) + ((mult2 >> 12) & 1); // 10
        int subvar = v1 + (mult3 >> 6) + ((mult3 >> 5) & 1); // 11

        ram1[3] = (uint32_t)v1;

        int tests = test;
        tests <<= 12;
        tests >>= 12;

        v3 = tests - subvar; // 12

        int mult4 = v3 * (int8_t)(filter >> 8);
        int mult5 = v3 * (int8_t)((filter >> 1) & 127);
        int v4 = reg1 + (mult4 >> 6) + ((mult4 >> 5) & 1); // 14
        int v5 = v4 + (mult5 >> 13) + ((mult5 >> 12) & 1); // 15

        ram1[1] = (uint32_t)v5;
    }


    ram1[5] = (uint32_t)reference;

    if (active && (ram2[6] & 1) != 0 && (ram2[8] & 0x4000) == 0 && !pcm.irq_assert && irq_flag)
    {
        //fprintf(stderr, "irq voice %i\n", slot);
        if (pcm.nfs)
            ram2[8] |= 0x4000;
        pcm.irq_assert = true;
        pcm.irq_channel = (uint8_t)slot;
        pcm.postIrq(true);
    }

    int volmul1 = 0;
    int volmul2 = 0;

    calc_tv(pcm, 0, ram2[3], &ram2[9], active, &volmul1);
    calc_tv(pcm, 1, ram2[4], &ram2[10], active, &volmul2);
    calc_tv(pcm, 2, ram2[5], &ram2[11], active, NULL);

    // if (volmul1 && volmul2)
    //     volmul1 += 0;

    int sample = (ram2[6] & 2) == 0 ? (int)ram1[3] : v3;
    //sample = test;

    int multiv1 = multi(sample, (int8_t)(volmul1 >> 8));
    int multiv2 = multi(sample, (int8_t)((volmul1 >> 1) & 127));

    int sample2 = addclip20(multiv1 >> 6, multiv2 >> 13, ((multiv2 >> 12) | (multiv1 >> 5)) & 1);

    int multiv3 = multi(sample2, (int8_t)(volmul2 >> 8));
    int multiv4 = multi(sample2, (int8_t)((volmul2 >> 1) & 127));

    int sample3 = addclip20(multiv3 >> 6, multiv4 >> 13, ((multiv4 >> 12) | (multiv3 >> 5)) & 1);

    int pan = active ? ram2[1] : 0;
    int rc = active ? ram2[2] : 0;

    sampl = multi(sample3, (int8_t)((pan >> 8) & 255));
    sampr = multi(sample3, (int8_t)((pan >> 0) & 255));

    rc0 = multi(sample3, (int8_t)((rc >> 8) & 255)) >> 5; // reverb
    rc1 = multi(sample3, (int8_t)((rc >> 0) & 255)) >> 5; // chorus
    }

    // mix reverb/chorus?
    int slot2 = lastVoice ? 31 : (slot == 30 ? -1 : int(slot + 1));
    switch (slot2)
    {
        // 17, 18 - reverb

        case 17:
            pcm.ram1[31][1] = (uint32_t)addclip20((int32_t)pcm.ram1[31][1], rcadd[0] >> 1, rcadd[0] & 1);
            break;
        case 18:
            pcm.ram1[31][3] = (uint32_t)addclip20((int32_t)pcm.ram1[31][3], rcadd[1] >> 1, rcadd[1] & 1);
            break;
        case 21:
            pcm.ram1[31][1] = (uint32_t)addclip20((int32_t)pcm.ram1[31][1], rcadd[2] >> 1, rcadd[2] & 1);
            break;
        case 22:
            pcm.ram1[31][3] = (uint32_t)addclip20((int32_t)pcm.ram1[31][3], rcadd[3] >> 1, rcadd[3] & 1);
            break;
        case 23:
            pcm.ram1[31][1] = (uint32_t)addclip20((int32_t)pcm.ram1[31][1], rcadd[4] >> 1, rcadd[4] & 1);
            break;
        case 31:
            pcm.ram1[31][3] = (uint32_t)addclip20((int32_t)pcm.ram1[31][3], rcadd[5] >> 1, rcadd[5] & 1);
            break;
    }

    int32_t suml = addclip20((int32_t)pcm.ram1[31][1], sampl >> 6, (sampl >> 5) & 1);
    int32_t sumr = addclip20((int32_t)pcm.ram1[31][3], sampr >> 6, (sampr >> 5) & 1);

    switch (slot2)
    {
        case 17:
            pcm.rcsum[1] = addclip20(pcm.rcsum[1], rcadd2[0] >> 1, rcadd2[0] & 1);
            break;
        case 18:
            pcm.rcsum[1] = addclip20(pcm.rcsum[1], rcadd2[1] >> 1, rcadd2[1] & 1);
            break;
        case 21:
            pcm.rcsum[0] = addclip20(pcm.rcsum[0], rcadd2[2] >> 1, rcadd2[2] & 1);
            break;
        case 22:
            pcm.rcsum[1] = addclip20(pcm.rcsum[1], rcadd2[3] >> 1, rcadd2[3] & 1);
            break;
        case 23:
            pcm.rcsum[0] = addclip20(pcm.rcsum[0], rcadd2[4] >> 1, rcadd2[4] & 1);
            break;
        case 31:
            pcm.rcsum[1] = addclip20(pcm.rcsum[1], rcadd2[5] >> 1, rcadd2[5] & 1);
            break;
    }

    pcm.rcsum[0] = addclip20(pcm.rcsum[0], rc0 >> 1, rc0 & 1);
    pcm.rcsum[1] = addclip20(pcm.rcsum[1], rc1 >> 1, rc1 & 1);

    if (!lastVoice)
    {
        pcm.ram1[31][1] = (uint32_t)suml;
        pcm.ram1[31][3] = (uint32_t)sumr;
    }
    else
    {
        pcm.accum_l = suml;
        pcm.accum_r = sumr;
    }

    if (key && pcm.nfs)
    {
        ram2[7] &= ~0xf020;
        ram2[7] |= (uint16_t)(((usenew || kon) ? newnibble : old_nibble) << 12);

        // update key
        ram2[7] |= (uint16_t)(key << 5);
    }

    if (!active)
    {
        if (pcm.nfs)
        {
            ram1[1] = 0;
            ram1[3] = 0;
            ram1[5] = 0;
        }

        ram2[8] = 0;
        ram2[9] = 0;
        ram2[10] = 0;
    }
}

void PCM_Update(pcm_t& pcm, uint64_t cycles, bool stopOnInterrupt)
{
    if(pcm.native_signal) {
        while(pcm.cycles<cycles) {
            pcm.postSample(PCM_RenderIndependentFrame(pcm));
            if(stopOnInterrupt && PCM_HasVoiceBoundary(pcm)) break;
        }
        return;
    }
    while (pcm.cycles < cycles)
    {
        const uint32_t voice_active = pcm.voice_mask & pcm.voice_mask_pending;
        // This clock is also read by firmware/native modulation, not just DAC
        // dither. Advancing it must not depend on the selected voice renderer.
        int shifter = pcm.ram2[30][10];
        int xr = ((shifter >> 0) ^ (shifter >> 1) ^ (shifter >> 7) ^ (shifter >> 12)) & 1;
        shifter = (shifter >> 1) | (xr << 15);
        pcm.ram2[30][10] = (uint16_t)shifter;
        if (pcm.use_simulation)
        {
            // The output stage below squeezes the 20 bit mix into 16 by feeding
            // the truncation error back and dithering it -- a way to get more
            // out of a cheap DAC, and a problem that does not exist once the
            // mix leaves here as float. So: one frame per pass, straight off
            // the bus, no dither, no DC bias, and no oversampling to carry the
            // shaped noise. The output rate halves accordingly; see
            // PCM_GetOutputFrequency.
            pcm.postSample({pcm.accum_l << 12, pcm.accum_r << 12});
            if (pcm.enable_oversampling && pcm.config.oversampling)
            {
                xr = ((shifter >> 0) ^ (shifter >> 1) ^ (shifter >> 7) ^ (shifter >> 12)) & 1;
                pcm.ram2[30][10] = (uint16_t)((shifter >> 1) | (xr << 15));
            }
        }
        else
        { // final mixing
            pcm.accum_l = addclip20(pcm.accum_l, (int32_t)pcm.ram1[30][0], 0);
            pcm.accum_r = addclip20(pcm.accum_r, (int32_t)pcm.ram1[30][1], 0);

            pcm.ram1[30][2] = (uint32_t)addclip20(pcm.accum_l,
                (int32_t)(pcm.config.orval | (shifter & pcm.config.noise_mask)), 0);

            pcm.ram1[30][4] = (uint32_t)addclip20(pcm.accum_r,
                (int32_t)(pcm.config.orval | (shifter & pcm.config.noise_mask)), 0);

            pcm.ram1[30][0] = (uint32_t)(pcm.accum_l & pcm.config.write_mask);
            pcm.ram1[30][1] = (uint32_t)(pcm.accum_r & pcm.config.write_mask);
            

            int32_t samp_l = (int32_t)((pcm.ram1[30][2] & (uint32_t)(~pcm.config.write_mask)) << 12);
            int32_t samp_r = (int32_t)((pcm.ram1[30][4] & (uint32_t)(~pcm.config.write_mask)) << 12);

            pcm.postSample({samp_l, samp_r});

            xr = ((shifter >> 0) ^ (shifter >> 1) ^ (shifter >> 7) ^ (shifter >> 12)) & 1;
            shifter = (shifter >> 1) | (xr << 15);

            pcm.accum_l = addclip20(pcm.accum_l, (int32_t)pcm.ram1[30][0], 0);
            pcm.accum_r = addclip20(pcm.accum_r, (int32_t)pcm.ram1[30][1], 0);

            pcm.ram1[30][3] = (uint32_t)addclip20(pcm.accum_l,
                (int32_t)(pcm.config.orval | (shifter & pcm.config.noise_mask)), 0);

            pcm.ram1[30][5] = (uint32_t)addclip20(pcm.accum_r,
                (int32_t)(pcm.config.orval | (shifter & pcm.config.noise_mask)), 0);

            if (pcm.enable_oversampling && pcm.config.oversampling) // oversampling
            {
                pcm.ram2[30][10] = (uint16_t)shifter;

                pcm.ram1[30][0] = (uint32_t)(pcm.accum_l & pcm.config.write_mask);
                pcm.ram1[30][1] = (uint32_t)(pcm.accum_r & pcm.config.write_mask);


                samp_l = (int32_t)((pcm.ram1[30][3] & (uint32_t)(~pcm.config.write_mask)) << 12);
                samp_r = (int32_t)((pcm.ram1[30][5] & (uint32_t)(~pcm.config.write_mask)) << 12);

                pcm.postSample({samp_l, samp_r});
            }
        }

        { // global counter for envelopes
            if (!pcm.nfs)
                pcm.tv_counter = pcm.ram2[31][8]; // fixme

            pcm.tv_counter -= 1;

            pcm.tv_counter &= 0x3fff;
        }

        // chorus/reverb

        if(!pcm.use_float_effects) { // Legacy interpolation readback.
            if (pcm.ram2[31][8] & 0x8000)
                pcm.ram2[31][9] = pcm.ram2[31][8] & 0x7fff;
            else
                pcm.ram2[31][10] = pcm.ram2[31][8] & 0x7fff;

            if ((0x4000 - pcm.ram2[31][8]) & 0x8000)
                pcm.ram2[31][10] = (0x4000 - pcm.ram2[31][8]) & 0x7fff;
            else
                pcm.ram2[31][9] = (0x4000 - pcm.ram2[31][8]) & 0x7fff;
        }

        {
            int v1 = pcm.ram2[31][1];

            int m1 = multi((int32_t)pcm.ram1[29][1], (int8_t)(v1 >> 8)) >> 5; // 14
            int m2 = multi(pcm.rcsum[1], (int8_t)(v1 & 255)) >> 5; // 15

            pcm.ram1[29][1] = (uint32_t)addclip20(m1 >> 1, m2 >> 1, (m1 | m2) & 1); // 16
        }

        if(!pcm.use_float_effects) {
            const bool okey = (pcm.ram2[31][7] & 0x20) != 0;
            const bool key = 1;
            const bool active = okey && key;
            sc55::EnvelopeRamp spread{pcm.ram2[30][0],pcm.ram2[30][9]};
            spread.advance(sc55::EnvelopeRamp::Stage::secondGain,{pcm.tv_counter,pcm.nfs},active);
            pcm.ram2[30][9]=spread.level;
        }

        {
            int v1 = pcm.ram2[30][1];
            int m1 = multi((int32_t)pcm.ram1[29][0], (int8_t)(v1 >> 8)) >> 5; // 17
            int m2 = multi(pcm.rcsum[0], (int8_t)(v1 & 255)) >> 5; // 18

            pcm.ram1[29][0] = (uint32_t)addclip20(m1 >> 1, m2 >> 1, (m1 | m2) & 1); // 19
        }

        int rcadd[6] = {};
        int rcadd2[6] = {};

        // エフェクトは帰還を持つので、入力が止まっても尾が回り続ける。「入力がゼロ」
        // だけでは飛ばせない（実際それで音が変わった）。正しい条件は「遅延メモリへの
        // 書き込みが 1 周ぶんすべてゼロだった」で、tv_counter が 0..0x3fff を巡る
        // あいだに書きタップは全アドレスを覆うから、そのときメモリ全域がゼロになる。
        //
        // ゼロのメモリにゼロを入れれば出力もゼロなので、そこから先は丸ごと飛ばせる。
        //
        // これは実機の無駄を削っているのではない。チップ側のエフェクトは回路で、
        // 入力があろうと無かろうと 32 kHz で動いているだけで、そこに無駄という概念は
        // 無い。省けるのは「その回路を書き写したこのコードを、結果が必ずゼロだと
        // 分かっている区間でホスト CPU に実行させること」のほう。
        //
        // 効きも限定的で、リバーブを使う曲では遅延メモリがゼロになる瞬間がほぼ無い。
        // 実測: 送りゼロの素材で 4.4%、リバーブを使う実曲では 0.6〜0.8%。
        if (pcm.tv_counter == 0)
        {
            pcm.eram_silent = pcm.eram_wrote_nonzero ? 0 : 1;
            pcm.eram_wrote_nonzero = 0;
        }

        if (pcm.rcsum[0] != 0 || pcm.rcsum[1] != 0)
            pcm.eram_silent = 0;

        if (pcm.use_float_effects)
        {
            float fa[6] = {}, fb[6] = {};
            PCM_SyncFloatEffects(pcm);
            auto& fx=*pcm.effects;
            fx.process({pcm.tv_counter,pcm.nfs},(pcm.ram2[31][7]&0x20)!=0,
                float(pcm.rcsum[0]),float(pcm.rcsum[1]),fa,fb);
            PCM_PublishFloatEffects(pcm);
            for (int i = 0; i < 6; ++i)
            {
                rcadd[i]  = (int) lrintf(fa[i]);
                rcadd2[i] = (int) lrintf(fb[i]);
            }
        }
        else if (!pcm.eram_silent)
        {
            {
                // 1
                int v1 = pcm.ram2[30][4];
                int m1 = multi((int32_t)pcm.ram1[29][0], (int8_t)(v1 >> 8)) >> 6;
                int v2 = 0;
                int s1 = eram_unpack(pcm, pcm.ram2[28][1] + pcm.tv_counter, 1);
                int s2 = eram_unpack(pcm, pcm.ram2[28][1] + pcm.tv_counter);
                if ((v1 & 0x30) != 0)
                {
                    v2 = s1;
                }
                int v3 = addclip20(m1, v2 ^ 0xfffff, 1);
                pcm.ram1[29][4] = (uint32_t)v3;
                int m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm.ram1[29][5] = (uint32_t)addclip20(m2 >> 1, s2, m2 & 1);
            }
            {
                // 2
                int v1 = pcm.ram2[30][4];
                int v2 = 0;
                int s1 = eram_unpack(pcm, pcm.ram2[28][2] + pcm.tv_counter, 1);
                int s2 = eram_unpack(pcm, pcm.ram2[28][2] + pcm.tv_counter);
                if ((v1 & 0x30) != 0)
                {
                    v2 = s1;
                }
                int v3 = addclip20((int32_t)pcm.ram1[29][5], v2 ^ 0xfffff, 1);
                pcm.ram1[29][5] = (uint32_t)v3;
                int m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm.ram1[28][0] = (uint32_t)addclip20(m2 >> 1, s2, m2 & 1);
            }
            {
                // 3
                int v1 = pcm.ram2[30][4];
                int v2 = 0;
                int s1 = eram_unpack(pcm, pcm.ram2[28][3] + pcm.tv_counter, 1);
                int s2 = eram_unpack(pcm, pcm.ram2[28][3] + pcm.tv_counter);
                if ((v1 & 0x30) != 0)
                {
                    v2 = s1;
                }
                int v3 = addclip20((int32_t)pcm.ram1[28][0], v2 ^ 0xfffff, 1);
                pcm.ram1[28][0] = (uint32_t)v3;
                int m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm.ram1[28][1] = (uint32_t)addclip20(m2 >> 1, s2, m2 & 1);


                pcm.ram1[28][2] = (uint32_t)eram_unpack(pcm, pcm.ram2[28][5] + pcm.tv_counter);
            }
            {
                // 4
                int v1 = pcm.ram2[30][5];
                int v2 = 0;
                int s1 = eram_unpack(pcm, pcm.ram2[28][4] + pcm.tv_counter, 1);
                int s2 = eram_unpack(pcm, pcm.ram2[28][4] + pcm.tv_counter);
                if ((v1 & 0x30) != 0)
                {
                    v2 = s1;
                }
                int v3 = addclip20((int32_t)pcm.ram1[28][1], v2 ^ 0xfffff, 1);
                pcm.ram1[28][1] = (uint32_t)v3;
                int m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm.ram1[28][3] = (uint32_t)addclip20(m2 >> 1, s2, m2 & 1);


                pcm.ram1[28][4] = (uint32_t)eram_unpack(pcm, pcm.ram2[29][1] + pcm.tv_counter);
            }
            {
                // 5

                int v1 = pcm.ram2[30][7];
                int m1 = multi((int32_t)pcm.ram1[29][2], (int8_t)(v1 >> 8)) >> 5;
                int s1 = eram_unpack(pcm, pcm.ram2[29][0] + pcm.tv_counter);
                int m2 = multi(s1, (int8_t)(v1 & 255)) >> 5;
                pcm.ram1[29][2] = (uint32_t)addclip20(m1 >> 1, m2 >> 1, (m1 | m2) & 1);

                eram_pack(pcm, pcm.ram2[28][0] + pcm.tv_counter, pcm.ram1[29][4]);
            }
            {
                // 6

                int v1 = pcm.ram2[30][8];
                int m1 = multi((int32_t)pcm.ram1[29][3], (int8_t)(v1 >> 8)) >> 5;
                int s1 = eram_unpack(pcm, pcm.ram2[29][8] + pcm.tv_counter);
                int m2 = multi(s1, (int8_t)(v1 & 255)) >> 5;
                pcm.ram1[29][3] = (uint32_t)addclip20(m1 >> 1, m2 >> 1, (m1 | m2) & 1);

                eram_pack(pcm, pcm.ram2[28][1] + pcm.tv_counter, pcm.ram1[29][5]);

                eram_pack(pcm, pcm.ram2[28][2] + pcm.tv_counter, pcm.ram1[28][0]);
            }
            {
                // 7

                int v1 = pcm.ram2[30][9];
                int v2 = (int)pcm.ram1[28][3];
                int m1 = multi((int32_t)pcm.ram1[29][2], (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi((int32_t)pcm.ram1[29][3], (int8_t)(v1 >> 8)) >> 5;
                pcm.ram1[28][3] = (uint32_t)addclip20(v2, m1 >> 1, m1 & 1);
                pcm.ram1[28][5] = (uint32_t)addclip20(v2, m2 >> 1, m2 & 1);

                eram_pack(pcm, pcm.ram2[28][3] + pcm.tv_counter, pcm.ram1[28][1]);
            }
            {
                // 8

                int v1 = pcm.ram2[30][6];
                int m1 = multi((int32_t)pcm.ram1[28][2], (int8_t)(v1 >> 8)) >> 5;

                int v2 = addclip20((int32_t)pcm.ram1[28][3], m1 >> 1, m1 & 1);
                pcm.ram1[28][3] = (uint32_t)v2;
                int m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;
                pcm.ram1[28][2] = (uint32_t)addclip20((int32_t)pcm.ram1[28][2], m2 >> 1, m2 & 1);


                pcm.ram1[28][1] = (uint32_t)eram_unpack(pcm, pcm.ram2[28][9] + pcm.tv_counter);
            }
            {
                // 9

                int v1 = pcm.ram2[30][6];
                int m1 = multi((int32_t)pcm.ram1[28][4], (int8_t)(v1 >> 8)) >> 5;

                int v2 = addclip20((int32_t)pcm.ram1[28][5], m1 >> 1, m1 & 1);
                pcm.ram1[28][5] = (uint32_t)v2;
                int m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;
                pcm.ram1[28][4] = (uint32_t)addclip20((int32_t)pcm.ram1[28][4], m2 >> 1, m2 & 1);


                pcm.ram1[29][4] = (uint32_t)eram_unpack(pcm, pcm.ram2[29][5] + pcm.tv_counter);
            }
            {
                // 10

                int v1 = pcm.ram2[30][6];
                int v2 = (int)pcm.ram1[28][1];
                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int s1 = eram_unpack(pcm, pcm.ram2[28][8] + pcm.tv_counter);
                int v3 = addclip20(m1 >> 1, s1, m1 & 1);
                pcm.ram1[28][1] = (uint32_t)v3;
                int m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm.ram1[29][5] = (uint32_t)addclip20(m2 >> 1, v2, m2 & 1);

                eram_pack(pcm, pcm.ram2[28][4] + pcm.tv_counter, pcm.ram1[28][3]);
            }
            {
                // 11

                int v1 = pcm.ram2[30][6];
                int v2 = (int)pcm.ram1[29][4];
                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int s1 = eram_unpack(pcm, pcm.ram2[29][4] + pcm.tv_counter);
                int v3 = addclip20(m1 >> 1, s1, m1 & 1);
                pcm.ram1[29][4] = (uint32_t)v3;
                int m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm.ram1[28][0] = (uint32_t)addclip20(m2 >> 1, v2, m2 & 1);


                eram_pack(pcm, pcm.ram2[28][5] + pcm.tv_counter, pcm.ram1[28][2]);

                eram_pack(pcm, pcm.ram2[29][0] + pcm.tv_counter, pcm.ram1[28][5]);
            }
            {
                // 12

                pcm.ram1[28][5] = (uint32_t)eram_unpack(pcm, pcm.ram2[28][6] + pcm.tv_counter);
            }

            {
                // 13

                int s1 = eram_unpack(pcm, pcm.ram2[28][10] + pcm.tv_counter);
                pcm.ram1[28][5] = (uint32_t)addclip20((int32_t)pcm.ram1[28][5], s1, 0);

                pcm.ram1[28][2] = (uint32_t)eram_unpack(pcm, pcm.ram2[29][2] + pcm.tv_counter);
            }

            {
                // 14

                int s1 = eram_unpack(pcm, pcm.ram2[29][6] + pcm.tv_counter);
                int t1 = addclip20(s1, (int32_t)pcm.ram1[28][2], 0); // 6

                pcm.ram1[28][5] = (uint32_t)addclip20(t1, (int32_t)pcm.ram1[28][5], 0);

                pcm.ram1[28][2] = (uint32_t)eram_unpack(pcm, pcm.ram2[28][7] + pcm.tv_counter);
            }

            {
                // 15

                int s1 = eram_unpack(pcm, pcm.ram2[28][11] + pcm.tv_counter);
                pcm.ram1[28][2] = (uint32_t)addclip20((int32_t)pcm.ram1[28][2], s1, 0);

                pcm.ram1[28][3] = (uint32_t)eram_unpack(pcm, pcm.ram2[29][3] + pcm.tv_counter);
            }

            {
                // 16

                int s1 = eram_unpack(pcm, pcm.ram2[29][7] + pcm.tv_counter);
                int t1 = addclip20(s1, (int32_t)pcm.ram1[28][2], 0);
                pcm.ram1[28][2] = (uint32_t)addclip20(t1, (int32_t)pcm.ram1[28][3], 0);


                eram_pack(pcm, pcm.ram2[29][1] + pcm.tv_counter, pcm.ram1[28][4]);

                eram_pack(pcm, pcm.ram2[28][8] + pcm.tv_counter, pcm.ram1[28][1]);
            }

            {
                // 17
                int v1 = pcm.ram2[30][2];
                int v2 = (int)pcm.ram1[28][5];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;

                rcadd[0] = m1;

                rcadd2[0] = multi(v2, (int8_t)(v1 & 255)) >> 5;

                int t1 = eram_unpack(pcm, pcm.ram2[29][10] + pcm.tv_counter + 1); //? 3a6e
                eram_pack(pcm, pcm.ram2[28][9] + pcm.tv_counter, pcm.ram1[29][5]);
                pcm.ram1[29][5] = (uint32_t)t1;
            }

            {
                // 18
                int v1 = pcm.ram2[30][3];
                int v2 = (int)pcm.ram1[28][2];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;

                rcadd[1] = m1;

                rcadd2[1] = multi(v2, (int8_t)(v1 & 255)) >> 5;

                pcm.ram1[28][1] = (uint32_t)eram_unpack(pcm, pcm.ram2[29][11] + pcm.tv_counter + 1); //? 3a1e
            }
            {
                // 19

                int v1 = pcm.ram2[31][9];

                int s1 = eram_unpack(pcm, pcm.ram2[29][10] + pcm.tv_counter); //? 3a6d

                eram_pack(pcm, pcm.ram2[29][4] + pcm.tv_counter, pcm.ram1[29][4]);

                int m1 = multi(s1, (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi((int32_t)pcm.ram1[29][5], (int8_t)(v1 >> 8)) >> 5;

                int t2 = addclip20(s1, (m1 >> 1) ^ 0xfffff, 1);

                pcm.ram1[29][5] = (uint32_t)addclip20(t2, m2 >> 1, m2 & 1);
            }
            {
                // 20

                int v1 = pcm.ram2[31][10];

                int s1 = eram_unpack(pcm, pcm.ram2[29][11] + pcm.tv_counter); //? 3a1d

                eram_pack(pcm, pcm.ram2[29][5] + pcm.tv_counter, pcm.ram1[28][0]);

                int m1 = multi(s1, (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi((int32_t)pcm.ram1[28][1], (int8_t)(v1 >> 8)) >> 5;

                int t2 = addclip20(s1, (m1 >> 1) ^ 0xfffff, 1);

                pcm.ram1[28][1] = (uint32_t)addclip20(t2, m2 >> 1, m2 & 1);

                eram_pack(pcm, pcm.ram2[29][9] + pcm.tv_counter, pcm.ram1[29][1]);
            }
            {
                // 21

                int v1 = pcm.ram2[31][2];
                int v2 = (int)pcm.ram1[29][5];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;

                rcadd[2] = m1;
                rcadd2[2] = m2;
            }
            {
                // 22

                int v1 = pcm.ram2[31][3];
                int v2 = (int)pcm.ram1[29][5];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;

                rcadd[3] = m1;
                rcadd2[3] = m2;
            }
            {
                // 23

                int v1 = pcm.ram2[31][4];
                int v2 = (int)pcm.ram1[28][1];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;

                rcadd[4] = m1;
                rcadd2[4] = m2;
            }
            {
                // 31

                int v1 = pcm.ram2[31][5];
                int v2 = (int)pcm.ram1[28][1];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;

                rcadd[5] = m1;
                rcadd2[5] = m2;

            }
        }

        // コーラスの変調タップを作るアドレス生成器。遅延メモリには触らず、
        // LFO の位相（ram2[31][8]）を進めるので、鳴り止んでいても止めてはいけない。
        // ゲートの内側に置いたままだと位相が凍り、音が戻ったときに違う所から再開して
        // 出力が変わる（実際それでコーラスの試験が 1 件外れた）。
        if(!pcm.use_float_effects) {
            sc55::ChorusOscillator oscillator;
            oscillator.position=pcm.ram1[31][4];
            oscillator.begin=pcm.ram1[31][2]; oscillator.end=pcm.ram1[31][0];
            oscillator.phase=pcm.ram2[31][8]&0x3fff;
            oscillator.increment=pcm.ram2[pcm.ram2[31][7]&31][0];
            oscillator.descending=(pcm.ram2[31][8]&0x8000)!=0;
            oscillator.pingPong=(pcm.ram2[31][7]&0x40)!=0;
            oscillator.reverse=(pcm.ram2[31][7]&0x80)!=0;
            oscillator.advance(pcm.nfs,(pcm.ram2[31][7]&0x20)!=0);
            pcm.ram1[31][4]=oscillator.position;
            pcm.ram2[31][8]=uint16_t((pcm.ram2[31][8]&0x4000)|oscillator.phase
                |(oscillator.descending?0x8000:0));
            pcm.ram2[29][10]=oscillator.leftTap();
            pcm.ram2[29][11]=oscillator.rightTap();
        }

        pcm.ram1[31][1] = 0;
        pcm.ram1[31][3] = 0;
        pcm.rcsum[0] = 0;
        pcm.rcsum[1] = 0;

        if (pcm.use_simulation)
            PCM_UpdateVoicesSimulated(pcm, rcadd, rcadd2);
        else
        {
        const auto voiceCount=pcm.native_voice_count ? pcm.native_voice_count : pcm.config.reg_slots;
        for (unsigned slot = 0; slot < voiceCount; ++slot)
            PCM_RenderIntegerVoice(pcm, pcm.voiceRam1(slot), pcm.voiceRam2(slot),
                pcm.native_voice_count ? pcm.voiceRam2(pcm.native_pitch_source[slot])[0]
                    : pcm.ram2[pcm.ram2[slot][7] & 31][0], slot,
                pcm.native_voice_count ? pcm.native_keys.contains(slot) : bool((voice_active >> slot) & 1),
                slot + 1 == voiceCount, rcadd, rcadd2);
        }

        if (pcm.nfs)
        {
            pcm.ram2[31][7] |= 0x20;
        }

        pcm.nfs = true;

        uint64_t new_cycles = (uint64_t)(pcm.config.reg_slots + 1) * 25;

        pcm.cycles += pcm.is_jv880 ? (new_cycles * 25) / 29 : new_cycles;
        if (stopOnInterrupt && pcm.irq_assert) break;
    }
}

uint32_t PCM_GetOutputFrequency(const pcm_t& pcm)
{
    uint32_t freq = (pcm.is_mk1 || pcm.is_jv880) ? 64000 : 66207;

    // The simulation posts once per pass over the voices rather than twice:
    // the second frame only existed to spread the quantisation noise.
    if (pcm.use_simulation)
        return freq / 2;

    if (pcm.enable_oversampling)
    {
        return freq;
    }
    else
    {
        return freq / 2;
    }
}
