#pragma once
// The SC-55's tone generator, written from what it does rather than how the
// chip does it: voices play waveforms through a resonant filter and two gain
// stages into four buses (left, right, reverb send, chorus send), and an
// effect unit adds the reverb and chorus back. The native controller drives it
// with the values it computes -- pitch steps, envelope segments, pan, sends --
// never with chip registers.
//
// Signal arithmetic stays in the device's 20-bit sample range and wraps there,
// as the device does when many loud voices add up.
//
// Header-only so the plug-in projects need no new source files.

#include "sc55_frame.h"
#include "sc55_effect_unit.h"
#include "sc55_audio_buses.h"
#include "sc55_envelope_ramp.h"
#include "sc55_voice_render_update.h"
#include "sc55_voice_set.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#include <arm_neon.h>
#define SC55_TONE_NEON 1
#endif

namespace sc55
{
namespace tone
{
inline int32_t wrap20(int32_t value) noexcept { return int32_t(uint32_t(value) << 12) >> 12; }

// The device interpolates between samples with a smooth 4-point kernel: a
// cubic B-spline with a little Gaussian mixed in (4:1, sigma 0.66 samples),
// which matches its measured response within 0.04 dB up to 0.45 fs. Applied
// to differences: out = value + w0*d0 + w1*d1 + w2*d2, weights in 1/4096, for
// each of 128 positions between two samples.
struct Interpolation
{
    std::array<std::array<int32_t, 3>, 128> weights{};

    Interpolation()
    {
        for (unsigned r = 0; r < 128; ++r) {
            const double t = r / 128.0;
            const double spline[4]{(1 - t) * (1 - t) * (1 - t) / 6, (3 * t * t * t - 6 * t * t + 4) / 6,
                (-3 * t * t * t + 3 * t * t + 3 * t + 1) / 6, t * t * t / 6};
            double gauss[4], sum = 0;
            for (int k = 0; k < 4; ++k) sum += gauss[k] = std::exp(-(k - 1 - t) * (k - 1 - t) / (2 * 0.66 * 0.66));
            double tap[4];
            for (int k = 0; k < 4; ++k) tap[k] = 0.8 * spline[k] + 0.2 * gauss[k] / sum;
            // Taps on samples -> weights on the differences that follow them.
            weights[r] = {int32_t(std::lround((1 - tap[0]) * 4096)), int32_t(std::lround((tap[2] + tap[3]) * 4096)),
                int32_t(std::lround(tap[3] * 4096))};
        }
    }
};
inline const Interpolation interpolation;

// Waveform memory, decoded once. The ROM stores each sample as the 8-bit
// difference from the previous one, scaled by a 4-bit exponent shared by 16
// samples. Each entry here is the difference a voice adds to its running value.
class DecodedWaves
{
public:
    static constexpr uint32_t bankSize = 0x100000;

    void decode(unsigned bank, std::span<const uint8_t> rom)
    {
        std::fill(headers_[bank].begin(), headers_[bank].end(), uint8_t(0));
        std::copy_n(rom.begin(), std::min(rom.size(), headers_[bank].size()), headers_[bank].begin());
        auto& out = banks_[bank];
        out.assign(bankSize, 0);
        for (uint32_t address = 0; address < bankSize && address < rom.size(); ++address) {
            const int exponentPair = rom[address >> 5];
            const int exponent = (address & 0x10) ? exponentPair >> 4 : exponentPair & 15;
            const int scaled = (int(int8_t(rom[address])) * 2048) >> ((10 - exponent) & 15);
            out[address] = (scaled >> 1) + (scaled & 1);
        }
    }

    // A voice's view of a bank: unpopulated banks read one silent entry.
    struct View { const int32_t* data; uint32_t mask; };
    static View silent() noexcept { static constexpr int32_t silence = 0; return {&silence, 0}; }
    View bank(unsigned index) const noexcept
    {
        if (index < banks_.size() && !banks_[index].empty()) return {banks_[index].data(), bankSize - 1};
        return silent();
    }

    uint8_t headerByte(unsigned bank, uint32_t offset) const noexcept
    {
        return bank < headers_.size() && offset < headers_[bank].size() ? headers_[bank][offset] : 0;
    }

private:
    std::array<std::vector<int32_t>, 3> banks_;
    std::array<std::array<uint8_t, 256>, 3> headers_{};
};

// One envelope stage: a level that moves toward a target, either a fixed
// fraction of the remaining distance (exponential) or a fixed step (linear),
// once every 1, 4, 16, 64 or 128 frames. The level carries four fractional
// bits, so slow rates move by fractions of the 15-bit readback value.
struct Ramp
{
    int32_t level = 0;         // 15-bit readback scale << 4
    int32_t target = 0;        // same scale
    int32_t step = 0;          // linear: magnitude per update, same scale << shift
    uint16_t command = 0;      // as the controller sent it (for readback)
    uint8_t shift = 0;
    uint8_t intervalMask = 0;  // update when (frame & mask) == 0
    bool linear = false;

