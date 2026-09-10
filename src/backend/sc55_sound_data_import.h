#pragma once
#include "sc55_sound_data.h"
#include "sc55_voice_setup.h"
#include "sc55_rhythm_presets.h"
#include "sc55_preset.h"
#include "sc55_system_defaults.h"
#include "sc55_effects_control.h"
#include "sha256.h"
#include <set>
#include <stdexcept>

namespace sc55
{
// Exact ROM identity is required: the offsets below are specific to SC-55 v1.21.
inline SHA256_Digest SoundDataDigest(std::span<const uint8_t> bytes)
{
    SHA256Context context;
    SHA256_Digest digest{};
    if (SHA256Reset(&context) != shaSuccess
        || SHA256Input(&context, bytes.data(), unsigned(bytes.size())) != shaSuccess
        || SHA256Result(&context, digest.data()) != shaSuccess)
        throw std::runtime_error("Sound-data SHA256 failed");
    return digest;
}
inline bool CanImportSoundData(const std::vector<uint8_t>& rom1, const std::vector<uint8_t>& rom2)
{
    return rom1.size() == 32768 && rom2.size() == 262144
        && SoundDataDigest(rom1) == SHA256_ToDigest("7e1bacd1d7c62ed66e465ba05597dcd60dfc13fc23de0287fdbce6cf906c6544")
        && SoundDataDigest(rom2) == SHA256_ToDigest("effc6132d68f7e300aaef915ccdd08aba93606c22d23e580daf9ea6617913af1");
}

// Setup-only extraction of 03:8000 /8080 /d168. Separate owned configuration
// for now; no cache-format change and no recovered data embedded in the source.
inline RhythmPresetTable ImportRhythmPresets(const std::vector<uint8_t>& rom1,
    const std::vector<uint8_t>& rom2)
{
    if (!CanImportSoundData(rom1,rom2))
        throw std::runtime_error("Rhythm import requires verified SC-55 v1.21 ROMs");
    RhythmPresetTable result;
    std::copy_n(rom2.begin()+0x38000,128,result.programs.begin());
    for (unsigned i = 0; i < result.records.size(); ++i)
        std::copy_n(rom2.begin()+0x38080+i*0x48c,0x48c,result.records[i].begin());
    std::copy_n(rom2.begin()+0x3d168,128,result.program127Accumulators.begin());
    return result;
}
inline SystemDefaults ImportSystemDefaults(const std::vector<uint8_t>& rom1,
    const std::vector<uint8_t>& rom2)
{
    if (!CanImportSoundData(rom1,rom2))
        throw std::runtime_error("System defaults import requires verified SC-55 v1.21 ROMs");
    SystemDefaults result;
    std::copy_n(rom2.begin()+0x3ca00,result.bytes.size(),result.bytes.begin());
    std::copy_n(rom2.begin()+0x3d148,result.identity.size(),result.identity.begin());
    return result;
}

inline EffectsTables ImportEffectsTables(const std::vector<uint8_t>& rom1,
    const std::vector<uint8_t>& rom2)
{
    if (!CanImportSoundData(rom1,rom2))
        throw std::runtime_error("Effects import requires verified SC-55 v1.21 ROMs");
    EffectsTables result;
    for (unsigned i=0;i<8;++i) {
        std::copy_n(rom2.begin()+0x10+6*i,6,result.reverbMacros[i].begin());
        std::copy_n(rom2.begin()+0x40+7*i,7,result.chorusMacros[i].begin());
        result.reverbLpf[i]=uint16_t((rom2[0x88+2*i]<<8)|rom2[0x89+2*i]);
        result.chorusLpf[i]=uint16_t((rom2[0x98+2*i]<<8)|rom2[0x99+2*i]);
        const unsigned address=(rom2[0x78+2*i]<<8)|rom2[0x79+2*i];
        for (unsigned j=0;j<26;++j)
            result.reverbPrograms[i][j]=uint16_t((rom2[address+2*j]<<8)|rom2[address+2*j+1]);
    }
    return result;
}

inline MelodicPresetTable ImportMelodicPresets(const std::vector<uint8_t>& rom1,
    const std::vector<uint8_t>& rom2)
{
    if (!CanImportSoundData(rom1,rom2))
        throw std::runtime_error("Melodic preset import requires verified SC-55 v1.21 ROMs");
    MelodicPresetTable result;
    for (unsigned i = 0; i < result.tones.size(); ++i)
        result.tones[i] = uint16_t((rom2[0x30000+2*i]<<8)|rom2[0x30001+2*i]);
    return result;
}

// This versioned cache is deterministic for the ROM pair above. A full digest
// rejects corruption as well as stale/incomplete formats; no ROM data is embedded.
inline bool IsCurrentSoundData(std::span<const uint8_t> bytes)
{
    return bytes.size() == 184480
        && SoundDataDigest(bytes) == SHA256_ToDigest("1c73f4e219a58e422e14f78e3dd2f068e0bf834567cf7038121ea80ce26d9b3e");
}

namespace sound_data_import_detail
{
inline sc55::EnvelopeKeyLevelTables importEnvelopeKeyLevels(std::span<const uint8_t> rom1,std::span<const uint8_t> rom2)
{
    if (rom1.size() != 32768 || rom2.size() != 262144) throw std::runtime_error("Unexpected key-level ROM size");
    sc55::EnvelopeKeyLevelTables result;
    std::copy_n(rom1.begin()+0x69c6,129,result.adjustment.begin());
    for (unsigned curve = 0; curve < 16; ++curve)
    {
        const unsigned at = 0x3dc72+curve*2;
        const uint16_t pointer = uint16_t((rom2[at]<<8)|rom2[at+1]);
        for (unsigned key = 0; key < 256; ++key) result.curves[curve][key] = rom2[0x30000|uint16_t(pointer+key)];
    }
    return result;
}

// ROM import only; never called from the audio callback.
inline sc55::EnvelopeKeyTables importEnvelopeKeys(std::span<const uint8_t> rom1,std::span<const uint8_t> rom2)
{
    if (rom1.size() != 32768 || rom2.size() != 262144) throw std::runtime_error("Unexpected key-table ROM size");
    sc55::EnvelopeKeyTables result;
    for (unsigned i = 0; i < 256; ++i)
        result.multipliers[i] = uint16_t((rom1[0x67c6+2*i]<<8)|rom1[0x67c7+2*i]);
    for (unsigned curve = 0; curve < 16; ++curve)
        for (unsigned kind = 0; kind < 2; ++kind)
        {
            const unsigned at = 0x3dc92+kind*32+curve*2;
            const uint16_t pointer = uint16_t((rom2[at]<<8)|rom2[at+1]);
            auto& output = kind == 0 ? result.attack[curve] : result.release[curve];
            for (unsigned key = 0; key < 256; ++key) output[key] = rom2[0x30000|uint16_t(pointer+key)];
        }
    return result;
}

}

// Setup-time extraction only: no H8 execution, wave ROM access or oracle tests.
inline std::vector<uint8_t> ImportSoundData(const std::vector<uint8_t>& rom1,
                                          const std::vector<uint8_t>& rom2)
{
    if (!CanImportSoundData(rom1, rom2))
        throw std::runtime_error("Native sound-data import requires the verified SC-55 v1.21 ROM pair");
    using namespace sound_data_import_detail;
    SampleBank sampleBank;
    VelocityCurves velocityCurves{};
    EnvelopeTimes envelopeTimes{};
    EnvelopeLevelTables envelopeLevels{};
    for (unsigned curve = 0; curve < velocityCurves.size(); ++curve)
    {
        const unsigned pointer = (rom1.at(0x111a + curve * 2) << 8) | rom1.at(0x111b + curve * 2);
        for (unsigned index = 0; index < 256; ++index)
            velocityCurves[curve][index] = rom2.at(0x30000 | uint16_t(pointer + index));
    }
    for (unsigned i = 0; i < envelopeTimes.size(); ++i)
        envelopeTimes[i] = uint16_t((rom1.at(0x6f12+2*i)<<8)|rom1.at(0x6f13+2*i));
    std::copy_n(rom1.begin()+0x6b0f,128,envelopeLevels.attenuation.begin());
    std::copy_n(rom1.begin()+0x6b8f,256,envelopeLevels.level.begin());
    std::set<uint16_t> groupIds, sampleIds;
    // Both verified banks, excluding the absent-partial sentinel.
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned patch = 0; patch < (bank == 0 ? 224u : 162u); ++patch)
            for (unsigned partial = 0; partial < 2; ++partial)
            {
                const auto at = (bank == 0 ? 0x10000u : 0x20000u)
                    + patch * SC55Patch::SIZE + SC55Patch::PARTIAL_OFFSET
                    + partial * SC55Partial::SIZE + 2;
                const auto id = uint16_t((rom2.at(at) << 8) | rom2.at(at + 1));
                if (id != 0xffff) groupIds.insert(id);
            }
    std::vector<sc55::SampleBank::Group> groups;
    std::vector<sc55::SampleBank::Sample> samples;
    const auto readRecord = [&](uint32_t offset,auto& data) {
        for (unsigned i = 0; i < data.size(); ++i)
            data[i] = rom2.at((offset & 0xffff0000u) | uint16_t(offset+i));
    };
    for (auto id : groupIds)
    {
        sc55::SampleBank::Group group{id,{}};
        readRecord(sc55::V121MultisampleOffset(id),group.data);
        for (unsigned key = 0; key < 128; ++key)
        {
            const auto sample = sc55::SelectSampleZone(group.data,uint8_t(key));
            if (!sample) throw std::runtime_error("Imported group has an uncovered key");
            if (!(*sample & 0x8000)) sampleIds.insert(*sample);
        }
        groups.push_back(group);
    }
    for (auto id : sampleIds)
    {
        sc55::SampleBank::Sample sample{id,{}};
        const auto offset = sc55::V121SampleDescriptorOffset(id);
        if (!offset) throw std::runtime_error("Negative sample ID entered descriptor import");
        readRecord(*offset,sample.data);
        samples.push_back(sample);
    }
    if (!sampleBank.load(std::move(groups),std::move(samples))) throw std::runtime_error("Invalid sample bank");
    const auto envelopeKeys = importEnvelopeKeys(rom1,rom2);
    const auto envelopeKeyLevels = importEnvelopeKeyLevels(rom1,rom2);
    sc55::PitchGlideRates glideRates;
    for (unsigned i = 0; i < glideRates.size(); ++i)
        glideRates[i] = uint16_t((rom1.at(0x7a32+2*i)<<8)|rom1.at(0x7a33+2*i));
    sc55::PartPitchKeyTables pitchKeys{};
    for (unsigned selector = 1; selector < pitchKeys.size(); ++selector)
    {
        const unsigned pointerAt = 0x3dd32+2*selector;
        const unsigned pointer = (rom2.at(pointerAt)<<8)|rom2.at(pointerAt+1);
        for (unsigned key = 0; key < 256; ++key)
        {
            const unsigned at = 0x30000+((pointer+2*key)&0xffff);
            pitchKeys[selector][key] = uint16_t((rom2.at(at)<<8)|rom2.at(0x30000+((at+1)&0xffff)));
        }
    }
    sc55::PitchEnvelopeTables pitchEnvelope;
    const auto importWords = [&](auto& values,unsigned at) {
        for (unsigned i = 0; i < values.size(); ++i) values[i] = uint16_t((rom1.at(at+2*i)<<8)|rom1.at(at+2*i+1));
    };
    importWords(pitchEnvelope.depth.base,0x78c6); importWords(pitchEnvelope.depth.velocity,0x78dc); importWords(pitchEnvelope.depth.depth,0x78f2);
    std::copy_n(rom1.begin()+0x79f2,256,pitchEnvelope.curve.begin());
    sc55::SecondEnvelopePcmTables secondEnvelope;
    importWords(secondEnvelope.levelCurve,0x7612);
    std::copy_n(rom1.begin()+0x7714,256,secondEnvelope.smoothingCeiling.begin());
    std::copy_n(rom1.begin()+0x7816,128,secondEnvelope.outputCeiling.begin());
    sc55::SecondEnvelopePreparationTables secondPreparation;
    importWords(secondPreparation.targets.startSensitivity,0x74d2);
    importWords(secondPreparation.targets.velocitySensitivity,0x74fc);
    importWords(secondPreparation.targets.scale,0x7512);
    const auto rom2word = [&](unsigned at) { return uint16_t((rom2.at(at)<<8)|rom2.at(at+1)); };
    for (unsigned selector = 0; selector < 16; ++selector)
    {
        const unsigned keyPointer = rom2word(0x3dcd2+selector*2);
        const unsigned attackPointer = rom2word(0x3dcf2+selector*2), releasePointer = rom2word(0x3dd12+selector*2);
        for (unsigned key = 0; key < 256; ++key)
        {
            secondPreparation.keys[selector][key] = uint16_t((rom2.at(0x30000+((keyPointer+key*2)&65535))<<8)
                |rom2.at(0x30000+((keyPointer+key*2+1)&65535)));
            secondPreparation.timing.attack[selector][key] = rom2.at(0x30000+((attackPointer+key)&65535));
            secondPreparation.timing.release[selector][key] = rom2.at(0x30000+((releasePointer+key)&65535));
        }
    }
    sc55::SoundData::ModulationRates modulationRates;
    importWords(modulationRates,0x7012);
    sc55::ModulationPreparationTables modulationPreparation;
    importWords(modulationPreparation.timing,0x7112);
    importWords(modulationPreparation.depths.secondEnvelope,0x7212);
    importWords(modulationPreparation.depths.pitch,0x7312);
    sc55::PanTable pan;
    std::copy_n(rom1.begin()+0x6c8f,pan.size(),pan.begin());
    sc55::PitchEnvelopeKeyCurves pitchTiming;
    for (unsigned selector = 0; selector < 16; ++selector)
        for (unsigned key = 0; key < 256; ++key)
        {
            pitchTiming.attack[selector][key] = rom2.at(0x30000|uint16_t(rom2word(0x3dd42+2*selector)+key));
            pitchTiming.release[selector][key] = rom2.at(0x30000|uint16_t(rom2word(0x3dd62+2*selector)+key));
        }
    const auto soundEncoded = sc55::SoundData::encode(
        std::span(rom2).subspan(0x10000,sc55::SoundData::patchBytes),velocityCurves,sampleBank,&envelopeTimes,&envelopeLevels,&envelopeKeys,&envelopeKeyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&secondPreparation,&modulationRates,&modulationPreparation,&pan,&pitchTiming,
        std::span<const uint8_t>(rom2).subspan(0x20000,sc55::SoundData::supplementalPatchBytes));

    if (!IsCurrentSoundData(soundEncoded))
        throw std::runtime_error("Generated native sound data failed verification");
    return soundEncoded;
}
}
