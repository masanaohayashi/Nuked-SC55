// pcm_sim - the SC-55 PCM chip as a simulation rather than an emulation.
//
// pcm.cpp reproduces the mechanism: it walks the chip's slot pipeline one chip
// cycle at a time, with 20 bit saturating adders and the block float packing
// the silicon used. This file reproduces the behaviour instead -- what the chip
// does to the signal -- in float32, structure of arrays, with no
// data-dependent branches in the per-voice path so it vectorises.
//
// Rendering consumes voice state directly: waveform windows, phase, filter,
// envelope outputs and four bus gains. No PCM chip or firmware object is needed.
// The legacy register adapter lives in pcm.cpp, outside this renderer.
//
// Effects (slots 28..31) are not handled here yet; this covers voices 0..23.
#pragma once

#include <cstdint>
#include "sc55_envelope_ramp.h"
#include "sc55_voice_render_update.h"



// Rounded up to a whole number of NEON quads.
inline constexpr int PCM_SIM_MAX_VOICES = 32;

// Resolved waveform memory and playback geometry. The owner keeps the memory
// alive; rendering never resolves banks, loads ROMs or allocates storage.
struct PCMSimWaveform
{
    const uint8_t* samples=nullptr;
    const uint8_t* exponents=nullptr;
    uint32_t sampleMask=0,exponentMask=0;
    int32_t loop=0,end=0,bank=0;
    bool pingPong=false,storedBackwards=false;
};

struct PCMSimVoices
{
    // Owned ramp commands and readback. Direct native callers need no chip RAM.
    sc55::VoiceEnvelopes envelopes[PCM_SIM_MAX_VOICES];
    // --- wave read position -------------------------------------------------
    // The phase stays integer, in the chip's own 14 bit units. It is an index,
    // not a signal: the differential decode integrates, so a phase that crosses
    // a sample boundary one frame early or late leaves that delta in the
    // running sum for good. float32 drifts; 14 bit integers cannot.
    alignas(16) uint32_t sub_phase[PCM_SIM_MAX_VOICES];   // 0..0x3fff
    alignas(16) uint32_t phase_step[PCM_SIM_MAX_VOICES];  // ram2[0] verbatim

    alignas(16) int32_t address[PCM_SIM_MAX_VOICES];
    alignas(16) int32_t address_loop[PCM_SIM_MAX_VOICES];
    alignas(16) int32_t address_end[PCM_SIM_MAX_VOICES];
    alignas(16) int32_t bank[PCM_SIM_MAX_VOICES];         // hiaddr << 20

    // Flags are kept as all-ones/all-zeros masks rather than bools so the
    // selects are the same shape scalar or vector.
    alignas(16) uint32_t bidi_mask[PCM_SIM_MAX_VOICES];    // sample ping-pongs
    alignas(16) uint32_t reverse_mask[PCM_SIM_MAX_VOICES]; // currently backwards
    alignas(16) int32_t  direction[PCM_SIM_MAX_VOICES];    // +1, or -1 when the
                                                           // data is stored
                                                           // backwards (b7)

    // The wave ROM window this voice reads through, and the much smaller one
    // holding its block exponents. The control adapter resolves these when
    // waveform settings change, not during sample rendering.
    const uint8_t* rom_base[PCM_SIM_MAX_VOICES];
    alignas(16) uint32_t rom_mask[PCM_SIM_MAX_VOICES];
    const uint8_t* block_base[PCM_SIM_MAX_VOICES];
    alignas(16) uint32_t block_mask[PCM_SIM_MAX_VOICES];

    // --- differential decode ------------------------------------------------
    // The ROM holds 8 bit deltas plus a 4 bit exponent shared by 16 samples.
    // `reference` is the running sum the next frame starts from. Interpolation
    // reads the upcoming deltas directly, so there is no history to keep.
    alignas(16) float reference[PCM_SIM_MAX_VOICES];

    // --- state variable filter ---------------------------------------------
    alignas(16) float svf_low[PCM_SIM_MAX_VOICES];
    alignas(16) float svf_band[PCM_SIM_MAX_VOICES];
    alignas(16) float svf_q[PCM_SIM_MAX_VOICES];     // ram2[6] bits 8..14 / 64
    alignas(16) float svf_tap[PCM_SIM_MAX_VOICES];   // ram2[6] bit 1: 0 low, 1 high

