// pcm_sim - the SC-55 PCM chip as a simulation rather than an emulation.
//
// pcm.cpp reproduces the mechanism: it walks the chip's slot pipeline one chip
// cycle at a time, with 20 bit saturating adders and the block float packing
// the silicon used. This file reproduces the behaviour instead -- what the chip
// does to the signal -- in float32, structure of arrays, with no
// data-dependent branches in the per-voice path so it vectorises.
//
// The interface is the one the firmware already uses, so the two can be driven
// from an identical register stream and compared:
//
//     ram1[slot][0..7]   20 bit words: sample addresses and filter state
//     ram2[slot][0..15]  16 bit words: pitch, envelopes, pan, sends, flags
//
// Effects (slots 28..31) are not handled here yet; this covers voices 0..23.
#pragma once

#include <cstdint>

struct pcm_t;

// Rounded up to a whole number of NEON quads.
inline constexpr int PCM_SIM_MAX_VOICES = 32;

struct PCMSimVoices
{
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
    // holding its block exponents. Resolving these means a switch and a chase
    // through mcu_t, so it happens when the firmware touches the voice
    // (250 Hz per voice) rather than per sample.
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

    // --- envelopes ----------------------------------------------------------
    // Three generators per voice: two gains and one filter cutoff. Each walks
    // `level` towards `target`, either asymptotically (rate) or as a straight
    // ramp (step). Both forms are always evaluated and blended by `linear`, so
    // there is no branch.
    alignas(16) float env_level[3][PCM_SIM_MAX_VOICES];
    alignas(16) float env_target[3][PCM_SIM_MAX_VOICES];
    alignas(16) float env_rate[3][PCM_SIM_MAX_VOICES];
    alignas(16) float env_step[3][PCM_SIM_MAX_VOICES];
    alignas(16) float env_linear[3][PCM_SIM_MAX_VOICES];

    // --- output routing -----------------------------------------------------
    alignas(16) float pan_l[PCM_SIM_MAX_VOICES];
    alignas(16) float pan_r[PCM_SIM_MAX_VOICES];
    alignas(16) float send_reverb[PCM_SIM_MAX_VOICES];
    alignas(16) float send_chorus[PCM_SIM_MAX_VOICES];
    alignas(16) float gate[PCM_SIM_MAX_VOICES];      // 1.0 while the voice sounds

    int voice_count = 24;
};

// Builds the interpolation and envelope rate tables. Call once.
void PCMSim_Init();

// Pulls one slot's register block into the simulation's own form. Cheap; the
// firmware rewrites a voice's parameters at 250 Hz, not per sample.
void PCMSim_SyncVoice(PCMSimVoices& voices, const pcm_t& pcm, int slot);

// Adopts the chip's start position for a voice the firmware has just keyed on
// and clears everything this voice was carrying.
void PCMSim_KeyOn(PCMSimVoices& voices, const pcm_t& pcm, int slot);

// Produces one output frame: dry stereo plus the two effect sends.
void PCMSim_RenderFrame(PCMSimVoices& voices, const pcm_t& pcm, float out[4]);

// The portable reference the vector path is checked against.
void PCMSim_RenderFrameScalar(PCMSimVoices& voices, const pcm_t& pcm, float out[4]);