    // A segment as the controller encodes it: 8-bit target, rate byte.
    void setCommand(uint16_t value) noexcept
    {
        command = value;
        const unsigned rate = value & 0xff;
        target = int32_t(value >> 8) << 11;
        const bool slowest = (rate & 0xf0) == 0;
        const bool slow = slowest || (rate & 0x10);
        linear = (rate & 0x80) == 0 || ((rate & 0x40) == 0 && (!slow || (rate & 0x20) == 0));
        const bool everyFrame = (rate & 0x80) == 0 || (rate & 0x40) == 0;
        constexpr uint8_t masks[4]{3, 15, 63, 127};
        intervalMask = everyFrame ? 0 : masks[unsigned(slow) | ((rate & 0x20) ? 2u : 0u)];
        shift = uint8_t(linear ? (10 - (((rate >> 4) & 14) | unsigned(slow))) & 15 : (10 - (rate & 15)) & 15);
        step = int32_t((rate & 15) << 9) | (slowest ? 0 : 0x2000);
    }

    uint16_t readback() const noexcept { return uint16_t((level >> 4) & 0x7fff); }
    void setReadback(uint16_t value) noexcept { level = int32_t(value & 0x7fff) << 4; }

    // One movement from `from` toward the target.
    int32_t moved(int32_t from) const noexcept
    {
        const int32_t distance = target - from;
        if (!linear) return from + (distance >> shift);
        const bool falling = distance < 0;
        const int32_t delta = (falling ? step ^ ~0x3f : step) >> shift;
        const int32_t next = from + delta;
        const bool overshoot = falling ? next < target : next > target;
        return overshoot ? target : next;
    }

    void advance(uint16_t frame) noexcept
    {
        level = (frame & intervalMask) == 0 ? moved(level) : level;
    }

    // A voice without a key moves its cutoff from zero on every frame: its
    // level is one movement from zero, whatever it was before.
    uint16_t idleReadback() const noexcept { return uint16_t((moved(0) >> 4) & 0x7fff); }
};

enum class LoopMode : uint8_t { forward, pingPong };

struct Voice
{
    // Waveform: the bank, the sample that plays next, and the loop.
    DecodedWaves::View wave = DecodedWaves::silent();
    uint32_t position = 0, loopStart = 0, loopEnd = 0;
    LoopMode loopMode = LoopMode::forward;
    bool storedBackwards = false; // the sample data runs toward lower addresses
    bool returning = false;       // ping-pong: on the way back to the loop start
    uint16_t fraction = 0;        // 14-bit position between samples
    uint8_t pitchSource = 0;

    int32_t value = 0;            // running sum of consumed differences
    int32_t low = 0, band = 0;    // state-variable filter
    int32_t resonance = 0;        // 0..127, /64
    bool highPass = false;

    std::array<Ramp, 3> ramps{};  // amplitude, second gain, cutoff
    int32_t panLeft = 0, panRight = 0, reverbSend = 0, chorusSend = 0;