    // Per-frame outputs of the owned envelope ramps. The legacy adapter mirrors
    // ramp readback to its register view for firmware/controller compatibility.
    alignas(16) float gain_a[PCM_SIM_MAX_VOICES];
    alignas(16) float gain_b[PCM_SIM_MAX_VOICES];
    alignas(16) float cutoff[PCM_SIM_MAX_VOICES];

    // --- output routing -----------------------------------------------------
    alignas(16) float pan_l[PCM_SIM_MAX_VOICES];
    alignas(16) float pan_r[PCM_SIM_MAX_VOICES];
    alignas(16) float send_reverb[PCM_SIM_MAX_VOICES];
    alignas(16) float send_chorus[PCM_SIM_MAX_VOICES];
    alignas(16) float gate[PCM_SIM_MAX_VOICES];      // 1.0 while the voice sounds

    int voice_count = 24;
    uint32_t boundaryEnabled=0, boundaryLatched=0;
};

// Returns the first eligible voice, or -1. Pending notifications block further
// publication, but inactive voices still clear their per-voice notification.
inline int PCMSim_CollectBoundary(PCMSimVoices& voices,bool clockUpdates,bool canPublish)
{
    int result=-1;
    if(!(voices.boundaryEnabled|voices.boundaryLatched)) return result;
    for(int slot=0;slot<voices.voice_count;++slot) {
        const auto bit=uint32_t(1)<<slot;
        if(voices.gate[slot]==0) { voices.boundaryLatched&=~bit; continue; }
        if(!canPublish || result>=0 || !(voices.boundaryEnabled&bit)
            || (voices.boundaryLatched&bit)) continue;
        const bool crossed=bool((uint32_t(voices.address[slot])
            + ((-uint32_t(voices.address_loop[slot]))&0xfffff))&0x100000)
            != (voices.direction[slot]<0);
        if(crossed) {
            if(clockUpdates) voices.boundaryLatched|=bit;
            result=slot;
        }
    }
    return result;
}

// Setup only, before rendering: initializes interpolation tables and chooses
// scalar/SIMD execution. Concurrent setup is supported; no lazy audio setup.
void PCMSim_Init();

// Changing waveform controls does not reset running signal history. Restart is
// a separate operation, called exactly at the owning scheduler's key edge.
void PCMSim_SetWaveform(PCMSimVoices& voices,unsigned slot,const PCMSimWaveform& waveform);
void PCMSim_RestartVoice(PCMSimVoices& voices,unsigned slot,uint32_t position,
    uint16_t phase,bool reverse);

// Apply a complete control update without resetting the voice's signal state.
void PCMSim_ApplyVoiceUpdate(PCMSimVoices& voices,unsigned slot,const sc55::VoiceRenderUpdate& update);

// Advance every voice's ramps on the same supplied clock, then render normally.
// The legacy adapter uses the same VoiceEnvelopes operation per voice.
inline void PCMSim_AdvanceEnvelopes(PCMSimVoices& voices,sc55::EnvelopeClock clock)
{
    for(int slot=0;slot<voices.voice_count;++slot) {
        const auto output=voices.envelopes[slot].advance(clock,voices.gate[slot]!=0);
        voices.gain_a[slot]=output.firstGain;
        voices.gain_b[slot]=output.secondGain;
        voices.cutoff[slot]=output.cutoff;
    }
}



// One shared-clock render operation owns EG advancement and signal generation.
// The caller supplies gates/controls; no per-voice EG scheduling is required.

// Signal-only diagnostic entry for comparing externally supplied EG outputs.
void PCMSim_RenderSignals(PCMSimVoices& voices,float out[4]);
inline void PCMSim_RenderFrame(PCMSimVoices& voices,sc55::EnvelopeClock clock,float out[4])
{
    PCMSim_AdvanceEnvelopes(voices,clock);
    PCMSim_RenderSignals(voices,out);
}

// The portable reference the vector path is checked against.
void PCMSim_RenderFrameScalar(PCMSimVoices& voices, float out[4]);
