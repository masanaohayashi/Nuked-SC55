#pragma once
#include "sc55_patch.h"
#include "sc55_sample_bank.h"
#include "sc55_envelope_setup.h"
#include "sc55_pitch.h"
#include "sc55_second_envelope_tables.h"
#include "sc55_modulation_tables.h"
#include "sc55_level.h"
#include <memory>

namespace sc55
{
// Research assets MD01..MD15, not a complete SC-55 data set (rhythm configuration,
// effects and further control tables remain). Import/export only off the audio thread.
// Runtime getters own all data; no CPU, ROM loader or source address needed.
class SoundData
{
public:
    using ModulationRates = std::array<uint16_t,256>;
    static constexpr size_t patchBytes = 224 * SC55Patch::SIZE;
    static constexpr size_t supplementalPatchBytes = 162 * SC55Patch::SIZE;
    static constexpr size_t curveBytes = 16 * 256;
    static constexpr size_t headerBytes = 8;

    static std::vector<uint8_t> encode(std::span<const uint8_t> patches,
                                     const VelocityCurves& curves, const SampleBank& samples,
                                     const EnvelopeTimes* times = nullptr,
                                     const EnvelopeLevelTables* levels = nullptr,
                                     const EnvelopeKeyTables* keys = nullptr,
                                     const EnvelopeKeyLevelTables* keyLevels = nullptr,
                                     const PitchGlideRates* glideRates = nullptr,
                                     const PartPitchKeyTables* pitchKeys = nullptr,
                                     const PitchEnvelopeTables* pitchEnvelope = nullptr,
                                     const SecondEnvelopePcmTables* secondEnvelope = nullptr,
                                     const SecondEnvelopePreparationTables* secondPreparation = nullptr,
                                     const ModulationRates* modulationRates = nullptr,
                                     const ModulationPreparationTables* modulationPreparation = nullptr,
                                     const PanTable* pan = nullptr,
                                     const PitchEnvelopeKeyCurves* pitchTiming = nullptr,
                                     std::span<const uint8_t> supplementalPatches = {})
    {
        if (patches.size() != patchBytes || (levels && !times) || (keys && !levels) || (keyLevels && !keys) || (glideRates && !keyLevels)) return {};
        const auto bank = samples.encode();
        if (pitchKeys && !glideRates) return {};
        if (pitchEnvelope && !pitchKeys) return {};
        if (secondEnvelope && !pitchEnvelope) return {};
        if (secondPreparation && !secondEnvelope) return {};
        if (modulationRates && !secondPreparation) return {};
        if (modulationPreparation && !modulationRates) return {};
        if (pan && !modulationPreparation) return {};
        if (pitchTiming && !pan) return {};
        if (!supplementalPatches.empty() && (!pitchTiming || supplementalPatches.size() != supplementalPatchBytes)) return {};
        if (bank.empty()) return {};
        std::vector<uint8_t> bytes{'S','C','5','5','M','D','0','1'};
        if (times) bytes[7] = '2';
        if (levels) bytes[7] = '3';
        if (keys) bytes[7] = '4';
        if (keyLevels) bytes[7] = '5';
        if (glideRates) bytes[7] = '6';
        if (pitchKeys) bytes[7] = '7';
        if (pitchEnvelope) bytes[7] = '8';
        if (secondEnvelope) bytes[7] = '9';
        if (secondPreparation) { bytes[6] = '1'; bytes[7] = '0'; }
        if (modulationRates) { bytes[6] = '1'; bytes[7] = '1'; }
        if (modulationPreparation) { bytes[6] = '1'; bytes[7] = '2'; }
        if (pan) { bytes[6] = '1'; bytes[7] = '3'; }
        bytes.insert(bytes.end(),patches.begin(),patches.end());
        for (const auto& curve : curves) bytes.insert(bytes.end(),curve.begin(),curve.end());
        if (times) for (auto time : *times)
        { bytes.push_back(uint8_t(time >> 8)); bytes.push_back(uint8_t(time)); }
        if (levels)
        {
            bytes.insert(bytes.end(),levels->attenuation.begin(),levels->attenuation.end());
            bytes.insert(bytes.end(),levels->level.begin(),levels->level.end());
        }
        if (keys)
        {
            for (auto value : keys->multipliers) { bytes.push_back(uint8_t(value >> 8)); bytes.push_back(uint8_t(value)); }
            for (const auto& curve : keys->attack) bytes.insert(bytes.end(),curve.begin(),curve.end());
            for (const auto& curve : keys->release) bytes.insert(bytes.end(),curve.begin(),curve.end());
        }
        if (keyLevels)
        {
            bytes.insert(bytes.end(),keyLevels->adjustment.begin(),keyLevels->adjustment.end());
            for (const auto& curve : keyLevels->curves) bytes.insert(bytes.end(),curve.begin(),curve.end());
        }
        if (glideRates) for (auto value : *glideRates)
        { bytes.push_back(uint8_t(value>>8)); bytes.push_back(uint8_t(value)); }
        if (pitchKeys) for (const auto& row : *pitchKeys) for (auto value : row)
        { bytes.push_back(uint8_t(value>>8)); bytes.push_back(uint8_t(value)); }
        if (pitchEnvelope)
        {
            const auto words = [&](const auto& values) { for (auto value : values)
            { bytes.push_back(uint8_t(value>>8)); bytes.push_back(uint8_t(value)); } };
            words(pitchEnvelope->depth.base); words(pitchEnvelope->depth.velocity); words(pitchEnvelope->depth.depth);
            bytes.insert(bytes.end(),pitchEnvelope->curve.begin(),pitchEnvelope->curve.end());
        }
        if (secondEnvelope)
        {
            bytes.insert(bytes.end(),secondEnvelope->smoothingCeiling.begin(),secondEnvelope->smoothingCeiling.end());
            for (auto value : secondEnvelope->levelCurve)
            { bytes.push_back(uint8_t(value>>8)); bytes.push_back(uint8_t(value)); }
            bytes.insert(bytes.end(),secondEnvelope->outputCeiling.begin(),secondEnvelope->outputCeiling.end());
        }
        if (secondPreparation)
        {
            const auto words = [&](const auto& values) { for (auto value : values)
            { bytes.push_back(uint8_t(value>>8)); bytes.push_back(uint8_t(value)); } };
            for (const auto& row : secondPreparation->keys) words(row);
            words(secondPreparation->targets.startSensitivity); words(secondPreparation->targets.velocitySensitivity); words(secondPreparation->targets.scale);
            for (const auto& row : secondPreparation->timing.attack) bytes.insert(bytes.end(),row.begin(),row.end());
            for (const auto& row : secondPreparation->timing.release) bytes.insert(bytes.end(),row.begin(),row.end());
        }
        if (modulationRates) for (auto value : *modulationRates)
        { bytes.push_back(uint8_t(value>>8)); bytes.push_back(uint8_t(value)); }
        if (modulationPreparation)
        {
            const auto words = [&](const auto& values) { for (auto value : values)
            { bytes.push_back(uint8_t(value>>8)); bytes.push_back(uint8_t(value)); } };
            words(modulationPreparation->timing);
            words(modulationPreparation->depths.secondEnvelope); words(modulationPreparation->depths.pitch);
        }
        if (pan) bytes.insert(bytes.end(),pan->begin(),pan->end());
        if (pitchTiming)
        {
            bytes[6] = '1'; bytes[7] = '4';
            for (const auto& curve : pitchTiming->attack) bytes.insert(bytes.end(),curve.begin(),curve.end());
            for (const auto& curve : pitchTiming->release) bytes.insert(bytes.end(),curve.begin(),curve.end());
        }
        if (!supplementalPatches.empty())
        {
            bytes[7] = '5';
            bytes.insert(bytes.end(),supplementalPatches.begin(),supplementalPatches.end());
        }
        bytes.insert(bytes.end(),bank.begin(),bank.end());
        return bytes;
    }