    bool keyed = false, started = false; // started: key-on frame done
    bool notifyLoop = false, notified = false;
    uint64_t installFrame = 0;
};

struct VoiceOutput { int32_t left, right, reverb, chorus; bool reachedLoop; };

// The next sample position: one step in the play direction, the loop start at
// the loop end, or a turn on the spot for ping-pong.
inline uint32_t nextPosition(const Voice& v, uint32_t position, bool& returning) noexcept
{
    const uint32_t limit = returning ? v.loopStart : v.loopEnd;
    const bool atLimit = position == limit;
    const bool pingPong = v.loopMode == LoopMode::pingPong;
    const int direction = (returning ? -1 : 1) * (v.storedBackwards ? -1 : 1);
    const uint32_t stepped = (position + uint32_t(direction)) & 0xfffff;
    const uint32_t wrapped = pingPong ? position : v.loopStart;
    returning = pingPong & (returning ^ atLimit);
    return atLimit ? wrapped : stepped;
}

inline int32_t scale14(int32_t sample, int32_t gain) noexcept
{
    return int32_t((int64_t(sample) * gain + 8192) >> 14);
}

// One frame of a voice that has started. Interpolation, filter, gains, buses.
inline VoiceOutput renderVoiceFrame(Voice& v, uint16_t increment, uint16_t frame) noexcept
{
    // The device reads four differences ahead and interpolates from the
    // running value with the first three, weighted by the position.
    bool returning = v.returning;
    const uint32_t p0 = v.position;
    const uint32_t p1 = nextPosition(v, p0, returning);
    const bool returning1 = returning;
    const uint32_t p2 = nextPosition(v, p1, returning);
    const bool returning2 = returning;
    const uint32_t p3 = nextPosition(v, p2, returning);
    const bool returning3 = returning;
    const uint32_t p4 = nextPosition(v, p3, returning);
    const auto sample = [&](uint32_t position) { return v.wave.data[position & v.wave.mask]; };
    const int32_t d0 = sample(p0), d1 = sample(p1), d2 = sample(p2), d3 = sample(p3);

    const auto& w = interpolation.weights[v.fraction >> 7];
    const int32_t interpolated = wrap20(v.value + ((d0 * w[0] + d1 * w[1] + d2 * w[2] + 2048) >> 12));

    const bool reachedLoop = v.notifyLoop & !v.notified
        & ((((p0 + ((0u - v.loopStart) & 0xfffff)) & 0x100000) != 0) ^ v.storedBackwards);

    // Advance by the whole samples the pitch step crosses this frame.
    const uint32_t phase = uint32_t(v.fraction) + increment;
    const unsigned whole = (phase >> 14) & 7;
    v.fraction = uint16_t(phase & 0x3fff);
    const int32_t consumed = (whole >= 1 ? d0 : 0) + (whole >= 2 ? d1 : 0) + (whole >= 3 ? d2 : 0) + (whole >= 4 ? d3 : 0);
    v.value = wrap20(v.value + consumed);
    v.position = whole >= 4 ? p4 : whole == 3 ? p3 : whole == 2 ? p2 : whole == 1 ? p1 : p0;
    v.returning = whole >= 3 ? returning3 : whole == 2 ? returning2 : whole == 1 ? returning1 : v.returning;

    // Resonant state-variable filter at the cutoff before this frame's move.
    const int32_t cutoff = v.ramps[2].readback();
    v.low = wrap20(v.low + scale14(v.band, cutoff));
    const int32_t damped = wrap20(v.low + ((v.band * v.resonance + 32) >> 6));
    const int32_t high = wrap20(interpolated - damped);
    v.band = wrap20(v.band + scale14(high, cutoff));
    const int32_t filtered = v.highPass ? high : v.low;

    for (auto& ramp : v.ramps) ramp.advance(frame);
    const int32_t amplitude = wrap20(scale14(filtered, v.ramps[0].readback() & 0x7ffe));
    const int32_t out = wrap20(scale14(amplitude, v.ramps[1].readback() & 0x7ffe));

    return {(out * v.panLeft + 32) >> 6, (out * v.panRight + 32) >> 6,
        (out * v.reverbSend + 32) >> 6, (out * v.chorusSend + 32) >> 6, reachedLoop};
}

#if SC55_TONE_NEON
// Four voices at once, one per 32-bit lane: renderVoiceFrame step for step,
// with lane-wise selects for its choices. Results are identical to it (the
// tone-generator check compares them). Voices here raise no loop events.
namespace simd
{
using I = int32x4_t;
using U = uint32x4_t; // lane masks are all ones or all zeros

inline I wrap20(I x) noexcept { return vshrq_n_s32(vshlq_n_s32(x, 12), 12); }
// (x * gain + 8192) >> 14 for 20-bit x and 15-bit gain, as one rounding
// doubling high multiply: ((x << 11) * (gain << 6) * 2 + 2^31) >> 32.
inline I scale14(I x, I gain) noexcept { return vqrdmulhq_s32(vshlq_n_s32(x, 11), vshlq_n_s32(gain, 6)); }
inline I mulRound6(I x, I gain) noexcept { return vrshrq_n_s32(vmulq_s32(x, gain), 6); }
inline I select(U mask, I a, I b) noexcept { return vbslq_s32(mask, a, b); }
inline U select(U mask, U a, U b) noexcept { return vbslq_u32(mask, a, b); }

struct RampLanes
{
    I level, target, step, shiftRight, intervalMask;
    U linear;

