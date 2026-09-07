#pragma once
#include <cstdint>
#include <span>
#include <optional>
#include <array>
#include <utility>

namespace sc55
{
// v1.21 00:1034..1082: reject velocities outside the partial interval;
// accepted values yield the curve index used by the following amplitude step.
// Bounds are MIDI bytes. Equal endpoints preserve this emulator's zero-divide
// result (index zero); real-hardware divide-by-zero behavior is not verified.
inline std::optional<uint8_t> PartialVelocityCurveIndex(uint8_t velocity,
    uint8_t first, uint8_t last) noexcept
{
    if (velocity > 127 || first > 127 || last > 127) return std::nullopt;
    const bool reversed = first > last;
    const unsigned low = reversed ? last : first;
    const unsigned high = reversed ? first : last;
    if (velocity < low || velocity > high) return std::nullopt;
    const unsigned width = high-low;
    unsigned normalized = width == 0 ? 0 : (unsigned(velocity)-low)*256/width;
    if (normalized > 255) normalized = 255;
    const auto index = uint8_t(normalized >> 1);
    return reversed ? uint8_t(first-index) : index;
}

struct PartialCandidates { uint8_t flags, count; };

using VelocityCurves = std::array<std::array<uint8_t,256>,16>;
struct PartialVelocityValues { uint8_t accumulator, amplitude, secondary; };

// v1.21 00:109b..10df. Byte arithmetic and the 16-bit multiply/round/shift
// are intentional: ordinary floating interpolation gives different results.
inline std::pair<uint8_t,uint8_t> InterpolateVelocityAmplitude(uint8_t accumulator,
    uint8_t increment, uint8_t endpoint, uint8_t curve) noexcept
{
    accumulator = uint8_t(accumulator + increment);
    const auto delta = uint8_t(endpoint - accumulator);
    const bool negative = (delta & 0x80) != 0;
    const auto distance = negative ? uint8_t(-delta) : delta;
    const auto interpolated = uint8_t(uint16_t((unsigned(curve)*distance + 127u)*2u) >> 8);
    const auto amplitude = uint8_t(negative ? accumulator-interpolated : accumulator+interpolated);
    return {accumulator,amplitude == 0 ? uint8_t(1) : amplitude};
}

// Complete accepted/rejected velocity operation at 00:1034..1119. Curve data
// is supplied as owned sound data; no firmware addresses are part of this API.
inline std::optional<PartialVelocityValues> EvaluatePartialVelocity(uint8_t velocity,
    std::span<const uint8_t,92> partial,uint8_t accumulator,bool reduceVelocity,
    const VelocityCurves& curves) noexcept
{
    const auto index = PartialVelocityCurveIndex(velocity,partial[0x41],partial[0x43]);
    if (!index) return std::nullopt;
    const auto amplitude = InterpolateVelocityAmplitude(accumulator,partial[0x42],partial[0x44],
        curves[partial[0x40] & 15][*index]);
    const auto secondIndex = reduceVelocity ? uint8_t(unsigned(velocity)*91u/127u) : velocity;
    return PartialVelocityValues{amplitude.first,amplitude.second,curves[partial[0x24] & 15][secondIndex]};
}
// v1.21 00:0fcc..1033. Presence (group != ffff) is checked later;
// this stage only removes velocity-rejected candidates and preserves high flags.
inline PartialCandidates SelectPartialCandidates(uint8_t flags,uint8_t velocity,
    std::span<const uint8_t,92> first,std::span<const uint8_t,92> second) noexcept
{
    if ((flags & 1) && !PartialVelocityCurveIndex(velocity,first[0x41],first[0x43])) flags &= uint8_t(~1u);
    if ((flags & 2) && !PartialVelocityCurveIndex(velocity,second[0x41],second[0x43])) flags &= uint8_t(~2u);
    return {flags,uint8_t((flags & 1) + ((flags >> 1) & 1))};
}

struct HighNoteMapping { uint16_t tone; uint8_t note; };
struct PatchVelocityPlan
{
    PartialCandidates candidates;
    uint8_t accumulator;
    std::array<std::optional<PartialVelocityValues>,2> partials;
};

// The two partials share an accumulator; rejected/disabled partials must not
// advance it. This is whole-patch sequencing, not two independent evaluations.
inline PatchVelocityPlan PreparePatchVelocity(uint8_t flags,uint8_t velocity,
    std::span<const uint8_t,92> first,std::span<const uint8_t,92> second,
    uint8_t accumulator,bool reduceVelocity,const VelocityCurves& curves) noexcept
{
    PatchVelocityPlan result{{flags,0},accumulator,{}};
    for (unsigned i = 0; i < 2; ++i)
    {
        const auto bit = uint8_t(1u << i);
        if (!(flags & bit)) continue;
        result.partials[i] = EvaluatePartialVelocity(velocity,i == 0 ? first : second,
            result.accumulator,reduceVelocity,curves);
        if (result.partials[i])
        {
            result.accumulator = result.partials[i]->accumulator;
            ++result.candidates.count;
        }
        else result.candidates.flags &= uint8_t(~bit);
    }
    return result;
}

// v1.21 melodic notes 125..127 use patch-common entries instead of the
// ordinary note path. A high-bit tone is absent; preserve it for the caller.
inline std::optional<HighNoteMapping> MapHighNote(uint8_t note,
    std::span<const uint8_t,20> common) noexcept
{
    if (note < 125 || note > 127) return std::nullopt;
    const unsigned index = note - 125;
    return HighNoteMapping{uint16_t((common[8+index*2] << 8) | common[9+index*2]),common[14+index]};
}

struct TrackedKey
{
    uint8_t key;
    uint16_t fraction; // firmware's 0..1000 sub-semitone representation
};

// v1.21 00:1390..141e. The 21-entry coefficient table at 00:14c6
// is n == 0 ? 0 : floor((n * 65536 - 1) / 20); no ROM bytes are retained.
// Values outside this verified table range are rejected, not silently clamped.
inline std::optional<TrackedKey> TrackPartialKey(uint8_t key, uint8_t sourceKey,
                                                uint8_t tracking) noexcept
{
    const int slope = int(tracking) - 64;
    const unsigned magnitude = unsigned(slope < 0 ? -slope : slope);
    if (key > 127 || sourceKey > 127 || magnitude > 20) return std::nullopt;
    const int distance = int(sourceKey) - 60;
    const bool negative = (slope < 0) != (distance < 0);
    const unsigned coefficient = magnitude == 0 ? 0 : (magnitude * 65536u - 1u) / 20u;
    const uint32_t product = unsigned(distance < 0 ? -distance : distance) * 2u * coefficient;
    unsigned integral = product >> 16;
    const unsigned remainder = product & 65535u;
    uint16_t fraction = 0;
    if (remainder >= 65000)
        ++integral;
    else
    {
        fraction = uint16_t(remainder * 999u / 65000u);
        if (negative)
        {
            fraction = uint16_t(1000 - fraction);
            ++integral;
        }
    }
    const auto left = uint8_t(negative ? -int(integral) : int(integral));
    const auto right = uint8_t(int(key) - int(sourceKey) + 60);
    auto result = uint8_t(left + right);
    // Match the byte ADD's N/V handling, including a wrapped base key.
    const bool overflow = ((~(left ^ right) & (left ^ result)) & 0x80) != 0;
    if (result & 0x80) result = overflow ? 127 : 0;
    return TrackedKey{result, fraction};
}

namespace detail
{
inline uint8_t AddKeyOffset(uint8_t key, uint8_t offset) noexcept
{
    const auto result = uint8_t(key + offset);
    return (result & 0x80) ? uint8_t((offset & 0x80) ? 0 : 127) : result;
}
// The firmware tests N and borrow after a byte subtraction, not a signed
// wide-integer clamp. Preserve wrap even for non-MIDI byte values.
inline uint8_t ClippedKeySubtract(uint8_t value, uint8_t amount) noexcept
{
    const auto result = uint8_t(value - amount);
    return (result & 0x80) ? uint8_t(value < amount ? 0 : 127) : result;
}
}

// v1.21 00:1e1b..1e39: part +6 is centred at 64; the per-part
// offset at ab46 is already a signed byte. Preserve their combined byte wrap.
inline uint8_t TransposePartKey(uint8_t note, uint8_t partShift, uint8_t offset) noexcept
{
    return detail::AddKeyOffset(note, uint8_t(partShift - 64 + offset));
}

// v1.21 00:1e3a..1e58. Zero bypasses master transpose, unlike value 64
// which performs a zero-offset byte add (observable for out-of-range keys).
inline uint8_t TransposeMasterKey(uint8_t key, uint8_t masterShift) noexcept
{
    return masterShift == 0 ? key : detail::AddKeyOffset(key, uint8_t(masterShift - 64));
}

// v1.21 00:141f..14c4. The caller selects scaleTuning[note % 12].
// Normalizes by one semitone only, exactly as the firmware does; it does not
// clamp the byte key at 0/127 or repeatedly normalize extreme fractions.
inline std::optional<TrackedKey> ApplyScaleTuning(TrackedKey input, uint8_t tuning,
                                                uint8_t tracking) noexcept
{
    if (tuning > 127 || input.fraction > 1000) return std::nullopt;
    if (tuning == 64 || tracking == 0) return input;
    const int slope = int(tracking) - 64;
    const unsigned magnitude = unsigned(slope < 0 ? -slope : slope);
    if (magnitude > 20) return std::nullopt;
    const int detune = int(tuning) - 64;
    const unsigned coefficient = magnitude == 0 ? 0 : (magnitude * 65536u - 1u) / 20u;
    const unsigned product = unsigned(detune < 0 ? -detune : detune) * 2u * coefficient;
    const unsigned remainder = product & 65535u;
    int offset = remainder >= 65000 ? int(((product >> 16) + 1) * 10)
                                   : int((product >> 16) * 10 + remainder / 6500);
    if ((slope < 0) != (detune < 0)) offset = -offset;
    int fraction = int(input.fraction) + offset;
    auto key = input.key;
    if (fraction < 0) { fraction += 1000; key = uint8_t(key - 1); }
    else if (fraction >= 1000) { fraction -= 1000; key = uint8_t(key + 1); }
    return TrackedKey{key, uint16_t(fraction)};
}

// v1.21 00:1378..138f, partial byte +0x0a, centre = 64.
inline uint8_t TransposePartialKey(uint8_t key, uint8_t coarse) noexcept
{
    return detail::ClippedKeySubtract(uint8_t(key + coarse), 64);
}

// Composed v1.21 00:132b..1334 pitch stage. Inputs belong to native note/part
// state; no emulated register or memory access is required by this operation.
// sourceKey and originalNote are deliberately separate: scale tuning uses
// the original note's pitch class, not the transposed/tracked result.
inline std::optional<TrackedKey> PreparePartialPitch(
    uint8_t key, uint8_t sourceKey, uint8_t originalNote,
    std::span<const uint8_t,92> partial, std::span<const uint8_t,12> scale) noexcept
{
    const auto tracked = TrackPartialKey(TransposePartialKey(key, partial[10]),
                                         sourceKey, partial[13]);
    if (!tracked) return std::nullopt;
    return ApplyScaleTuning(*tracked, scale[originalNote % 12], partial[13]);
}

// v1.21 00:1361..1374, after the earlier key-tracking/override steps.
// partial byte +1 is the reference key used for multisample lookup.
inline uint8_t MakeSampleLookupKey(uint8_t adjustedKey, uint8_t referenceKey) noexcept
{
    return detail::ClippedKeySubtract(uint8_t(adjustedKey + 64), referenceKey);
}

struct PartialKeyResolution
{
    uint8_t lookupKey;
    std::optional<uint8_t> storedOriginalNote;
    std::optional<uint8_t> storedAdjustedKey;
};

// v1.21 00:1334..1374. Optional fields express actual note-state writes,
// not replacement values for unchanged fields. The minimum-key comparison
// tests the sign of a wrapped byte subtraction, NOT C++ signed/unsigned max.
inline PartialKeyResolution ResolvePartialKey(uint8_t adjustedKey, uint8_t minimumKey,
    uint8_t mode, uint8_t remappedNote, uint8_t referenceKey) noexcept
{
    PartialKeyResolution result{};
    if (mode != 0x81)
    {
        if (mode != 0x80)
        {
            result.storedOriginalNote = remappedNote;
        }
        result.storedAdjustedKey = adjustedKey;
    }
    // Caller 123d/12be reloads the partial-specific minimum and updates N.
    if (!(minimumKey & 0x80) && (uint8_t(adjustedKey - minimumKey) & 0x80))
        adjustedKey = minimumKey;
    result.lookupKey = MakeSampleLookupKey(adjustedKey, referenceKey);
    return result;
}

// v1.21 import-time locations, not pointers into an emulated address space.
// Keep the firmware's 16-bit offset wrap within the selected ROM bank.
inline uint32_t V121MultisampleOffset(uint16_t group) noexcept
{
    const bool second = group >= 144;
    return (second ? 0x20000u : 0x10000u)
        | uint16_t(0xbd00u + (second ? group - 144u : group) * 60u);
}

inline std::optional<uint32_t> V121SampleDescriptorOffset(uint16_t sample) noexcept
{
    // Negative firmware IDs take a separate path, not a descriptor lookup.
    if (sample & 0x8000) return std::nullopt;
    const bool second = sample >= 532;
    return (second ? 0x20000u : 0x10000u)
        | uint16_t(0xdec0u + (second ? sample - 532u : sample) * 16u);
}

// A multisample record contains 16 inclusive upper-key boundaries at +12 and
// 16 big-endian sample IDs at +28. High-bit IDs are retained for the caller's
// special/absent-sample handling. No firmware address arithmetic is used here.
inline std::optional<uint16_t> SelectSampleZone(std::span<const uint8_t,60> record,
                                               uint8_t key) noexcept
{
    for (unsigned zone = 0; zone < 16; ++zone)
        if (key <= record[12 + zone])
        {
            const auto offset = 28 + 2 * zone;
            return uint16_t((record[offset] << 8) | record[offset + 1]);
        }
    return std::nullopt;
}

struct SampleAddresses { uint32_t start, loop, end; };

// 534b..5364: unsigned reference C8B0 plus signed voice+A6, saturated to
// the PCM10 word range. Sign conversion is explicit for every 16-bit pattern.
inline uint16_t ApplyPitchOffset(uint16_t reference,uint16_t encodedOffset) noexcept
{
    const int32_t offset = encodedOffset < 0x8000 ? int32_t(encodedOffset) : int32_t(encodedOffset)-65536;
    const int32_t sum = int32_t(reference)+offset;
    return uint16_t(sum < 0 ? 0 : sum > 65535 ? 65535 : sum);
}

// 5326..534b: source byte is centered at 128, scaled by 65536/divisor.
// Both signed-quotient overflow and unsigned DIVXU overflow saturate to 32767.
// The preceding firmware path ensures a nonzero divisor; reject zero instead
// of emulating the CPU's divide-error trap in the native audio path.
inline std::optional<uint16_t> PreparePitchOffset(uint8_t source,uint16_t divisor) noexcept
{
    if (divisor == 0) return std::nullopt;
    const int32_t delta = int32_t(source)-128;
    const uint32_t magnitude = uint32_t(delta < 0 ? -delta : delta);
    const uint32_t quotient = (magnitude<<16)/divisor;
    const int32_t clipped = int32_t(quotient > 32767 ? 32767 : quotient);
    return uint16_t(delta < 0 ? -clipped : clipped);
}

// v1.21 00:2bb4..2bec. Selection of this descriptor is a separate operation.
// The flag selects the unoffset start; the hardware address arithmetic is 24-bit.
inline SampleAddresses DecodeSampleAddresses(std::span<const uint8_t, 10> descriptor,
                                             bool unoffsetStart) noexcept
{
    const auto word = [&](unsigned offset) {
        return uint32_t((descriptor[offset] << 8) | descriptor[offset + 1]);
    };
    constexpr uint32_t mask = 0xffffff;
    const uint32_t base = (uint32_t(descriptor[1]) << 16) | word(2);
    const uint32_t end = (base + word(6)) & mask;
    return {(base + (unoffsetStart ? 0 : word(4))) & mask,
            (end - word(8)) & mask, end};
}

// Firmware-independent description of the sample-address portion of voice
// startup. Input words must ultimately come from native sample selection.
// Encoded addresses retain the source's upper byte, although PCM consumes only
// its low nibble. Keeping it preserves the byte-level device transaction.
struct SampleAddressSetup
{
    uint16_t mode = 0;
    uint32_t start = 0, loop = 0, end = 0;
};

struct SampleControl { uint16_t mode; uint8_t loopFlag; };

// v1.21 00:2bef..2c13. The loop address's high nibble selects the
// sample bank; descriptor +10 bit 0 and the logical voice fill the low byte.
// The byte operations preserve the incoming R3 high byte: its low nibble
// becomes the PCM history nibble. It is an explicit input, not assumed zero.
// Descriptor bit 1 is retained separately by the firmware.
inline SampleControl DecodeSampleControl(uint32_t loop, uint8_t descriptorFlags,
                                         uint8_t voice, uint8_t historyNibble) noexcept
{
    return {uint16_t(((historyNibble & 15u) << 12) | ((loop >> 12) & 0x0f00)
                    | ((descriptorFlags & 1u) << 6) | voice),
            uint8_t(descriptorFlags & 2)};
}

// The voice slot must already be selected. Does not key on the voice or create
// envelope/pitch state. Matches the 00:5784..00:57a7 device-write sequence.
template <class Write>
void WriteSampleAddressSetup(const SampleAddressSetup& setup, Write&& write)
{
    write(0x1e, uint8_t(setup.mode >> 8));
    write(0x1f, uint8_t(setup.mode));
    const auto address = [&](uint8_t reg, uint32_t value)
    {
        write(reg, uint8_t(value >> 16));
        write(uint8_t(reg + 1), uint8_t(value >> 8));
        write(uint8_t(reg + 2), uint8_t(value));
    };
    address(0x05, setup.start);
    address(0x09, setup.loop);
    address(0x0d, setup.end);
}
}
