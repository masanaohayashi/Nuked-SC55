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
#pragma once

#include <cstdint>

#include "pcm_sim.h"
#include "audio.h"
#include "sc55_effect_parameter.h"

struct mcu_t;
namespace sc55 { class SignalRenderer; struct ChorusSetup; struct ReverbSetup; }

struct PCM_Config
{
    // config_reg_3c
    uint32_t orval        = 0;
    int      dac_mask     = 0; // unused
    uint8_t  noise_mask   = 0;
    uint8_t  write_mask   = 0;
    bool     oversampling = false;

    // config_reg_3d
    // important that this starts at 1, see derivation in PCM_Write
    uint8_t reg_slots = 1;
};

struct pcm_t
{
#if defined(SC55_NATIVE_IO_AUDIT)
    std::array<uint64_t,64> auditReads{},auditWrites{};
    uint64_t auditVoiceImports=0,auditEffectImports=0;
#endif
    uint32_t ram1[32][8]{};
    uint16_t ram2[32][16]{};
    mcu_t*   mcu                 = nullptr;
    // Device identity and endpoints are independent of firmware execution.
    // mcu is retained only for legacy diagnostic probes.
    bool is_mk1 = false, is_jv880 = false;
    void* output_context = nullptr;
    void (*output_sample)(void*, const AudioFrame<int32_t>&) = nullptr;
    void* irq_context = nullptr;
    void (*output_irq)(void*, bool) = nullptr;

    void postSample(const AudioFrame<int32_t>& frame) const
    { if (output_sample) output_sample(output_context, frame); }
    void postIrq(bool asserted) const
    { if (output_irq) output_irq(irq_context, asserted); }
    uint64_t cycles              = 0;
    // Native startup must retain its initial ramps through the key-latch pass
    // and the following active-voice pass, before periodic control may write.
    uint64_t native_voice_install_cycle[24]{};
    uint32_t voice_mask          = 0; // same size as voice_mask_pending
    uint32_t voice_mask_pending  = 0; // 28 bits wide?
    uint32_t write_latch         = 0; // 20 bits wide?
    uint32_t read_latch          = 0; // 20 bits wide?
    uint32_t wave_read_address   = 0;
    uint16_t tv_counter          = 0; // 14 bits wide?
    uint8_t  wave_byte_latch     = 0;
    uint8_t  select_channel      = 0; // 5 bits wide?
    uint8_t  config_reg_3c       = 0; // SC55:c3 JV880:c0
    uint8_t  config_reg_3d       = 0;
    uint8_t  irq_channel         = 0; // range 1..32
    bool     irq_assert          = 0;
    bool     voice_mask_updating = false;
    bool     nfs                 = false;
    int32_t  accum_l             = 0;
    int32_t  accum_r             = 0;
    int32_t  rcsum[2]{};

    // エフェクトが本当に鳴り止んだかの判定に使う。遅延メモリへの書き込みが 1 周ぶん
    // すべてゼロなら、メモリ全域がゼロだと言える（PCM_Update 参照）。
    uint8_t  eram_wrote_nonzero = 0;
    uint8_t  eram_silent = 0;

    // エフェクトを浮動小数版（pcm_effects.h）で回すかどうか。SC55_FXSIM=1 で有効。
    bool use_float_effects = false;
    bool effects_dirty = true;
    struct PCMEffects* effects = nullptr;

    PCM_Config config{};

    uint16_t eram[0x4000]{};

    uint8_t waverom1[0x200000]{};
    uint8_t waverom2[0x200000]{};
    uint8_t waverom3[0x100000]{};
    uint8_t waverom_card[0x200000]{};
    uint8_t waverom_exp[0x800000]{};

    bool enable_oversampling = true;

    // The voice engine, rewritten as a simulation instead of a transcription of
    // the slot pipeline. Off by default; PCM_UseSimulation turns it on, and the
    // emulated path stays in place so the two can be compared.
    bool use_simulation = false;
    sc55::SignalRenderer* native_signal = nullptr; // Owned by NativeSynth, not by the chip adapter.
    bool native_readback_pending = false;
    bool native_readback_update = false;
    // Diagnostic oracle can retain the full inactive-slot pipeline.
    bool skip_inactive_voices = true;
    PCMSimVoices sim{};