    I readback() const noexcept { return vandq_s32(vshrq_n_s32(level, 4), vdupq_n_s32(0x7fff)); }
    void advance(I frame) noexcept
    {
        const U update = vceqzq_s32(vandq_s32(frame, intervalMask));
        const I distance = vsubq_s32(target, level);
        const I exponential = vaddq_s32(level, vshlq_s32(distance, shiftRight));
        const U falling = vcltzq_s32(distance);
        const I signedStep = select(falling, veorq_s32(step, vdupq_n_s32(~0x3f)), step);
        const I next = vaddq_s32(level, vshlq_s32(signedStep, shiftRight));
        const U overshoot = select(falling, vcltq_s32(next, target), vcgtq_s32(next, target));
        const I moved = select(linear, select(overshoot, target, next), exponential);
        level = select(update, moved, level);
    }
};

struct VoiceQuad
{
    const int32_t* wave[4];
    uint32_t waveMask[4];
    U position, loopStart, loopEnd, fraction, increment;
    U pingPong, backwards, returning, highPass;
    I value, low, band, resonance;
    RampLanes ramps[3];
    I panLeft, panRight, reverbSend, chorusSend;

    template<class Get> static U lanesU(Get&& get) noexcept
    { const uint32_t a[4]{get(0), get(1), get(2), get(3)}; return vld1q_u32(a); }
    template<class Get> static I lanesI(Get&& get) noexcept
    { const int32_t a[4]{get(0), get(1), get(2), get(3)}; return vld1q_s32(a); }
    static uint32_t mask(bool b) noexcept { return b ? 0xffffffffu : 0u; }

    void load(Voice* const v[4], const uint16_t increments[4]) noexcept
    {
        for (unsigned l = 0; l < 4; ++l) { wave[l] = v[l]->wave.data; waveMask[l] = v[l]->wave.mask; }
        position = lanesU([&](unsigned l) { return v[l]->position; });
        loopStart = lanesU([&](unsigned l) { return v[l]->loopStart; });
        loopEnd = lanesU([&](unsigned l) { return v[l]->loopEnd; });
        fraction = lanesU([&](unsigned l) { return uint32_t(v[l]->fraction); });
        increment = lanesU([&](unsigned l) { return uint32_t(increments[l]); });
        pingPong = lanesU([&](unsigned l) { return mask(v[l]->loopMode == LoopMode::pingPong); });
        backwards = lanesU([&](unsigned l) { return mask(v[l]->storedBackwards); });
        returning = lanesU([&](unsigned l) { return mask(v[l]->returning); });
        highPass = lanesU([&](unsigned l) { return mask(v[l]->highPass); });
        value = lanesI([&](unsigned l) { return v[l]->value; });
        low = lanesI([&](unsigned l) { return v[l]->low; });
        band = lanesI([&](unsigned l) { return v[l]->band; });
        resonance = lanesI([&](unsigned l) { return v[l]->resonance; });
        for (unsigned r = 0; r < 3; ++r) {
            auto& ramp = ramps[r];
            ramp.level = lanesI([&](unsigned l) { return v[l]->ramps[r].level; });
            ramp.target = lanesI([&](unsigned l) { return v[l]->ramps[r].target; });
            ramp.step = lanesI([&](unsigned l) { return v[l]->ramps[r].step; });
            ramp.shiftRight = lanesI([&](unsigned l) { return -int32_t(v[l]->ramps[r].shift); });
            ramp.intervalMask = lanesI([&](unsigned l) { return int32_t(v[l]->ramps[r].intervalMask); });
            ramp.linear = lanesU([&](unsigned l) { return mask(v[l]->ramps[r].linear); });
        }
        panLeft = lanesI([&](unsigned l) { return v[l]->panLeft; });
        panRight = lanesI([&](unsigned l) { return v[l]->panRight; });
        reverbSend = lanesI([&](unsigned l) { return v[l]->reverbSend; });
        chorusSend = lanesI([&](unsigned l) { return v[l]->chorusSend; });
    }

    // Only the state a frame changes goes back.
    void store(Voice* const v[4]) const noexcept
    {
        alignas(16) uint32_t p[4], f[4], back[4];
        alignas(16) int32_t val[4], lo[4], ba[4], lv[3][4];
        vst1q_u32(p, position); vst1q_u32(f, fraction); vst1q_u32(back, returning);
        vst1q_s32(val, value); vst1q_s32(lo, low); vst1q_s32(ba, band);
        for (unsigned r = 0; r < 3; ++r) vst1q_s32(lv[r], ramps[r].level);
        for (unsigned l = 0; l < 4; ++l) {
            v[l]->position = p[l]; v[l]->fraction = uint16_t(f[l]); v[l]->returning = back[l] != 0;
            v[l]->value = val[l]; v[l]->low = lo[l]; v[l]->band = ba[l];
            for (unsigned r = 0; r < 3; ++r) v[l]->ramps[r].level = lv[r][l];
        }
    }