    bool loadEncoded(std::span<const uint8_t> bytes)
    {
        constexpr std::array<uint8_t,8> magic{'S','C','5','5','M','D','0','1'};
        constexpr size_t timeAt = headerBytes + patchBytes + curveBytes;
        if (bytes.size() < timeAt + 12 || !std::equal(magic.begin(),magic.begin()+6,bytes.begin())) return false;
        const unsigned version = bytes[6] == '0' && bytes[7] >= '1' && bytes[7] <= '9' ? unsigned(bytes[7]-'0')
            : bytes[6] == '1' && bytes[7] >= '0' && bytes[7] <= '5' ? 10u+unsigned(bytes[7]-'0') : 0u;
        if (version == 0) return false;
        const bool hasTimes = version >= 2;
        const bool hasLevels = version >= 3;
        const bool hasKeys = version >= 4;
        const bool hasKeyLevels = version >= 5;
        const bool hasGlideRates = version >= 6;
        const bool hasPitchKeys = version >= 7;
        const size_t pitchKeysAt = timeAt + (hasTimes ? 256 : 0) + (hasLevels ? 384 : 0) + (hasKeys ? 8704 : 0) + (hasKeyLevels ? 4225 : 0) + (hasGlideRates ? 256 : 0);
        const bool hasPitchEnvelope = version >= 8;
        const size_t pitchEnvelopeAt = pitchKeysAt + (hasPitchKeys ? 20480 : 0);
        const size_t secondEnvelopeAt = pitchEnvelopeAt + (hasPitchEnvelope ? 1536 : 0);
        const bool hasSecondEnvelope = version >= 9;
        const size_t secondPreparationAt = secondEnvelopeAt + (hasSecondEnvelope ? 642 : 0);
        const size_t modulationRatesAt = secondPreparationAt + (version >= 10 ? 17664 : 0);
        const size_t modulationPreparationAt = modulationRatesAt + (version >= 11 ? 512 : 0);
        const size_t panAt = modulationPreparationAt + (version >= 12 ? 1024 : 0);
        const size_t pitchTimingAt = panAt + (version >= 13 ? 129 : 0);
        const size_t supplementalAt = pitchTimingAt + (version >= 14 ? 8192 : 0);
        const size_t bankAt = supplementalAt + (version >= 15 ? supplementalPatchBytes : 0);
        if (bytes.size() < bankAt + 12) return false;
        auto next = std::make_unique<Storage>();
        if (!next->patches.loadRecords(bytes.subspan(headerBytes,patchBytes),0,224)
            || !next->samples.loadEncoded(bytes.subspan(bankAt))) return false;
        if (version >= 15)
        {
            next->supplemental = std::make_unique<SC55PatchTable>();
            if (!next->supplemental->loadRecords(bytes.subspan(supplementalAt,supplementalPatchBytes),0,162)) return false;
            for (int p = 0; p < 162; ++p)
                for (const auto& partial : (*next->supplemental)[p].partial)
                    if (partial.used && !next->samples.select(uint16_t((partial.raw[2]<<8)|partial.raw[3]),0)) return false;
        }
        for (int p = 0; p < next->patches.size(); ++p)
            for (const auto& partial : next->patches[p].partial)
                if (partial.used && !next->samples.select(uint16_t((partial.raw[2]<<8)|partial.raw[3]),0))
                    return false;
        for (size_t c = 0; c < next->curves.size(); ++c)
            std::copy_n(bytes.begin()+headerBytes+patchBytes+c*256,256,next->curves[c].begin());
        if (hasTimes)
        {
            next->times.emplace();
            for (size_t i = 0; i < 128; ++i)
                (*next->times)[i] = uint16_t((bytes[timeAt+2*i]<<8)|bytes[timeAt+2*i+1]);
        }
        if (hasLevels)
        {
            next->levels.emplace();
            std::copy_n(bytes.begin()+timeAt+256,128,next->levels->attenuation.begin());
            std::copy_n(bytes.begin()+timeAt+384,256,next->levels->level.begin());
        }
        if (hasKeys)
        {
            next->keys.emplace();
            size_t at = timeAt+640;
            for (auto& value : next->keys->multipliers) { value = uint16_t((bytes[at]<<8)|bytes[at+1]); at += 2; }
            for (auto& curve : next->keys->attack) { std::copy_n(bytes.begin()+at,256,curve.begin()); at += 256; }
            for (auto& curve : next->keys->release) { std::copy_n(bytes.begin()+at,256,curve.begin()); at += 256; }
        }
        if (hasKeyLevels)
        {
            next->keyLevels.emplace();
            size_t at = timeAt+640+8704;
            std::copy_n(bytes.begin()+at,129,next->keyLevels->adjustment.begin()); at += 129;
            for (auto& curve : next->keyLevels->curves) { std::copy_n(bytes.begin()+at,256,curve.begin()); at += 256; }
        }
        if (hasGlideRates)
        {
            next->glideRates.emplace();
            for (size_t i = 0; i < 128; ++i)
                (*next->glideRates)[i] = uint16_t((bytes[pitchKeysAt-256+2*i]<<8)|bytes[pitchKeysAt-255+2*i]);
        }
        if (hasPitchKeys)
        {
            next->pitchKeys.emplace();
            size_t at = pitchKeysAt;
            for (auto& row : *next->pitchKeys) for (auto& value : row)
            { value = uint16_t((bytes[at]<<8)|bytes[at+1]); at += 2; }
        }
        if (hasPitchEnvelope)
        {
            next->pitchEnvelope.emplace();
            size_t at = pitchEnvelopeAt;
            const auto words = [&](auto& values) { for (auto& value : values)
            { value = uint16_t((bytes[at]<<8)|bytes[at+1]); at += 2; } };
            words(next->pitchEnvelope->depth.base); words(next->pitchEnvelope->depth.velocity); words(next->pitchEnvelope->depth.depth);
            std::copy_n(bytes.begin()+at,256,next->pitchEnvelope->curve.begin());
        }
        if (hasSecondEnvelope)
        {
            next->secondEnvelope.emplace();
            size_t at = secondEnvelopeAt;
            std::copy_n(bytes.begin()+at,256,next->secondEnvelope->smoothingCeiling.begin()); at += 256;
            for (auto& value : next->secondEnvelope->levelCurve)
            { value = uint16_t((bytes[at]<<8)|bytes[at+1]); at += 2; }
            std::copy_n(bytes.begin()+at,128,next->secondEnvelope->outputCeiling.begin());
        }
        if (version >= 10)
        {
            next->secondPreparation.emplace(); size_t at = secondPreparationAt;
            const auto words = [&](auto& values) { for (auto& value : values)
            { value = uint16_t((bytes[at]<<8)|bytes[at+1]); at += 2; } };
            for (auto& row : next->secondPreparation->keys) words(row);
            words(next->secondPreparation->targets.startSensitivity); words(next->secondPreparation->targets.velocitySensitivity); words(next->secondPreparation->targets.scale);
            for (auto& row : next->secondPreparation->timing.attack) { std::copy_n(bytes.begin()+at,256,row.begin()); at += 256; }
            for (auto& row : next->secondPreparation->timing.release) { std::copy_n(bytes.begin()+at,256,row.begin()); at += 256; }
        }
        if (version >= 11)
        {
            next->modulationRates.emplace(); size_t at = modulationRatesAt;
            for (auto& value : *next->modulationRates)
            { value = uint16_t((bytes[at]<<8)|bytes[at+1]); at += 2; }
        }
        if (version >= 12)
        {
            next->modulationPreparation.emplace(); size_t at = modulationPreparationAt;
            const auto words = [&](auto& values) { for (auto& value : values)
            { value = uint16_t((bytes[at]<<8)|bytes[at+1]); at += 2; } };
            words(next->modulationPreparation->timing);
            words(next->modulationPreparation->depths.secondEnvelope); words(next->modulationPreparation->depths.pitch);
        }
        if (version >= 13)
        {
            next->pan.emplace();
            std::copy_n(bytes.begin()+panAt,129,next->pan->begin());
        }
        if (version >= 14)
        {
            next->pitchTiming.emplace(); size_t at = pitchTimingAt;
            for (auto& curve : next->pitchTiming->attack)
            { std::copy_n(bytes.begin()+at,256,curve.begin()); at += 256; }
            for (auto& curve : next->pitchTiming->release)
            { std::copy_n(bytes.begin()+at,256,curve.begin()); at += 256; }
        }
        data_ = std::move(next); // invalid imports preserve the last valid asset
        return true;
    }
    bool loaded() const noexcept { return bool(data_); }
    const SC55Patch* patch(unsigned index) const noexcept
    {
        if (!data_) return nullptr;
        if (index < 224) return &data_->patches[int(index)];
        return data_->supplemental && index < 386 ? &(*data_->supplemental)[int(index-224)] : nullptr;
    }
    unsigned patchCount() const noexcept { return data_ ? (data_->supplemental ? 386u : 224u) : 0u; }
    const VelocityCurves* curves() const noexcept { return data_ ? &data_->curves : nullptr; }
    const SampleBank* samples() const noexcept { return data_ ? &data_->samples : nullptr; }
    const EnvelopeTimes* times() const noexcept
    { return data_ && data_->times ? &*data_->times : nullptr; }
    const EnvelopeLevelTables* levels() const noexcept
    { return data_ && data_->levels ? &*data_->levels : nullptr; }
    const EnvelopeKeyTables* keys() const noexcept
    { return data_ && data_->keys ? &*data_->keys : nullptr; }
    const EnvelopeKeyLevelTables* keyLevels() const noexcept
    { return data_ && data_->keyLevels ? &*data_->keyLevels : nullptr; }
    const PitchGlideRates* glideRates() const noexcept
    { return data_ && data_->glideRates ? &*data_->glideRates : nullptr; }
    const PartPitchKeyTables* pitchKeys() const noexcept
    { return data_ && data_->pitchKeys ? &*data_->pitchKeys : nullptr; }
    const PitchEnvelopeTables* pitchEnvelope() const noexcept
    { return data_ && data_->pitchEnvelope ? &*data_->pitchEnvelope : nullptr; }
    const SecondEnvelopePcmTables* secondEnvelope() const noexcept
    { return data_ && data_->secondEnvelope ? &*data_->secondEnvelope : nullptr; }
    const SecondEnvelopePreparationTables* secondPreparation() const noexcept
    { return data_ && data_->secondPreparation ? &*data_->secondPreparation : nullptr; }
    const ModulationRates* modulationRates() const noexcept
    { return data_ && data_->modulationRates ? &*data_->modulationRates : nullptr; }
    const ModulationPreparationTables* modulationPreparation() const noexcept
    { return data_ && data_->modulationPreparation ? &*data_->modulationPreparation : nullptr; }
    const PanTable* pan() const noexcept
    { return data_ && data_->pan ? &*data_->pan : nullptr; }
    const PitchEnvelopeKeyCurves* pitchTiming() const noexcept
    { return data_ && data_->pitchTiming ? &*data_->pitchTiming : nullptr; }
private:
    // The previous formats intentionally expose missing preparation as null.
    // Optional in MD01..07; never manufacture neutral data for missing tables.
    struct Storage { SC55PatchTable patches; std::unique_ptr<SC55PatchTable> supplemental; VelocityCurves curves; SampleBank samples; std::optional<EnvelopeTimes> times; std::optional<EnvelopeLevelTables> levels; std::optional<EnvelopeKeyTables> keys; std::optional<EnvelopeKeyLevelTables> keyLevels; std::optional<PitchGlideRates> glideRates; std::optional<PartPitchKeyTables> pitchKeys; std::optional<PitchEnvelopeTables> pitchEnvelope; std::optional<SecondEnvelopePcmTables> secondEnvelope; std::optional<SecondEnvelopePreparationTables> secondPreparation; std::optional<ModulationRates> modulationRates; std::optional<ModulationPreparationTables> modulationPreparation; std::optional<PanTable> pan; std::optional<PitchEnvelopeKeyCurves> pitchTiming; };
    std::unique_ptr<Storage> data_;
};
}