    // Which voices the firmware has written to since the simulation last read
    // their registers. It rewrites a voice about 250 times a second; rebuilding
    // all of them 32000 times a second was most of what the simulation cost.
    uint32_t sim_dirty = 0xffffffffu;
};

void PCM_Write(pcm_t& pcm, uint32_t address, uint8_t data);
void PCM_ApplyVoiceUpdate(pcm_t& pcm,unsigned channel,const sc55::VoiceRenderUpdate& update);
void PCM_SetVoicePitch(pcm_t& pcm,unsigned channel,uint16_t increment) noexcept;
void PCM_InstallVoice(pcm_t& pcm,unsigned channel,const sc55::VoiceRenderStart& start);
void PCM_CommitVoiceKeys(pcm_t& pcm,uint32_t enabled);
bool PCM_CompleteVoiceEnable(pcm_t& pcm,unsigned channel,uint16_t level,uint16_t command);
std::array<uint16_t,2> PCM_VoiceGainLevels(pcm_t& pcm,unsigned channel);
std::array<uint16_t,3> PCM_SynchronizeVoiceEnvelopes(pcm_t& pcm,unsigned channel,
    const std::array<uint16_t,3>& commands,const std::array<uint16_t,3>& levels);
uint8_t PCM_Read(pcm_t& pcm, uint32_t address);
uint8_t PCM_ReadROM(const pcm_t& pcm, uint32_t address);
void PCM_Init(pcm_t& pcm, mcu_t& mcu);
// Native block rendering yields at the end of the pass that raised an IRQ.
// Legacy firmware stepping retains its existing deadline-only behavior.
void PCM_Update(pcm_t& pcm, uint64_t cycles, bool stopOnInterrupt = false);
// Transitional control-state adapter. Render one native frame without chip
// scheduling or output callbacks; the synth owns the destination span.
AudioFrame<int32_t> PCM_RenderIndependentFrame(pcm_t& pcm) noexcept;
// Native controller seam: imports before a render slice, publishes afterwards.
// No chip work is required for individual frames within that slice.
void PCM_PrepareIndependentRender(pcm_t& pcm) noexcept;
void PCM_PublishIndependentRender(pcm_t& pcm) noexcept;
// Materialize the renderer's state only when a legacy control consumer needs it.
void PCM_SynchronizeNativeReadback(pcm_t& pcm) noexcept;
void PCM_ConfigureChorus(pcm_t& pcm,const sc55::ChorusSetup& setup) noexcept;
void PCM_ConfigureReverb(pcm_t& pcm,const sc55::ReverbSetup& setup) noexcept;
void PCM_UpdateEffect(pcm_t& pcm,sc55::EffectParameter parameter,uint16_t value) noexcept;
void PCM_SetChorusMix(pcm_t& pcm,const std::array<uint8_t,3>& mix) noexcept;
void PCM_BeginReverbDrain(pcm_t& pcm) noexcept;
void PCM_SetVoiceRamp(pcm_t& pcm,unsigned channel,sc55::EnvelopeRamp::Stage stage,uint16_t command) noexcept;
uint16_t PCM_VoiceRampLevel(pcm_t& pcm,unsigned channel,sc55::EnvelopeRamp::Stage stage) noexcept;
std::array<uint16_t,2> PCM_PeekVoiceGainLevels(const pcm_t& pcm,unsigned channel) noexcept;
bool PCM_HasVoiceBoundary(const pcm_t& pcm) noexcept;
int PCM_TakeVoiceBoundary(pcm_t& pcm) noexcept;
uint16_t PCM_ControlRandomWord(const pcm_t& pcm) noexcept;

// Swaps the voice slot loop for pcm_sim. Effects and the output stage are
// untouched either way.
void PCM_UseSimulation(pcm_t& pcm, bool enable);
uint32_t PCM_GetOutputFrequency(const pcm_t& pcm);
void PCM_GetConfig(PCM_Config& config, uint8_t config_byte);

// Legacy register adapter; the voice renderer itself has no pcm_t dependency.
// Pulls one slot's register block into the simulation's own form. Cheap; the
// firmware rewrites a voice's parameters at 250 Hz, not per sample.
void PCMSim_SyncVoice(PCMSimVoices& voices, const pcm_t& pcm, int slot);

// Adopts the chip's start position for a voice the firmware has just keyed on
// and clears everything this voice was carrying.
void PCMSim_KeyOn(PCMSimVoices& voices, const pcm_t& pcm, int slot);