    U next(U at, U& back) const noexcept
    {
        const U atLimit = vceqq_u32(at, select(back, loopStart, loopEnd));
        const U stepBack = veorq_u32(back, backwards);
        const U stepped = vandq_u32(vaddq_u32(at, select(stepBack, vdupq_n_u32(0xffffffffu), vdupq_n_u32(1))), vdupq_n_u32(0xfffff));
        const U wrapped = select(pingPong, at, loopStart);
        back = vandq_u32(pingPong, veorq_u32(back, atLimit));
        return select(atLimit, wrapped, stepped);
    }

    I gather(U at) const noexcept
    {
        alignas(16) uint32_t a[4];
        vst1q_u32(a, at);
        const int32_t d[4]{wave[0][a[0] & waveMask[0]], wave[1][a[1] & waveMask[1]],
            wave[2][a[2] & waveMask[2]], wave[3][a[3] & waveMask[3]]};
        return vld1q_s32(d);
    }

    struct Buses { int32_t left, right, reverb, chorus; };

    Buses frame(uint16_t phase) noexcept
    {
        U back = returning;
        const U p0 = position;
        const U p1 = next(p0, back); const U back1 = back;
        const U p2 = next(p1, back); const U back2 = back;
        const U p3 = next(p2, back); const U back3 = back;
        const U p4 = next(p3, back);
        const I d0 = gather(p0), d1 = gather(p1), d2 = gather(p2), d3 = gather(p3);

        alignas(16) uint32_t ratio[4];
        vst1q_u32(ratio, vshrq_n_u32(fraction, 7));
        const auto weight = [&](unsigned k) {
            const auto& table = interpolation.weights;
            const int32_t w[4]{table[ratio[0]][k], table[ratio[1]][k], table[ratio[2]][k], table[ratio[3]][k]};
            return vld1q_s32(w);
        };
        const I weighted = vmlaq_s32(vmlaq_s32(vmulq_s32(d0, weight(0)), d1, weight(1)), d2, weight(2));
        const I interpolated = wrap20(vaddq_s32(value, vrshrq_n_s32(weighted, 12)));

        const U phaseSum = vaddq_u32(fraction, increment);
        const U whole = vandq_u32(vshrq_n_u32(phaseSum, 14), vdupq_n_u32(7));
        fraction = vandq_u32(phaseSum, vdupq_n_u32(0x3fff));
        const U reached1 = vcgeq_u32(whole, vdupq_n_u32(1)), reached2 = vcgeq_u32(whole, vdupq_n_u32(2));
        const U reached3 = vcgeq_u32(whole, vdupq_n_u32(3)), reached4 = vcgeq_u32(whole, vdupq_n_u32(4));
        const I consumed = vaddq_s32(vaddq_s32(vandq_s32(d0, vreinterpretq_s32_u32(reached1)), vandq_s32(d1, vreinterpretq_s32_u32(reached2))),
            vaddq_s32(vandq_s32(d2, vreinterpretq_s32_u32(reached3)), vandq_s32(d3, vreinterpretq_s32_u32(reached4))));
        value = wrap20(vaddq_s32(value, consumed));
        position = select(reached4, p4, select(reached3, p3, select(reached2, p2, select(reached1, p1, p0))));
        returning = select(reached3, back3, select(reached2, back2, select(reached1, back1, returning)));

        const I cutoff = ramps[2].readback();
        low = wrap20(vaddq_s32(low, scale14(band, cutoff)));
        const I damped = wrap20(vaddq_s32(low, mulRound6(band, resonance)));
        const I high = wrap20(vsubq_s32(interpolated, damped));
        band = wrap20(vaddq_s32(band, scale14(high, cutoff)));
        const I filtered = select(highPass, high, low);

        const I frameLanes = vdupq_n_s32(phase);
        for (auto& ramp : ramps) ramp.advance(frameLanes);
        const I evenMask = vdupq_n_s32(0x7ffe);
        const I amplitude = wrap20(scale14(filtered, vandq_s32(ramps[0].readback(), evenMask)));
        const I out = wrap20(scale14(amplitude, vandq_s32(ramps[1].readback(), evenMask)));
        return {vaddvq_s32(mulRound6(out, panLeft)), vaddvq_s32(mulRound6(out, panRight)),
            vaddvq_s32(mulRound6(out, reverbSend)), vaddvq_s32(mulRound6(out, chorusSend))};
    }
};
}
#endif
}

class ToneGenerator
{
public:
    static constexpr unsigned maxVoices = 128;
    static constexpr unsigned blockFrames = 128;
    static constexpr std::size_t waveBankSize = tone::DecodedWaves::bankSize;

