#include "pcm_sim.h"

#include "pcm_interpolation.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

void PCMSim_SetWaveform(PCMSimVoices& voices,unsigned slot,const PCMSimWaveform& waveform)
{
    if(slot>=PCM_SIM_MAX_VOICES) return;
    voices.address_loop[slot]=waveform.loop;
    voices.address_end[slot]=waveform.end;
    voices.bank[slot]=waveform.bank;
    voices.bidi_mask[slot]=waveform.pingPong ? 0xffffffffu : 0;
    voices.direction[slot]=waveform.storedBackwards ? -1 : 1;
    voices.rom_base[slot]=waveform.samples; voices.rom_mask[slot]=waveform.sampleMask;
    voices.block_base[slot]=waveform.exponents; voices.block_mask[slot]=waveform.exponentMask;
}

void PCMSim_RestartVoice(PCMSimVoices& voices,unsigned slot,uint32_t position,uint16_t phase,bool reverse)
{
    if(slot>=PCM_SIM_MAX_VOICES) return;
    voices.address[slot]=int32_t(position&0xfffff);
    voices.sub_phase[slot]=phase&0x3fff;
    voices.reverse_mask[slot]=reverse ? 0xffffffffu : 0;
    voices.reference[slot]=0;
    voices.svf_low[slot]=0;
    voices.svf_band[slot]=0;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
 #include <arm_neon.h>
 #define PCM_SIM_NEON 1
#endif



namespace
{

// The chip's interpolation weights are Q12; as floats they are plain gains.
float g_interp[3][128];

// Scale for the 4 bit block exponent the ROM stores per 16 samples. The chip
// shifts by (10 - n) & 15, so the exponent wraps: n of 11 or more does not keep
// growing, it collapses to a fraction. Not a quirk to tidy away -- the wave
// data relies on it.
float g_block_scale[16];

std::once_flag g_tablesOnce;
bool g_forceScalar = false;

// The chip carries signals in 20 bit signed registers whose adders saturate.
// That is not an artefact to drop with the fixed point: it bounds the running
// differential sum, it is what stops the resonant filter from running away, and
// on the mix bus it is the audible clipping of a loud chord.
constexpr float PCM_CLIP_LO = -524288.0f;
constexpr float PCM_CLIP_HI = 524287.0f;

inline float clip20(float value)
{
    return std::fmin(std::fmax(value, PCM_CLIP_LO), PCM_CLIP_HI);
}

inline int32_t select(int32_t mask, int32_t when_true, int32_t when_false)
{
    return when_false ^ (mask & (when_true ^ when_false));
}

inline int32_t mask_eq(int32_t a, int32_t b)
{
    return -static_cast<int32_t>(a == b);
}



} // namespace

void PCMSim_Init()
{
    // Setup only: concurrent plug-in instances must not write shared tables
    // while another instance is already rendering from them.
    std::call_once(g_tablesOnce, [] {
    g_forceScalar = std::getenv("SC55_SCALAR") != nullptr;
    for (int tap = 0; tap < 3; ++tap)
        for (int i = 0; i < 128; ++i)
            g_interp[tap][i] = static_cast<float>(interp_lut[tap][i]) * (1.0f / 4096.0f);

    for (int n = 0; n < 16; ++n)
        g_block_scale[n] = std::ldexp(1.0f, 10 - ((10 - n) & 15));

    });
}



void PCMSim_ApplyVoiceUpdate(PCMSimVoices& voices,unsigned slot,const sc55::VoiceRenderUpdate& update)
{
    if(slot>=PCM_SIM_MAX_VOICES) return;
    voices.phase_step[slot]=update.phaseIncrement;
    for(unsigned i=0;i<3;++i) voices.envelopes[slot].ramps[i].command=update.rampCommands[i];
    voices.pan_l[slot]=float(update.panLeft)/64.0f;
    voices.pan_r[slot]=float(update.panRight)/64.0f;
    voices.send_reverb[slot]=float(update.reverbSend)/64.0f;
    voices.send_chorus[slot]=float(update.chorusSend)/64.0f;
    voices.svf_q[slot]=float(update.resonance&127)/64.0f;
    voices.svf_tap[slot]=(update.filterFlags&2)?1.0f:0.0f;
    const auto bit=uint32_t(1)<<slot;
    voices.boundaryEnabled=(voices.boundaryEnabled&~bit)|((update.filterFlags&1)?bit:0);
}

void PCMSim_RenderFrameScalar(PCMSimVoices& voices, float out[4])
{
    float dry_l = 0.0f, dry_r = 0.0f, send_rev = 0.0f, send_cho = 0.0f;

    for (int slot = 0; slot < voices.voice_count; ++slot)
    {
        // A silent voice contributes nothing and has no state left to carry, so
        // skip the whole slot. One well-predicted branch per voice per frame,
        // not per sample -- and it is the difference between an idle module
        // costing what a full chord costs and costing nothing.
        if (voices.gate[slot] == 0.0f)
        {
            voices.sub_phase[slot] = 0;
            voices.reference[slot] = 0.0f;
            voices.svf_low[slot] = 0.0f;
            voices.svf_band[slot] = 0.0f;
            voices.reverse_mask[slot] = 0;
            continue;
        }

        // ---- walk the read position ---------------------------------------
        // The chip always looks at the next four source samples and then keeps
        // whichever one the phase landed on, so four unrolled steps cover every
        // case with no data-dependent loop. Interpolation reads the same four
        // deltas, which is why they are gathered before anything is consumed.
        const uint32_t phase_before = voices.sub_phase[slot];
        const uint32_t phase = phase_before + voices.phase_step[slot];
        const int steps = static_cast<int>((phase >> 14) & 7);

        const int32_t loop = voices.address_loop[slot];
        const int32_t end  = voices.address_end[slot];
        const int32_t bank = voices.bank[slot];
        const int32_t bidi = static_cast<int32_t>(voices.bidi_mask[slot]);
        const int32_t flip = voices.direction[slot];
        int32_t reverse = static_cast<int32_t>(voices.reverse_mask[slot]);

        const uint8_t* const sample_rom = voices.rom_base[slot];
        const uint32_t sample_mask = voices.rom_mask[slot];
        const uint8_t* const block_rom = voices.block_base[slot];
        const uint32_t block_mask = voices.block_mask[slot];

        // Where each of the four steps would land, and the direction after it.
        // Only the entry the phase actually reached is kept, and the fourth step
        // does not update the direction -- the chip's own pipeline drops it
        // there, and ping-pong samples turn around at the wrong sample without.
        int32_t walk[5];
        int32_t turn[5];
        float delta[4];
        walk[0] = voices.address[slot];
        turn[0] = reverse;

        for (int step = 0; step < 4; ++step)
        {
            const int32_t here = walk[step];

            // Both reads unconditionally. Caching the exponent across the four
            // steps needs a branch per step to test the block, and these are L1
            // hits: the branch costs more than the load it saves.
            const int8_t stored =
                static_cast<int8_t>(sample_rom[(static_cast<uint32_t>(bank | here)) & sample_mask]);
            const uint8_t packed =
                block_rom[(static_cast<uint32_t>(bank | (here >> 5))) & block_mask];
            const float scale =
                g_block_scale[(here & 0x10) ? ((packed >> 4) & 15) : (packed & 15)];

            delta[step] = static_cast<float>(stored) * scale;

            // The boundary test is against where we are, not where we would
            // land, and the frame that sits on the boundary does not move: a
            // one-shot jumps to the loop point, a ping-pong stays put and only
            // turns around. Getting this off by one is invisible in the waveform
            // but fatal downstream, because the decode integrates.
            const int32_t boundary = select(reverse, loop, end);
            const int32_t at_boundary = mask_eq(here, boundary);

            const int32_t base = select(at_boundary & ~bidi, loop, here);
            const int32_t forward = select(bidi & reverse, -1, 1);
            const int32_t advance = (~at_boundary) & forward;

            walk[step + 1] = (base + flip * advance) & 0xfffff;
            reverse = bidi & (reverse ^ at_boundary);
            turn[step + 1] = (step < 3) ? reverse : turn[step];
        }

        // ---- differential decode -------------------------------------------
        // The running sum is what the next frame starts from; the interpolated
        // value this frame outputs starts from where the sum was.
        const float reference = voices.reference[slot];
        float advanced = reference;
        for (int step = 0; step < 4; ++step)
            advanced = clip20(advanced + delta[step] * static_cast<float>(step < steps));

        voices.sub_phase[slot] = phase & 0x3fff;
        voices.reference[slot] = advanced;
        voices.address[slot] = walk[steps];
        voices.reverse_mask[slot] = static_cast<uint32_t>(turn[steps]);

        // ---- interpolate ----------------------------------------------------
        const int ratio = static_cast<int>((phase_before >> 7) & 127);
        const float wave = clip20(reference
                                  + g_interp[0][ratio] * delta[0]
                                  + g_interp[1][ratio] * delta[1]
                                  + g_interp[2][ratio] * delta[2]);

        // ---- state variable filter ------------------------------------------
        const float f = voices.cutoff[slot] * (1.0f / 16384.0f);
        const float band = voices.svf_band[slot];
        const float low = clip20(voices.svf_low[slot] + f * band);
        const float damped = clip20(low + voices.svf_q[slot] * band);
        const float high = clip20(wave - damped);

        voices.svf_low[slot] = low;
        voices.svf_band[slot] = clip20(band + f * high);

        const float filtered = low + (high - low) * voices.svf_tap[slot];

        // ---- output ----------------------------------------------------------
        // The two envelope gains are separate stages on the chip and each one
        // saturates, so a voice sitting on the rail cannot be multiplied past it.
        const float voiced = clip20(clip20(filtered * voices.gain_a[slot]) * voices.gain_b[slot]);

        // Saturating per voice, not once at the end: the chip clips as it
        // accumulates, which makes the result order dependent.
        dry_l    = clip20(dry_l    + voiced * voices.pan_l[slot]);
        dry_r    = clip20(dry_r    + voiced * voices.pan_r[slot]);
        send_rev = clip20(send_rev + voiced * voices.send_reverb[slot]);
        send_cho = clip20(send_cho + voiced * voices.send_chorus[slot]);
    }

    out[0] = dry_l;
    out[1] = dry_r;
    out[2] = send_rev;
    out[3] = send_cho;
}

#if PCM_SIM_NEON
namespace
{
// Four voices to a register. Everything except the wave ROM reads is lane
// parallel; those stay scalar because NEON has no gather, but they are ordinary
// L1 hits, and keeping the address walk in vector form is what makes the rest
// worth doing.
void RenderFrameNeon(PCMSimVoices& voices, float out[4])
{
    const float32x4_t clip_lo = vdupq_n_f32(PCM_CLIP_LO);
    const float32x4_t clip_hi = vdupq_n_f32(PCM_CLIP_HI);
    const auto clip = [&] (float32x4_t v) { return vminq_f32(vmaxq_f32(v, clip_lo), clip_hi); };

    const float32x4_t zero = vdupq_n_f32(0.0f);
    const int32x4_t address_mask = vdupq_n_s32(0xfffff);

    float32x4_t dry_l = zero, dry_r = zero, send_rev = zero, send_cho = zero;

    for (int base = 0; base < voices.voice_count; base += 4)
    {
        const float32x4_t gate = vld1q_f32(&voices.gate[base]);
        const uint32x4_t live = vcgtq_f32(gate, zero);

        if (vmaxvq_u32(live) == 0)
        {
            // Whole quad silent: clear what the voices were carrying, as the
            // chip does, and move on.
            vst1q_u32(&voices.sub_phase[base], vdupq_n_u32(0));
            vst1q_u32(&voices.reverse_mask[base], vdupq_n_u32(0));
            vst1q_f32(&voices.reference[base], zero);
            vst1q_f32(&voices.svf_low[base], zero);
            vst1q_f32(&voices.svf_band[base], zero);
            continue;
        }

        // ---- phase ---------------------------------------------------------
        const uint32x4_t phase_before = vld1q_u32(&voices.sub_phase[base]);
        const uint32x4_t phase = vaddq_u32(phase_before, vld1q_u32(&voices.phase_step[base]));
        const uint32x4_t steps = vandq_u32(vshrq_n_u32(phase, 14), vdupq_n_u32(7));
        const uint32x4_t ratio = vandq_u32(vshrq_n_u32(phase_before, 7), vdupq_n_u32(127));

        // ---- walk the read position ----------------------------------------
        const int32x4_t loop = vld1q_s32(&voices.address_loop[base]);
        const int32x4_t end = vld1q_s32(&voices.address_end[base]);
        const int32x4_t bank = vld1q_s32(&voices.bank[base]);
        const uint32x4_t bidi = vld1q_u32(&voices.bidi_mask[base]);
        const int32x4_t flip = vld1q_s32(&voices.direction[base]);
        uint32x4_t reverse = vld1q_u32(&voices.reverse_mask[base]);

        int32x4_t walk[5];
        int32x4_t turn[5];
        float32x4_t delta[4];
        walk[0] = vld1q_s32(&voices.address[base]);
        turn[0] = vreinterpretq_s32_u32(reverse);

        // The walk is pure address arithmetic and does not depend on a single
        // byte of wave data, so run all four steps in registers first. Doing it
        // interleaved with the reads meant a vector store and reload per step,
        // which broke the dependency chain four times per quad for nothing.
        for (int step = 0; step < 4; ++step)
        {
            const int32x4_t boundary = vbslq_s32(reverse, loop, end);
            const uint32x4_t at_boundary = vceqq_s32(walk[step], boundary);

            const int32x4_t start = vbslq_s32(vandq_u32(at_boundary, vmvnq_u32(bidi)),
                                              loop, walk[step]);
            const int32x4_t forward = vbslq_s32(vandq_u32(bidi, reverse),
                                                vdupq_n_s32(-1), vdupq_n_s32(1));
            const int32x4_t advance = vandq_s32(vreinterpretq_s32_u32(vmvnq_u32(at_boundary)),
                                                forward);

            walk[step + 1] = vandq_s32(vaddq_s32(start, vmulq_s32(flip, advance)), address_mask);
            reverse = vandq_u32(bidi, veorq_u32(reverse, at_boundary));
            turn[step + 1] = (step < 3) ? vreinterpretq_s32_u32(reverse) : turn[step];
        }

        // Now the reads. NEON has no gather, so these stay scalar, but they are
        // one contiguous burst instead of four interleaved ones.
        alignas(16) int32_t addresses[4][4];
        for (int step = 0; step < 4; ++step)
            vst1q_s32(addresses[step], walk[step]);

        alignas(16) int32_t banks[4];
        vst1q_s32(banks, bank);

        alignas(16) float gathered[4][4];   // [step][lane]
        for (int lane = 0; lane < 4; ++lane)
        {
            const int slot = base + lane;
            // A newly constructed independent renderer need not install a
            // waveform for silent lanes that share a quad with a live voice.
            // Do not read their ROM pointers (or spend gathers on them).
            if(voices.gate[slot]==0.0f) {
                for(int step=0;step<4;++step) gathered[step][lane]=0.0f;
                continue;
            }
            const uint8_t* const sample_rom = voices.rom_base[slot];
            const uint32_t sample_mask = voices.rom_mask[slot];
            const uint8_t* const block_rom = voices.block_base[slot];
            const uint32_t block_mask = voices.block_mask[slot];
            const int32_t voice_bank = banks[lane];

            // Both reads unconditionally. Caching the exponent across the four
            // steps needs a branch per step to test the block, and these are L1
            // hits: the branch costs more than the load it saves.
            for (int step = 0; step < 4; ++step)
            {
                const int32_t at = addresses[step][lane];

                const int8_t stored = static_cast<int8_t>(
                    sample_rom[static_cast<uint32_t>(voice_bank | at) & sample_mask]);
                const uint8_t packed =
                    block_rom[static_cast<uint32_t>(voice_bank | (at >> 5)) & block_mask];
                const float scale =
                    g_block_scale[(at & 0x10) ? ((packed >> 4) & 15) : (packed & 15)];

                gathered[step][lane] = static_cast<float>(stored) * scale;
            }
        }

        for (int step = 0; step < 4; ++step)
            delta[step] = vld1q_f32(gathered[step]);

        // Keep the entry the phase actually reached.
        int32x4_t next_address = walk[0];
        int32x4_t next_turn = turn[0];
        for (int k = 1; k <= 4; ++k)
        {
            const uint32x4_t reached = vceqq_u32(steps, vdupq_n_u32(static_cast<uint32_t>(k)));
            next_address = vbslq_s32(reached, walk[k], next_address);
            next_turn = vbslq_s32(reached, turn[k], next_turn);
        }

        // ---- differential decode --------------------------------------------
        const float32x4_t reference = vld1q_f32(&voices.reference[base]);
        float32x4_t advanced = reference;
        for (int step = 0; step < 4; ++step)
        {
            const uint32x4_t take = vcgtq_u32(steps, vdupq_n_u32(static_cast<uint32_t>(step)));
            advanced = clip(vaddq_f32(advanced, vbslq_f32(take, delta[step], zero)));
        }

        vst1q_u32(&voices.sub_phase[base], vbslq_u32(live, vandq_u32(phase, vdupq_n_u32(0x3fff)),
                                                     vdupq_n_u32(0)));
        vst1q_f32(&voices.reference[base], vbslq_f32(live, advanced, zero));
        vst1q_s32(&voices.address[base], vbslq_s32(live, next_address, walk[0]));
        vst1q_u32(&voices.reverse_mask[base], vbslq_u32(live, vreinterpretq_u32_s32(next_turn),
                                                        vdupq_n_u32(0)));

        // ---- interpolate ------------------------------------------------------
        uint32_t ratios[4];
        vst1q_u32(ratios, ratio);
        float w0[4], w1[4], w2[4];
        for (int lane = 0; lane < 4; ++lane)
        {
            w0[lane] = g_interp[0][ratios[lane]];
            w1[lane] = g_interp[1][ratios[lane]];
            w2[lane] = g_interp[2][ratios[lane]];
        }

        float32x4_t wave = vfmaq_f32(reference, vld1q_f32(w0), delta[0]);
        wave = vfmaq_f32(wave, vld1q_f32(w1), delta[1]);
        wave = vfmaq_f32(wave, vld1q_f32(w2), delta[2]);
        wave = clip(wave);

        const float32x4_t gain_a = vld1q_f32(&voices.gain_a[base]);
        const float32x4_t gain_b = vld1q_f32(&voices.gain_b[base]);
        const float32x4_t cutoff = vld1q_f32(&voices.cutoff[base]);

        // ---- state variable filter ---------------------------------------------
        const float32x4_t f = vmulq_n_f32(cutoff, 1.0f / 16384.0f);
        const float32x4_t band = vld1q_f32(&voices.svf_band[base]);
        const float32x4_t low = clip(vfmaq_f32(vld1q_f32(&voices.svf_low[base]), f, band));
        const float32x4_t damped = clip(vfmaq_f32(low, vld1q_f32(&voices.svf_q[base]), band));
        const float32x4_t high = clip(vsubq_f32(wave, damped));

        vst1q_f32(&voices.svf_low[base], vbslq_f32(live, low, zero));
        vst1q_f32(&voices.svf_band[base], vbslq_f32(live, clip(vfmaq_f32(band, f, high)), zero));

        const float32x4_t filtered = vfmaq_f32(low, vsubq_f32(high, low),
                                               vld1q_f32(&voices.svf_tap[base]));

        // ---- output --------------------------------------------------------------
        const float32x4_t voiced =
            vmulq_f32(clip(vmulq_f32(clip(vmulq_f32(filtered, gain_a)), gain_b)), gate);

        dry_l    = clip(vfmaq_f32(dry_l,    voiced, vld1q_f32(&voices.pan_l[base])));
        dry_r    = clip(vfmaq_f32(dry_r,    voiced, vld1q_f32(&voices.pan_r[base])));
        send_rev = clip(vfmaq_f32(send_rev, voiced, vld1q_f32(&voices.send_reverb[base])));
        send_cho = clip(vfmaq_f32(send_cho, voiced, vld1q_f32(&voices.send_chorus[base])));
    }

    // Four lanes accumulated four interleaved groups of voices; fold them with
    // the same saturation. The order of the additions differs from the scalar
    // path, which only shows up once the bus is actually clipping.
    const auto fold = [] (float32x4_t v)
    {
        float lanes[4];
        vst1q_f32(lanes, v);
        float total = 0.0f;
        for (int i = 0; i < 4; ++i)
            total = clip20(total + lanes[i]);
        return total;
    };

    out[0] = fold(dry_l);
    out[1] = fold(dry_r);
    out[2] = fold(send_rev);
    out[3] = fold(send_cho);
}
} // namespace
#endif // PCM_SIM_NEON

void PCMSim_RenderSignals(PCMSimVoices& voices,float out[4])
{
#if PCM_SIM_NEON
    // SC55_SCALAR forces the reference path so the two can be compared.
    if (! g_forceScalar)
    {
        RenderFrameNeon(voices, out);
        return;
    }
#endif
    PCMSim_RenderFrameScalar(voices, out);
}