    ToneGenerator(std::span<const uint8_t> wave1, std::span<const uint8_t> wave2,
        std::span<const uint8_t> wave3, unsigned voices = 24)
        : voiceCount_(std::min(voices, maxVoices))
    {
        waves_.decode(0, wave1);
        waves_.decode(1, wave2);
        waves_.decode(2, wave3);
    }

    // The shared clocks continue from the controller's setup.
    void initializeClock(uint16_t envelopePhase, uint16_t random) noexcept
    {
        envelopeFrame_ = uint16_t(envelopePhase & 0x3fff);
        random_ = random;
    }
    uint16_t randomWord() const noexcept { return random_; }
    unsigned voiceCount() const noexcept { return voiceCount_; }
    // A wave ROM byte by device address (2 MB per bank), for the bank names
    // the identity reply reports. Only bank headers are kept.
    uint8_t waveByte(uint32_t address) const noexcept { return waves_.headerByte(address >> 21, address & 0xfffff); }
    uint64_t renderedFrames() const noexcept { return frame_; }
    VoiceSet keys() const noexcept { return keys_; }

    // --- voices --------------------------------------------------------------

    void setPitch(unsigned source, uint16_t increment) noexcept
    {
        if (source < pitch_.size()) pitch_[source] = increment;
    }

    void update(unsigned index, const VoiceRenderUpdate& update) noexcept
    {
        if (index >= voiceCount_) return;
        auto& v = voices_[index];
        setPitch(index, update.phaseIncrement);
        for (unsigned i = 0; i < 3; ++i) v.ramps[i].setCommand(update.rampCommands[i]);
        v.panLeft = update.panLeft;
        v.panRight = update.panRight;
        v.reverbSend = update.reverbSend;
        v.chorusSend = update.chorusSend;
        v.resonance = update.resonance & 127;
        v.notifyLoop = (update.filterFlags & 1) != 0;
        v.highPass = (update.filterFlags & 2) != 0;
    }

    // Waveform and initial controls. The key comes separately, so that the
    // voices of a paired note start together.
    void install(unsigned index, const VoiceRenderStart& start) noexcept
    {
        if (index >= voiceCount_) return;
        auto& v = voices_[index];
        v.wave = waves_.bank((start.mode >> 9) & 7);
        v.position = start.start & 0xfffff;
        v.loopStart = start.loop & 0xfffff;
        v.loopEnd = start.end & 0xfffff;
        v.loopMode = (start.mode & 0x40) ? tone::LoopMode::pingPong : tone::LoopMode::forward;
        v.storedBackwards = (start.mode & 0x80) != 0;
        v.pitchSource = start.pitchSource < maxVoices ? start.pitchSource : uint8_t(start.mode & 31);
        v.started = (start.mode & 0x20) != 0;
        v.installFrame = frame_;
        update(index, start.controls);
    }

    void setKeys(VoiceSet keys) noexcept { keys_ = keys; }

    // Ready for the controller's first envelope commands: started, and two
    // frames have passed since the waveform was installed.
    bool ready(unsigned index) const noexcept
    {
        return index < voiceCount_ && voices_[index].started && frame_ - voices_[index].installFrame >= 2;
    }

    bool completeEnable(unsigned index, uint16_t level, uint16_t command) noexcept
    {
        if (!ready(index)) return false;
        auto& cutoff = voices_[index].ramps[2];
        cutoff.setReadback(uint16_t(level >> 1));
        cutoff.setCommand(command);
        return true;
    }

    void setRamp(unsigned index, EnvelopeRamp::Stage stage, uint16_t command) noexcept
    {
        if (index < voiceCount_) voices_[index].ramps[unsigned(stage)].setCommand(command);
    }

    uint16_t rampLevel(unsigned index, EnvelopeRamp::Stage stage) const noexcept
    {
        if (index >= voiceCount_) return 0;
        const auto& v = voices_[index];
        const auto& ramp = v.ramps[unsigned(stage)];
        // Until its key-on frame a voice is idle: no gain, cutoff one move from zero.
        if (!keys_.contains(index) || !v.started) return stage == EnvelopeRamp::Stage::cutoff ? ramp.idleReadback() : 0;
        return ramp.readback();
    }

    std::array<uint16_t, 2> gainLevels(unsigned index) const noexcept
    {
        return {rampLevel(index, EnvelopeRamp::Stage::firstGain), rampLevel(index, EnvelopeRamp::Stage::secondGain)};
    }

    // Segment hand-over: a held stage takes the saved level, a moving stage
    // stops where it is and reports that level.
    std::array<uint16_t, 3> synchronizeEnvelopes(unsigned index, const std::array<uint16_t, 3>& commands,
        const std::array<uint16_t, 3>& levels) noexcept
    {
        auto result = levels;
        if (index >= voiceCount_) return result;
        for (unsigned i = 0; i < 3; ++i) {
            auto& ramp = voices_[index].ramps[i];
            if (commands[i] == 0xff00) ramp.setReadback(uint16_t(levels[i] >> 1));
            else {
                result[i] = uint16_t(rampLevel(index, EnvelopeRamp::Stage(i)) << 1);
                ramp.setCommand(0xff00);
            }
        }
        return result;
    }

    // A voice that entered its loop, one at a time until the controller takes it.
    bool hasLoopEvent() const noexcept { return loopEvent_ >= 0; }
    int takeLoopEvent() noexcept { const int voice = loopEvent_; loopEvent_ = -1; return voice; }

    // Four voices per instruction where the CPU has NEON. The scalar path is
    // the definition; the check runs both and requires identical output.
    void setVectorized(bool enabled) noexcept { vectorized_ = enabled; }

    // --- effects -------------------------------------------------------------

    EffectUnit& effects() noexcept { return *effects_; }
    void enableEffects() noexcept { effectsActive_ = true; }

    // --- rendering -----------------------------------------------------------

    // Renders up to out.size() frames; stops after a frame that raised a loop
    // event (or after one frame while one is pending). Returns frames written.
    std::size_t render(std::span<sc55::Frame> out) noexcept
    {
        std::size_t done = 0;
        while (done < out.size()) {
            const std::size_t n = renderBlock(out.subspan(done, std::min<std::size_t>(out.size() - done, blockFrames)));
            done += n;
            if (loopEvent_ >= 0) break;
        }
        return done;
    }

private:
    std::size_t renderBlock(std::span<sc55::Frame> out) noexcept
    {
        // One frame at a time while an event waits for the controller, and
        // for the very first frame, which only settles the clocks.
        unsigned frames = loopEvent_ >= 0 || !updating_ ? 1u : unsigned(out.size());

        uint16_t phases[blockFrames];
        for (unsigned t = 0; t < frames; ++t) phases[t] = uint16_t((envelopeFrame_ - 1 - t) & 0x3fff);

        int eventVoice = -1;
        if (updating_) {
            // Voices that can raise a loop event run first. The block ends at
            // the first frame that raises one; only those voices replay.
            uint8_t notifying[maxVoices];
            unsigned notifyingCount = 0;
            for (unsigned i = 0; i < voiceCount_; ++i)
                if (keys_.contains(i) && voices_[i].notifyLoop && !voices_[i].notified) notifying[notifyingCount++] = uint8_t(i);

            const bool eventsOpen = loopEvent_ < 0;
            tone::Voice saved[maxVoices];
            for (unsigned k = 0; k < notifyingCount; ++k) saved[k] = voices_[notifying[k]];
            clearBuses(frames);
            int eventFrame = -1;
            for (unsigned k = 0; k < notifyingCount; ++k) {
                const int frame = renderVoice(notifying[k], frames, phases);
                if (eventsOpen && frame >= 0 && (eventFrame < 0 || frame < eventFrame)) { eventFrame = frame; eventVoice = notifying[k]; }
            }
            if (eventFrame >= 0 && unsigned(eventFrame) + 1 < frames) {
                frames = unsigned(eventFrame) + 1;
                for (unsigned k = 0; k < notifyingCount; ++k) voices_[notifying[k]] = saved[k];
                clearBuses(frames);
                for (unsigned k = 0; k < notifyingCount; ++k) renderVoice(notifying[k], frames, phases);
            }
            if (eventVoice >= 0) voices_[unsigned(eventVoice)].notified = true;

            // The rest: voices already playing go four at a time; voices
            // starting this block (their first frame is silent) go alone.
            uint8_t playing[maxVoices];
            unsigned playingCount = 0;
            for (unsigned i = 0; i < voiceCount_; ++i) {
                auto& v = voices_[i];
                if (!keys_.contains(i)) silence(v);
                else if ((v.notifyLoop && !v.notified) || int(i) == eventVoice) continue;
                else if (!v.started) renderVoice(i, frames, phases);
                else playing[playingCount++] = uint8_t(i);
            }
            unsigned k = 0;
#if SC55_TONE_NEON
            for (; vectorized_ && k + 4 <= playingCount; k += 4) renderQuad(playing + k, frames, phases);
#endif
            for (; k < playingCount; ++k) renderVoice(playing[k], frames, phases);
        } else clearBuses(frames);

        for (unsigned t = 0; t < frames; ++t) {
            const unsigned bit = ((random_ >> 0) ^ (random_ >> 1) ^ (random_ >> 7) ^ (random_ >> 12)) & 1;
            random_ = uint16_t((random_ >> 1) | (bit << 15));
            out[t] = {int32_t(uint32_t(pending_.left) << 12), int32_t(uint32_t(pending_.right) << 12)};
            float dry[6]{}, sends[6]{};
            effects_->process({phases[t], updating_}, effectsActive_, float(pending_.reverb), float(pending_.chorus), dry, sends);
            int dryReturns[6], sendReturns[6];
            for (unsigned i = 0; i < 6; ++i) { dryReturns[i] = int(std::lrint(dry[i])); sendReturns[i] = int(std::lrint(sends[i])); }
            const float voices[4]{float(tone::wrap20(left_[t])), float(tone::wrap20(right_[t])),
                float(tone::wrap20(reverb_[t])), float(tone::wrap20(chorus_[t]))};
            pending_ = MixVoiceAndEffectBuses(voices, dryReturns, sendReturns);
            if (updating_) effectsActive_ = true;
            updating_ = true;
            ++frame_;
        }
        envelopeFrame_ = uint16_t((envelopeFrame_ - frames) & 0x3fff);
        if (eventVoice >= 0) loopEvent_ = eventVoice;
        return frames;
    }

    void clearBuses(unsigned frames) noexcept
    {
        std::fill_n(left_, frames, 0); std::fill_n(right_, frames, 0);
        std::fill_n(reverb_, frames, 0); std::fill_n(chorus_, frames, 0);
    }

    // A voice without a key is silent and keeps no signal history.
    static void silence(tone::Voice& v) noexcept
    {
        v.value = v.low = v.band = 0;
        v.fraction = 0;
        v.returning = v.notified = false;
        v.ramps[0].level = v.ramps[1].level = 0;
    }

    // Runs one keyed voice for `frames` frames into the buses. Returns the
    // first frame that raised a loop event, or -1.
    int renderVoice(unsigned index, unsigned frames, const uint16_t* phases) noexcept
    {
        tone::Voice v = voices_[index];
        unsigned t = 0;
        if (!v.started) {
            // Key-on: the waveform starts from silence on the next frame. The
            // cutoff, still without a key, is one move from zero.
            silence(v);
            v.ramps[2].level = int32_t(v.ramps[2].idleReadback()) << 4;
            v.started = true;
            t = 1;
        }
        int event = -1;
        for (; t < frames; ++t) {
            const auto o = tone::renderVoiceFrame(v, pitch_[v.pitchSource], phases[t]);
            left_[t] += o.left; right_[t] += o.right; reverb_[t] += o.reverb; chorus_[t] += o.chorus;
            event = (event < 0 && o.reachedLoop) ? int(t) : event;
        }
        voices_[index] = v;
        return event;
    }

#if SC55_TONE_NEON
    void renderQuad(const uint8_t* indices, unsigned frames, const uint16_t* phases) noexcept
    {
        tone::Voice* const v[4]{&voices_[indices[0]], &voices_[indices[1]], &voices_[indices[2]], &voices_[indices[3]]};
        const uint16_t increments[4]{pitch_[v[0]->pitchSource], pitch_[v[1]->pitchSource],
            pitch_[v[2]->pitchSource], pitch_[v[3]->pitchSource]};
        tone::simd::VoiceQuad quad;
        quad.load(v, increments);
        for (unsigned t = 0; t < frames; ++t) {
            const auto o = quad.frame(phases[t]);
            left_[t] += o.left; right_[t] += o.right; reverb_[t] += o.reverb; chorus_[t] += o.chorus;
        }
        quad.store(v);
    }
#endif

    tone::DecodedWaves waves_;
    std::array<tone::Voice, maxVoices> voices_{};
    std::array<uint16_t, maxVoices> pitch_{};
    unsigned voiceCount_;
    VoiceSet keys_;
    std::unique_ptr<EffectUnit> effects_ = std::make_unique<EffectUnit>();
    bool effectsActive_ = false, updating_ = false, vectorized_ = true;
    AudioBuses pending_{};
    uint16_t envelopeFrame_ = 0, random_ = 0xffff;
    uint64_t frame_ = 0;
    int loopEvent_ = -1;
    int32_t left_[blockFrames], right_[blockFrames], reverb_[blockFrames], chorus_[blockFrames];
};
}
