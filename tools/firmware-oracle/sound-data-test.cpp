#include "sc55_sound_data.h"
#include "sc55_voice_control.h"
#include "sc55_lfo.h"
#include "sc55_voice_lifecycle.h"
#include "sc55_note_setup.h"
#include "sc55_note_dispatch.h"
#include "sc55_rhythm_admission.h"
#include "sc55_sample_install.h"
#include "sc55_envelope_setup.h"
#include "sc55_envelope.h"
#include "sc55_envelope_runner.h"
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cstdio>

int main(int argc, char** argv)
{
    const auto require = [](bool ok) { if (!ok) throw std::runtime_error("sound data regression"); };
    const std::array<SC55ExpandedParameter,5> stageParameters{{{10,0},{20,4},{30,0},{40,4},{50,0}}};
    const std::array<uint8_t,4> stageTargets{100,80,60,40};
    sc55::EnvelopeStageState stageState{sc55::EnvelopeStage::attack1,{65534,3},stageParameters[0],0,100};
    stageState = sc55::AdvanceEnvelopeStage(stageState,stageParameters,stageTargets);
    require(stageState.stage == sc55::EnvelopeStage::attack1 && stageState.progress.position == 65534);
    stageState.progress.position = 65535;
    stageState = sc55::AdvanceEnvelopeStage(stageState,stageParameters,stageTargets);
    require(stageState.stage == sc55::EnvelopeStage::attack2 && stageState.progress.position == 0
        && stageState.progress.deferredTicks == 3 && stageState.parameter.value == 20
        && stageState.parameter.flag == 4 && stageState.start == 100 && stageState.target == 80);
    for (unsigned expected = 3; expected <= 7; ++expected)
    {
        stageState.progress.position = 65535;
        stageState = sc55::AdvanceEnvelopeStage(stageState,stageParameters,stageTargets);
        require(unsigned(stageState.stage) == expected && stageState.progress.deferredTicks == 3);
    }
    require(stageState.stage == sc55::EnvelopeStage::finished);
    const auto release = sc55::ReleaseEnvelope({sc55::EnvelopeStage::sustain,{123,9},{10,0},20,100},0xabcd,{55,4},0);
    require(release.stage == sc55::EnvelopeStage::release && release.start == 0xab && release.target == 0
        && release.progress.position == 0 && release.progress.deferredTicks == 0 && release.parameter.value == 55 && release.parameter.flag == 4);
    const auto repeatedRelease = sc55::ReleaseEnvelope(release,0xffff,{1,0},0);
    require(repeatedRelease.start == 0xab && repeatedRelease.parameter.value == 55);
    const sc55::EnvelopeStageState delayed{sc55::EnvelopeStage::delay,{42,7},{3,4},8,9};
    const auto cancelledDelay = sc55::ReleaseEnvelope(delayed,0xffff,{55,0},1);
    require(cancelledDelay.stage == sc55::EnvelopeStage::finished && cancelledDelay.progress.position == 42
        && cancelledDelay.progress.deferredTicks == 7 && cancelledDelay.target == 9);
    require(sc55::ReleaseEnvelope(delayed,0,{55,0},0).stage == sc55::EnvelopeStage::attack1);
    stageState = sc55::AdvanceEnvelopeStage(stageState,stageParameters,stageTargets);
    require(stageState.stage == sc55::EnvelopeStage::finished);
    require(sc55::ScaleEnvelopeTime(65279,256) == 65279);
    require(sc55::ScaleEnvelopeTime(65280,256) == 65535);
    require(sc55::ScaleEnvelopeTime(65535,0) == 0);
    sc55::EnvelopeTimes syntheticTimes;
    for (unsigned i = 0; i < 128; ++i) syntheticTimes[i] = uint16_t(i*511);
    require(sc55::PrepareEnvelopeDuration(60,64,256,256,syntheticTimes) == syntheticTimes[60]);
    require(sc55::PrepareEnvelopeDuration(60,0,256,256,syntheticTimes) == 0);
    require(sc55::PrepareEnvelopeDuration(60,127,256,256,syntheticTimes) == syntheticTimes[127]);
    require(!sc55::PrepareEnvelopeDuration(128,64,256,256,syntheticTimes));
    for (unsigned duration = 0; duration <= 8; ++duration)
        require(!sc55::AdvanceEnvelopeProgress(uint16_t(duration),1,{0,0}));
    const auto firstTick = sc55::AdvanceEnvelopeProgress(16,1,{0,0});
    require(firstTick && firstTick->position == 32768 && firstTick->deferredTicks == 0);
    const auto crossed = sc55::AdvanceEnvelopeProgress(16,2,*firstTick);
    require(crossed && crossed->position == 65535 && crossed->deferredTicks == 1);
    const auto nextStage = sc55::AdvanceEnvelopeProgress(16,0,{0,crossed->deferredTicks});
    require(nextStage && nextStage->position == 32768 && nextStage->deferredTicks == 0);
    const auto wrappedTicks = sc55::AdvanceEnvelopeProgress(16,65535,{123,1});
    require(wrappedTicks && wrappedTicks->position == 123 && wrappedTicks->deferredTicks == 0);
    require(sc55::EnvelopeSegment(0,100,firstTick->position,false) == 12800);
    require(sc55::EnvelopeSegment(100,0,firstTick->position,false) == 12800);
    require(sc55::EnvelopeSegment(20,100,crossed->position,true) == 25600);
    require(sc55::EncodeEnvelope(25600,25600) == 0xff00);
    const auto step = sc55::StepEnvelopeSegment(16,1,{0,0},0,100,false,0);
    require(step && step->level == 12800 && step->progress.position == 32768
        && step->pcmWord == sc55::EncodeEnvelope(0,12800));
    const auto immediate = sc55::StepEnvelopeSegment(0,1,{0,3},0,100,false,0);
    require(immediate && immediate->level == 25600 && immediate->pcmWord == 0x64af
        && immediate->progress.position == 65535 && immediate->progress.deferredTicks == 3);
    const auto shortHold = sc55::StepEnvelopeSegment(1,1,{0,3},0,100,false,25600);
    require(shortHold && shortHold->pcmWord == 0xff00 && shortHold->progress.deferredTicks == 3);
    std::vector<uint8_t> patches(sc55::SoundData::patchBytes,0);
    for (size_t at = 0; at < patches.size(); at += SC55Patch::SIZE)
    {
        std::fill_n(patches.begin()+at,12,'A');
        patches[at+SC55Patch::COMMON_OFFSET+6] = 3;
        for (size_t p : {size_t(32),size_t(124)}) patches[at+p+0x43] = 127;
    }
    sc55::SampleBank samples;
    sc55::SampleBank::Group group{0,{}};
    std::fill_n(group.data.begin()+12,16,127); // every zone points to sample 0
    require(samples.load({group},{{0,{}}}));
    SC55Partial partial;
    require(sc55::EnvelopeVelocityScale(127,64) == 256);
    require(sc55::EnvelopeVelocityScale(127,44) == 255);
    require(sc55::EnvelopeVelocityScale(0,44) == 4);
    require(sc55::EnvelopeVelocityScale(64,84) == 32512);
    require(!sc55::EnvelopeVelocityScale(100,43) && !sc55::EnvelopeVelocityScale(100,85));
    partial.raw[0x59] = partial.raw[0x5a] = 64;
    for (unsigned value = 0; value < 256; ++value)
    {
        for (unsigned stage = 0; stage < 5; ++stage) partial.raw[0x4e + stage] = uint8_t(value+stage);
        const auto envelope = sc55::PreparePartialEnvelope(partial,100);
        require(envelope && envelope->velocityScale1 == 256 && envelope->velocityScale2 == 256);
        for (unsigned stage = 0; stage < 5; ++stage)
        {
            const auto byte = uint8_t(value+stage);
            require(envelope->stages[stage].value == (byte & 127)
                && envelope->stages[stage].flag == ((byte & 128) ? 0 : 4));
        }
    }
    partial.used = true;
    partial.raw[1] = partial.raw[10] = partial.raw[13] = 64;
    std::array<uint8_t,12> scale; scale.fill(64);
    auto prepared = sc55::PreparePartialSample(partial,samples,60,60,60,scale,255,0x80,9);
    require(prepared && prepared->pitch.key == 60 && prepared->pitch.fraction == 0
        && prepared->key.lookupKey == 60 && prepared->key.storedAdjustedKey == 60
        && !prepared->key.storedOriginalNote && prepared->sampleId == 0
        && prepared->normalStart->start == 0 && prepared->unoffsetStart->end == 0);
    prepared = sc55::PreparePartialSample(partial,samples,60,60,60,scale,70,0,9);
    require(prepared && prepared->key.lookupKey == 70 && prepared->key.storedOriginalNote == 9);
    partial.used = false;
    require(!sc55::PreparePartialSample(partial,samples,60,60,60,scale,255,0x80,9));
    partial.used = true; partial.raw[13] = 85;
    require(!sc55::PreparePartialSample(partial,samples,60,60,60,scale,255,0x80,9));
    partial.raw[13] = 64; partial.raw[3] = 1;
    require(!sc55::PreparePartialSample(partial,samples,60,60,60,scale,255,0x80,9));
    partial.raw[3] = 0;
    sc55::SampleBank specialSamples;
    auto specialGroup = group;
    for (unsigned zone = 0; zone < 16; ++zone) specialGroup.data[28+zone*2] = 0x80;
    require(specialSamples.load({specialGroup},{}));
    prepared = sc55::PreparePartialSample(partial,specialSamples,60,60,60,scale,255,0x81,9);
    require(prepared && prepared->sampleId == 0x8000 && !prepared->normalStart
        && !prepared->unoffsetStart && !prepared->key.storedAdjustedKey);
    sc55::VelocityCurves curves{};
    for (size_t c = 0; c < 16; ++c)
        for (size_t i = 0; i < 256; ++i) curves[c][i] = uint8_t(i+c);
    auto bytes = sc55::SoundData::encode(patches,curves,samples);
    sc55::SoundData data;
    require(!data.loaded() && !data.patch(0) && !data.curves() && !data.samples());
    require(data.loadEncoded(bytes) && data.patch(223) && !data.patch(224));
    require(*data.curves() == curves && data.samples()->sample(0));
    const auto* preserved = data.patch(0);
    for (size_t length : {size_t(0),size_t(7),size_t(8),
                         sc55::SoundData::headerBytes+sc55::SoundData::patchBytes-1,
                         sc55::SoundData::headerBytes+sc55::SoundData::patchBytes+sc55::SoundData::curveBytes-1,
                         bytes.size()-1})
        require(!data.loadEncoded(std::span(bytes).first(length)) && data.patch(0) == preserved);
    auto invalid = bytes;
    invalid[0] = 'X'; require(!data.loadEncoded(invalid));
    invalid = bytes; invalid.push_back(0); require(!data.loadEncoded(invalid));
    invalid = bytes; invalid[8+32+3] = 1; // missing multisample group
    require(!data.loadEncoded(invalid) && data.patch(0) == preserved);
    invalid = bytes; invalid[8] = 0; require(!data.loadEncoded(invalid));
    // Ownership: modifying the import buffer must not affect runtime data.
    std::fill(bytes.begin(),bytes.end(),0);
    require(data.patch(0)->name == "AAAAAAAAAAAA" && *data.curves() == curves);
    require(!data.times()); // old SC55MD01 never invents a timing table
    auto version2 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes);
    require(data.loadEncoded(version2) && data.times() && *data.times() == syntheticTimes);
    auto brokenVersion2 = version2; brokenVersion2.pop_back();
    require(!data.loadEncoded(brokenVersion2) && *data.times() == syntheticTimes);
    brokenVersion2 = version2; brokenVersion2[7] = '3'; require(!data.loadEncoded(brokenVersion2));
    require(data.loadEncoded(sc55::SoundData::encode(patches,curves,samples)) && !data.times());
    require(data.loadEncoded(version2));
    sc55::EnvelopeLevelTables levels{};
    for (unsigned i = 0; i < 128; ++i) levels.attenuation[i] = uint8_t(i*2);
    for (unsigned i = 0; i < 256; ++i) levels.level[i] = uint8_t(255-i);
    const auto version3 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels);
    require(data.loadEncoded(version3) && data.levels() && data.levels()->level == levels.level
        && data.levels()->attenuation == levels.attenuation && *data.times() == syntheticTimes);
    require(sc55::SoundData::encode(patches,curves,samples,nullptr,&levels).empty());
    for (size_t length : {size_t(8),version3.size()-1,version2.size()})
        require(!data.loadEncoded(std::span(version3).first(length)) && data.levels()->level == levels.level);
    SC55Partial targetPartial{};
    targetPartial.raw[0x4a] = 0; targetPartial.raw[0x4b] = 1;
    targetPartial.raw[0x4c] = 127; targetPartial.raw[0x4d] = 2;
    require(sc55::PrepareEnvelopeTargets(targetPartial,2,*data.levels()) == std::array<uint8_t,4>{253,255,255,255});
    targetPartial.raw[0x4c] = 128;
    require(!sc55::PrepareEnvelopeTargets(targetPartial,2,*data.levels()));
    require(sc55::PrepareEnvelopeBase(10,1,1,1,levels) == 4);
    require(sc55::PrepareEnvelopeBase(2,1,0,0,levels) == 1);
    require(sc55::PrepareEnvelopeBase(0,0,0,0,levels) == 1);
    require(sc55::PrepareEnvelopeBase(255,127,127,127,levels) == 1);
    require(!sc55::PrepareEnvelopeBase(255,128,0,0,levels));
    require(!sc55::PrepareEnvelopeBase(255,0,128,0,levels));
    require(!sc55::PrepareEnvelopeBase(255,0,0,128,levels));
    require(data.loadEncoded(version2) && !data.levels());
    sc55::EnvelopeKeyTables keyTables{};
    for (unsigned i = 0; i < 256; ++i) keyTables.multipliers[i] = uint16_t(i*257);
    for (unsigned c = 0; c < 16; ++c)
        for (unsigned k = 0; k < 256; ++k)
        { keyTables.attack[c][k] = uint8_t(c+k); keyTables.release[c][k] = uint8_t(c-k); }
    const auto version4 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables);
    require(data.loadEncoded(version4) && data.keys() && data.keys()->multipliers == keyTables.multipliers
        && data.keys()->attack == keyTables.attack && data.keys()->release == keyTables.release);
    const auto* savedKeys = data.keys();
    for (size_t length : {size_t(8),version3.size(),version4.size()-1})
        require(!data.loadEncoded(std::span(version4).first(length)) && data.keys() == savedKeys);
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,nullptr,&keyTables).empty());
    require(sc55::EnvelopeKeyScale(0,64,keyTables.multipliers) == 256);
    require(sc55::EnvelopeKeyScale(0,44,keyTables.multipliers) == keyTables.multipliers[255]);
    require(sc55::EnvelopeKeyScale(0,84,keyTables.multipliers) == keyTables.multipliers[0]);
    require(sc55::EnvelopeKeyScale(128,84,keyTables.multipliers) == 256);
    require(!sc55::EnvelopeKeyScale(0,43,keyTables.multipliers));
    targetPartial.raw[0x55] = 16;
    require(!sc55::PrepareEnvelopeKeyScales(targetPartial,0,keyTables));
    require(data.loadEncoded(version2) && !data.keys());
    sc55::EnvelopeKeyLevelTables keyLevels{};
    for (unsigned i = 0; i < 129; ++i) keyLevels.adjustment[i] = uint8_t(i);
    keyLevels.curves = keyTables.attack;
    const auto version5 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels);
    require(data.loadEncoded(version5) && data.keyLevels() && data.keyLevels()->adjustment == keyLevels.adjustment
        && data.keyLevels()->curves == keyLevels.curves && data.keys()->release == keyTables.release);
    const auto* savedKeyLevels = data.keyLevels();
    for (size_t length : {size_t(8),version4.size(),version5.size()-1})
        require(!data.loadEncoded(std::span(version5).first(length)) && data.keyLevels() == savedKeyLevels);
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,nullptr,&keyLevels).empty());
    sc55::PitchGlideRates glideRates;
    for (unsigned i = 0; i < glideRates.size(); ++i) glideRates[i] = uint16_t(i*503);
    const auto version6 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates);
    require(version6[7] == '6' && version6.size() == version5.size()+256);
    require(data.loadEncoded(version6) && data.glideRates() && *data.glideRates() == glideRates);
    const auto* savedRates = data.glideRates();
    for (size_t length : {size_t(8),version5.size(),version6.size()-1})
        require(!data.loadEncoded(std::span(version6).first(length)) && data.glideRates() == savedRates);
    auto unknownVersion = version6; unknownVersion[7] = '9';
    require(!data.loadEncoded(unknownVersion) && data.glideRates() == savedRates);
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,nullptr,&glideRates).empty());
    require(data.loadEncoded(version5) && !data.glideRates());
    sc55::PartPitchKeyTables pitchKeys{};
    for (unsigned row = 0; row < pitchKeys.size(); ++row)
        for (unsigned key = 0; key < 256; ++key) pitchKeys[row][key] = uint16_t(row*1000+key);
    const auto version7 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys);
    require(version7[7] == '7' && version7.size() == version6.size()+20480);
    require(data.loadEncoded(version7) && data.pitchKeys() && *data.pitchKeys() == pitchKeys && *data.glideRates() == glideRates);
    const auto* savedPitchKeys = data.pitchKeys();
    for (size_t length : {size_t(8),version6.size(),version7.size()-1})
        require(!data.loadEncoded(std::span(version7).first(length)) && data.pitchKeys() == savedPitchKeys);
    require(sc55::ApplyPartPitchKeyTable(60000,0,255,pitchKeys) == 60000);
    require(!sc55::ApplyPartPitchKeyTable(60000,40,0,pitchKeys));
    require(!sc55::ApplyPartPitchKeyTable(60000,255,0,pitchKeys));
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,nullptr,&pitchKeys).empty());
    require(data.loadEncoded(version6) && !data.pitchKeys() && data.glideRates());
    sc55::PitchEnvelopeTables pitchEnvelope;
    for (unsigned i = 0; i < 192; ++i) { pitchEnvelope.depth.base[i] = uint16_t(i*337); pitchEnvelope.depth.velocity[i] = uint16_t(i*599); }
    for (unsigned i = 0; i < 256; ++i) { pitchEnvelope.depth.depth[i] = uint16_t(i*257); pitchEnvelope.curve[i] = uint8_t(255-i); }
    const auto version8 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope);
    require(version8[7] == '8' && version8.size() == version7.size()+1536);
    require(data.loadEncoded(version8) && data.pitchEnvelope() && *data.pitchEnvelope() == pitchEnvelope && *data.pitchKeys() == pitchKeys && *data.glideRates() == glideRates);
    const auto* savedPitchEnvelope = data.pitchEnvelope();
    for (size_t length : {size_t(8),version7.size(),version8.size()-1})
        require(!data.loadEncoded(std::span(version8).first(length)) && data.pitchEnvelope() == savedPitchEnvelope);
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,nullptr,&pitchEnvelope).empty());
    require(data.loadEncoded(version7) && !data.pitchEnvelope() && data.pitchKeys());
    sc55::SecondEnvelopePcmTables secondEnvelope;
    for (unsigned i = 0; i < 256; ++i) secondEnvelope.smoothingCeiling[i] = uint8_t(i);
    for (unsigned i = 0; i < 129; ++i) secondEnvelope.levelCurve[i] = uint16_t(i*337);
    for (unsigned i = 0; i < 128; ++i) secondEnvelope.outputCeiling[i] = uint8_t(i*3);
    const auto version9 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope);
    require(version9[7] == '9' && version9.size() == version8.size()+642);
    require(data.loadEncoded(version9) && data.secondEnvelope() && *data.secondEnvelope() == secondEnvelope);
    const auto* savedSecond = data.secondEnvelope();
    for (size_t length : {size_t(8),version8.size(),version9.size()-1})
        require(!data.loadEncoded(std::span(version9).first(length)) && data.secondEnvelope() == savedSecond);
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,nullptr,&secondEnvelope).empty());
    require(data.loadEncoded(version8) && !data.secondEnvelope());
    sc55::SecondEnvelopePreparationTables preparation;
    for (unsigned row = 0; row < 16; ++row) for (unsigned i = 0; i < 256; ++i)
    { preparation.keys[row][i] = uint16_t(i*313+row); preparation.timing.attack[row][i] = uint8_t(i+row); preparation.timing.release[row][i] = uint8_t(i-row); }
    for (unsigned i = 0; i < 192; ++i) { preparation.targets.startSensitivity[i] = uint16_t(i*977); preparation.targets.velocitySensitivity[i] = uint16_t(i*41); }
    for (unsigned i = 0; i < 256; ++i) preparation.targets.scale[i] = uint16_t(i*73);
    const auto version10 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&preparation);
    require(version10[6] == '1' && version10[7] == '0' && version10.size() == version9.size()+17664);
    require(data.loadEncoded(version10) && data.secondPreparation() && *data.secondPreparation() == preparation);
    const auto* savedPreparation = data.secondPreparation();
    for (size_t length : {size_t(8),version9.size(),version10.size()-1})
        require(!data.loadEncoded(std::span(version10).first(length)) && data.secondPreparation() == savedPreparation);
    auto invalid10 = version10; invalid10[7] = '4'; require(!data.loadEncoded(invalid10));
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,nullptr,&preparation).empty());
    require(data.loadEncoded(version9) && !data.secondPreparation());
    sc55::SoundData::ModulationRates modulationRates;
    for (unsigned i = 0; i < modulationRates.size(); ++i) modulationRates[i] = uint16_t(i*257);
    const auto version11 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&preparation,&modulationRates);
    require(version11[6] == '1' && version11[7] == '1' && version11.size() == version10.size()+512);
    require(data.loadEncoded(version11) && data.modulationRates() && *data.modulationRates() == modulationRates);
    const auto* savedModulation = data.modulationRates();
    for (size_t length : {size_t(8),version10.size(),version11.size()-1})
        require(!data.loadEncoded(std::span(version11).first(length)) && data.modulationRates() == savedModulation);
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,nullptr,&modulationRates).empty());
    require(data.loadEncoded(version10) && !data.modulationRates());
    sc55::ModulationPreparationTables modulationPreparation;
    for (unsigned i = 0; i < 256; ++i) modulationPreparation.timing[i] = uint16_t(i*177);
    for (unsigned i = 0; i < 128; ++i) { modulationPreparation.depths.secondEnvelope[i] = uint16_t(i*333); modulationPreparation.depths.pitch[i] = uint16_t(i*999); }
    const auto version12 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&preparation,&modulationRates,&modulationPreparation);
    require(version12[6] == '1' && version12[7] == '2' && version12.size() == version11.size()+1024);
    require(data.loadEncoded(version12) && data.modulationPreparation() && *data.modulationPreparation() == modulationPreparation);
    const auto* savedModulationPreparation = data.modulationPreparation();
    for (size_t length : {size_t(8),version11.size(),version12.size()-1})
        require(!data.loadEncoded(std::span(version12).first(length)) && data.modulationPreparation() == savedModulationPreparation);
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&preparation,nullptr,&modulationPreparation).empty());
    require(data.loadEncoded(version11) && !data.modulationPreparation());
    sc55::PanTable pan{};
    for (unsigned i = 0; i < pan.size(); ++i) pan[i] = uint8_t(i*37);
    const auto version13 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&preparation,&modulationRates,&modulationPreparation,&pan);
    require(version13[6] == '1' && version13[7] == '3' && version13.size() == version12.size()+129);
    require(data.loadEncoded(version13) && data.pan() && *data.pan() == pan);
    const auto* savedPan = data.pan();
    for (size_t length : {size_t(8),version12.size(),version13.size()-1})
        require(!data.loadEncoded(std::span(version13).first(length)) && data.pan() == savedPan);
    auto invalid13 = version13; invalid13[7] = '4';
    require(!data.loadEncoded(invalid13) && data.pan() == savedPan);
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&preparation,&modulationRates,nullptr,&pan).empty());
    require(data.loadEncoded(version12) && !data.pan());
    sc55::PitchEnvelopeKeyCurves pitchTiming;
    for (unsigned c = 0; c < 16; ++c)
        for (unsigned k = 0; k < 256; ++k)
        { pitchTiming.attack[c][k] = uint8_t(c*31+k); pitchTiming.release[c][k] = uint8_t(c*17-k); }
    const auto version14 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&preparation,&modulationRates,&modulationPreparation,&pan,&pitchTiming);
    require(version14.size() == version13.size()+8192 && version14[7] == '4');
    require(data.loadEncoded(version14) && data.pitchTiming()
        && data.pitchTiming()->attack == pitchTiming.attack && data.pitchTiming()->release == pitchTiming.release);
    const auto* savedTiming = data.pitchTiming();
    for (size_t length : {size_t(8),version13.size(),version14.size()-1})
        require(!data.loadEncoded(std::span(version14).first(length)) && data.pitchTiming() == savedTiming);
    auto invalid14 = version14; invalid14[7] = '5';
    require(!data.loadEncoded(invalid14) && data.pitchTiming() == savedTiming);
    require(sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&preparation,&modulationRates,&modulationPreparation,nullptr,&pitchTiming).empty());
    std::vector<uint8_t> supplemental(patches.begin(),patches.begin()+sc55::SoundData::supplementalPatchBytes);
    supplemental[0] = 'Z';
    const auto version15 = sc55::SoundData::encode(patches,curves,samples,&syntheticTimes,&levels,&keyTables,&keyLevels,&glideRates,&pitchKeys,&pitchEnvelope,&secondEnvelope,&preparation,&modulationRates,&modulationPreparation,&pan,&pitchTiming,supplemental);
    require(version15.size() == version14.size()+supplemental.size() && version15[7] == '5');
    require(data.loadEncoded(version15) && data.patchCount() == 386 && data.patch(224)->name[0] == 'Z'
        && data.patch(385) && !data.patch(386) && !data.patch(0xffff));
    const auto* savedBank2 = data.patch(224);
    const size_t supplementalAt = version14.size()-samples.encode().size();
    auto bad15 = version15; bad15[supplementalAt] = 0;
    require(!data.loadEncoded(bad15) && data.patch(224) == savedBank2);
    bad15 = version15; bad15[supplementalAt+34] = 0x7f; bad15[supplementalAt+35] = 0xff;
    require(!data.loadEncoded(bad15) && data.patch(224) == savedBank2);
    require(!data.loadEncoded(std::span(version15).first(version15.size()-1)) && data.patch(224) == savedBank2);
    require(data.loadEncoded(version14) && data.patchCount() == 224 && !data.patch(224));
    require(data.loadEncoded(version13) && !data.pitchTiming());
    targetPartial.raw[0x45] = 100; targetPartial.raw[0x46] = 0; targetPartial.raw[0x47] = 84;
    require(sc55::PrepareEnvelopeKeyLevel(targetPartial,255,levels,keyLevels) == 182);
    require(sc55::PrepareEnvelopeKeyLevel(targetPartial,0,levels,keyLevels) == 1);
    targetPartial.raw[0x47] = 44;
    require(sc55::PrepareEnvelopeKeyLevel(targetPartial,0,levels,keyLevels) == 183);
    targetPartial.raw[0x47] = 64;
    require(sc55::PrepareEnvelopeKeyLevel(targetPartial,0,levels,keyLevels) == 55);
    targetPartial.raw[0x46] = 16;
    require(!sc55::PrepareEnvelopeKeyLevel(targetPartial,0,levels,keyLevels));
    require(data.loadEncoded(version4) && !data.keyLevels());
    require(data.loadEncoded(version2));
    if (argc == 2)
    {
        std::ifstream input(argv[1],std::ios::binary);
        require(bool(input));
        const std::vector<uint8_t> asset{std::istreambuf_iterator<char>(input),{}};
        require(data.loadEncoded(asset));
    }
    {
        unsigned checks = 0;
        for (unsigned program = 0; program < 128; ++program)
            for (unsigned velocity = 1; velocity < 128; ++velocity)
                for (bool soft : {false,true})
                {
                    sc55::ChannelControls controls;
                    sc55::MidiDecoder decoder;
                    const auto channel = uint8_t(program%16);
                    const uint8_t note = uint8_t((program+velocity)%128);
                    const uint8_t accumulator = uint8_t(program+velocity);
                    std::optional<sc55::MelodicNoteVelocity> actual;
                    sc55::MidiDecoder::Event noteEvent{};
                    const std::array<uint8_t,8> bytes{uint8_t(0xc0|channel),uint8_t(program),
                        uint8_t(0xb0|channel),67,uint8_t(soft ? 127 : 0),uint8_t(0x90|channel),note,uint8_t(velocity)};
                    for (const auto byte : bytes)
                        decoder.push(std::span(&byte,1),[&](const auto& event) {
                            if (controls.apply(event)) return;
                            noteEvent = event;
                            actual = sc55::PrepareMelodicNoteVelocity(event,controls.channel(channel),0,false,accumulator,data);
                        });
                    require(actual && actual->tone == sc55::v121CapitalToneIndices[program]
                        && actual->note == note && actual->velocity == velocity);
                    const auto& patch = *data.patch(actual->tone);
                    const auto expected = sc55::PreparePatchVelocity(patch.common[6],uint8_t(velocity),
                        patch.partial[0].raw,patch.partial[1].raw,accumulator,soft,*data.curves());
                    require(actual->partials.candidates.count == expected.candidates.count
                        && actual->partials.candidates.flags == expected.candidates.flags
                        && actual->partials.accumulator == expected.accumulator);
                    for (unsigned p = 0; p < 2; ++p)
                    {
                        require(bool(actual->partials.partials[p]) == bool(expected.partials[p]));
                        if (!expected.partials[p]) continue;
                        const auto& a = *actual->partials.partials[p]; const auto& e = *expected.partials[p];
                        require(a.accumulator == e.accumulator && a.amplitude == e.amplitude && a.secondary == e.secondary);
                    }
                    require(!sc55::PrepareMelodicNoteVelocity(noteEvent,controls.channel(channel),1,false,accumulator,data));
                    require(!sc55::PrepareMelodicNoteVelocity(noteEvent,controls.channel(channel),0,true,accumulator,data));
                    auto rejected = noteEvent; rejected.second = 0;
                    require(!sc55::PrepareMelodicNoteVelocity(rejected,controls.channel(channel),0,false,accumulator,data));
                    rejected = noteEvent; rejected.status = uint8_t(0x80|channel);
                    require(!sc55::PrepareMelodicNoteVelocity(rejected,controls.channel(channel),0,false,accumulator,data));
                    rejected = noteEvent; rejected.dataSize = 1;
                    require(!sc55::PrepareMelodicNoteVelocity(rejected,controls.channel(channel),0,false,accumulator,data));
                    rejected = noteEvent; rejected.first = 128;
                    require(!sc55::PrepareMelodicNoteVelocity(rejected,controls.channel(channel),0,false,accumulator,data));
                    const sc55::SoundData missing;
                    require(!sc55::PrepareMelodicNoteVelocity(noteEvent,controls.channel(channel),0,false,accumulator,missing));
                    ++checks;
                }
        require(checks == 32512);
        std::printf("Native melodic note velocity: %u program/velocity/soft-pedal cases through fragmented MIDI\n",checks);
    }
    if (data.pan())
    {
        sc55::EnvelopeRunner::Setup setup{};
        setup.targets = {100,100,100,100}; setup.keyScale = setup.releaseKeyScale = 256;
        setup.plan.velocityScale1 = setup.plan.velocityScale2 = 256;
        sc55::EnvelopeRunner::State state{{sc55::EnvelopeStage::sustain,{0,0},{0,0},100,100},25600,0xff00,65535};
        sc55::VoiceControlState voice{sc55::EnvelopeRunner(setup,state)};
        voice.release.second.stage = 10; voice.release.second.target = 1000;
        voice.pitch.envelope.stage = 10; voice.pitch.envelope.output = 60000;
        std::array<sc55::VoiceModulation,24> modulation{};
        std::array<uint8_t,24> sources{}; sources.fill(24);
        modulation[0].block.waveform = 3; modulation[0].block.rateIndex = 100;
        const sc55::ModulationBlock first;
        sc55::VoiceControlInputs input;
        input.level.expression = input.level.velocity = input.level.master = 100;
        input.pitch.masterTune = input.pitch.partTune = 1024;
        const sc55::PitchConversion conversion; const sc55::LfoWaveformTables waves;
        std::array<uint8_t,64> registers{}; unsigned io = 0;
        const auto read = [&](uint8_t)->uint8_t { ++io; return 0; };
        const auto write = [&](uint8_t a,uint8_t value) { ++io; registers[a] = value; };
        sc55::SoundData missing;
        require(sc55::AdvanceVoiceControl(0,voice,modulation,sources,first,input,missing,conversion,waves,read,write)
            == sc55::VoiceControlResult::invalidInput && io == 0);
        for (unsigned tick = 0; tick < 64; ++tick)
        {
            input.ticks = uint16_t(tick%5); input.level.expression = uint8_t(64+tick);
            require(sc55::AdvanceVoiceControl(0,voice,modulation,sources,first,input,data,conversion,waves,read,write)
                == sc55::VoiceControlResult::updated);
            require(voice.lifecycle.stages[0] == 10 && modulation[0].firstStage == 10);
            require(sc55::UpdateVoicePcm(0,voice.lifecycle,voice.prepared,voice.output,voice.second,write)
                == sc55::VoicePcmUpdateResult::written);
            require(uint16_t((registers[0x16]<<8)|registers[0x17]) == voice.output.tva.command);
        }
        require(voice.output.tva.ramp == 65535 && io != 0);
        state.segment.stage = sc55::EnvelopeStage::finished;
        voice.amplitude = sc55::EnvelopeRunner(setup,state); voice.release.pending = 1;
        const auto phase = modulation[0].block.wave.phase; const auto ramp = voice.output.tva.ramp;
        io = 0;
        require(sc55::AdvanceVoiceControl(0,voice,modulation,sources,first,input,data,conversion,waves,read,write)
            == sc55::VoiceControlResult::stopped);
        require(io == 0 && voice.release.pending == 1 && modulation[0].block.wave.phase == phase && voice.output.tva.ramp == ramp);
        state.segment.stage = sc55::EnvelopeStage::release; state.level = 0;
        voice.amplitude = sc55::EnvelopeRunner(setup,state); voice.release.pending = 0;
        sc55::PrepareVoiceAmplitude(voice.lifecycle,voice.amplitude);
        const auto secondStage = voice.release.second.stage;
        const auto pitchOutput = voice.pitch.envelope.output;
        input.ticks = 3;
        require(sc55::AdvanceVoiceControl(0,voice,modulation,sources,first,input,data,conversion,waves,read,write)
            == sc55::VoiceControlResult::finished);
        require(voice.lifecycle.stages[0] == 22 && modulation[0].firstStage == 22
            && voice.release.second.stage == secondStage && voice.pitch.envelope.output == pitchOutput && voice.output.tva.ramp == ramp);
        for (uint16_t stage : {uint16_t(14),uint16_t(16)})
        {
            voice.lifecycle.stages[0] = stage;
            io = 0;
            require(sc55::AdvanceVoiceControl(0,voice,modulation,sources,first,input,data,conversion,waves,read,write)
                == sc55::VoiceControlResult::stopped);
            require(voice.lifecycle.stages[0] == stage && io == 0);
        }
        io = 0;
        require(sc55::AdvanceVoiceControl(24,voice,modulation,sources,first,input,data,conversion,waves,read,write)
            == sc55::VoiceControlResult::invalidInput && io == 0);
        std::printf("Data-only serialized voice control: 64 updates through PCM readback, envelopes, pitch and output writes (synthetic PCM)\n");
    }
    if (const auto* preparation = data.modulationPreparation())
    {
        const sc55::LfoWaveformTables tables;
        unsigned checks = 0;
        for (unsigned tone = 0; tone < data.patchCount(); ++tone)
            for (unsigned p = 0; p < 2; ++p)
                for (unsigned mode = 0; mode < 256; ++mode)
                {
                    sc55::ModulationBlock first,second;
                    sc55::FirstModulationInputs input;
                    input.pitchDepth = sc55::PrepareModulationDepths(data.patch(tone)->partial[p],first,second,preparation->depths);
                    input.mode = uint8_t(mode); input.baseRate = uint8_t(mode*13);
                    input.delay = uint8_t(mode*19); input.attack = uint8_t(mode*31);
                    require(sc55::InitializeFirstModulation(first,input,preparation->timing,preparation->depths.pitch,*data.modulationRates(),tables,
                        [](uint8_t a)->uint8_t { return a == 0x3a ? 0x12 : 0x34; },[](uint8_t,uint8_t) {}));
                    require(first.waveform <= 6 && first.wave.held == 0x1234);
                    const auto secondDepth = second.depth;
                    sc55::InitializeSecondModulation(second,data.patch(tone)->partial[p],preparation->timing,*data.modulationRates(),tables,
                        [](uint8_t a)->uint8_t { return a == 0x3a ? 0x12 : 0x34; },[](uint8_t,uint8_t) {});
                    require(second.waveform <= 6 && second.wave.held == 0x1234 && second.depth == secondDepth
                        && second.rateIndex == data.patch(tone)->partial[p].raw[5]);
                    ++checks;
                }
        std::printf("Data-only modulation initialization: %u first-block variants; %u second-block patch/partial configurations repeated across variants (synthetic PCM inputs)\n",checks,data.patchCount()*2);
    }
    if (const auto* rates = data.modulationRates())
    {
        const sc55::LfoWaveformTables tables;
        unsigned checks = 0;
        unsigned levelChanges = 0;
        for (unsigned selector = 0; selector < 256; ++selector)
            for (unsigned waveform = 0; waveform < 7; ++waveform)
            {
                sc55::ModulationBlock block;
                block.rateIndex = uint8_t(selector); block.waveform = uint8_t(waveform);
                block.depth = {32767,0x8000,1234}; block.delayRate = 16384; block.attackRate = 8192;
                sc55::SecondEnvelopeOutputInputs envelopeInput;
                envelopeInput.base = 64;
                sc55::SecondEnvelopePcmState envelopeOutput;
                sc55::PitchModulationInputs pitchInput;
                pitchInput.masterTune = 1024; pitchInput.partTune = 1024;
                sc55::LevelInputs levelInput;
                levelInput.expression = 100; levelInput.velocity = 100; levelInput.master = 127;
                uint16_t previousLevel = sc55::ComputeLevel(levelInput);
                sc55::VoiceOutputState output;
                auto& tva = output.tva;
                sc55::SpatialInputs spatial;
                spatial.pan = uint8_t(selector%128); spatial.reverb = 100; spatial.chorus = 80;
                sc55::ModulationBlock otherBlock;
                for (unsigned tick = 0; tick < 32; ++tick)
                {
                    require(block.advance(uint16_t(tick%5),*rates,tables,
                        [](uint8_t a)->uint8_t { return a == 0x3a ? 0x12 : 0x34; },[](uint8_t,uint8_t) {}));
                    sc55::ApplyVoiceModulationOutputs(block,otherBlock,levelInput,envelopeInput,pitchInput);
                    const auto level = sc55::ComputeLevel(levelInput);
                    if (level != previousLevel) ++levelChanges;
                    previousLevel = level;
                    if (data.pan())
                    {
                        const auto previousPan = output.spatial.pan;
                        require(output.advance(2,levelInput,spatial,*data.pan()) == sc55::VoiceOutputState::Result::updated);
                        require(std::abs(int(output.spatial.pan)-int(previousPan)) <= 1);
                        if (output.spatial.pan != previousPan)
                            require(output.spatial.panWord == uint16_t((uint16_t((*data.pan())[128-output.spatial.pan])<<8)|(*data.pan())[output.spatial.pan]));
                    }
                    else require(tva.advance(2,levelInput));
                    require(envelopeOutput.advance(0,envelopeInput,64,64,127,8,*data.secondEnvelope()));
                    require(envelopeOutput.level <= 0xe600);
                    require(sc55::PrepareModulatedPitch(60000,pitchInput) <= 0xffffff);
                    ++checks;
                }
                require(block.delay == 65535 && block.attack == 65535 && block.output == block.depth);
                require(tva.ramp == 65535);
            }
        std::printf("Data-only modulation: %u updates without MCU/control ROM (synthetic PCM reads)\n",checks);
        require(levelChanges != 0);
        std::printf("Data-only modulation-to-level: %u level changes across %u updates\n",levelChanges,checks);
        std::printf("Data-only TVA: %u updates with native ramp/command state\n",checks);
        if (data.pan()) std::printf("Data-only full output control: %u TVA/pan/send updates using owned pan table\n",checks);
    }
    if (data.secondEnvelope())
    {
        sc55::SecondEnvelopePcmState output;
        sc55::SecondEnvelopeOutputInputs input;
        unsigned checks = 0;
        for (unsigned value = 0; value < 32768; ++value)
        {
            require(output.advance(uint16_t(value),input,64,64,127,uint16_t(value%9),*data.secondEnvelope()));
            require(output.level <= 0xe600 && output.control >= 8 && output.control <= 127);
            ++checks;
        }
        std::printf("Data-only second envelope output: %u updates\n",checks);
    }
    if (const auto* tables = data.secondPreparation())
    {
        unsigned checks = 0;
        for (unsigned tone = 0; tone < data.patchCount(); ++tone)
            for (const auto& partial : data.patch(tone)->partial)
                for (unsigned key = 0; key < 128; ++key)
                {
                    sc55::SecondEnvelopeSetup setup; sc55::SecondEnvelopeReleaseState segment; sc55::SecondEnvelopePcmState pcm;
                    require(setup.prepare(partial,uint8_t(key),100,segment,pcm,tables->keys,tables->targets,data.pitchEnvelope()->curve,
                        tables->timing,data.keys()->multipliers,*data.times(),*data.secondEnvelope()));
                    ++checks;
                }
        std::printf("Data-only full second envelope setup: %u cases\n",checks);
    }
    if (data.pitchKeys())
    {
        unsigned inputChecks = 0;
        unsigned pitchTargetPlans = 0;
        unsigned nativeStarts = 0;
        const sc55::PitchConversion startConversion;
        std::array<uint8_t,12> scale; scale.fill(64);
        for (unsigned tone = 0; tone < data.patchCount(); ++tone)
            for (unsigned partial = 0; partial < 2; ++partial)
                for (unsigned key = 0; key < 128; ++key)
                {
                    const auto& patch = *data.patch(tone);
                    const auto plan = sc55::PreparePartialSample(patch.partial[partial],*data.samples(),
                        uint8_t(key),uint8_t(key),uint8_t(key),scale,0,0,uint8_t(key));
                    if (!plan || (plan->sampleId & 0x8000)) continue;
                    const auto amplitude = sc55::PreparePartialAmplitude(patch,partial,*plan,*data.samples(),
                        uint8_t(key),100,sc55::EnvelopeStage::attack1,64,*data.levels(),*data.keyLevels(),*data.keys(),*data.times());
                    require(bool(amplitude));
                    const auto* descriptor = data.samples()->sample(plan->sampleId);
                    const auto expectedAmplitude = sc55::EnvelopeRunner::fromKey(patch.partial[partial],
                        {uint8_t(key),100,descriptor->data[0],patch.common[0],sc55::EnvelopeStage::attack1,64},
                        *data.levels(),*data.keyLevels(),*data.keys(),*data.times());
                    require(expectedAmplitude && amplitude->setup().targets == expectedAmplitude->setup().targets
                        && amplitude->state().level == expectedAmplitude->state().level);
                    auto invalidSample = *plan; invalidSample.sampleId = 0x8000;
                    require(!sc55::PreparePartialAmplitude(patch,partial,invalidSample,*data.samples(),0,100,
                        sc55::EnvelopeStage::attack1,64,*data.levels(),*data.keyLevels(),*data.keys(),*data.times()));
                    const auto input = sc55::PreparePartialPitchInputs(patch,partial,*plan,*data.samples(),uint8_t(key),1024,32,60);
                    require(bool(input));
                    if (data.pitchTiming())
                    {
                        sc55::PreparedPartPitch previous;
                        previous.values.pitch = 46000+tone;
                        previous.glide.increment = 1234;
                        previous.glide.pitch.accumulator = 60000;
                        previous.glide.pitch.correction = {17,23};
                        sc55::NormalPitchStartInputs start{uint8_t(key),uint8_t(127-key),uint8_t(key),1024,
                            uint8_t(((key&1) ? 32 : 0)|((key&2) ? 128 : 0)),
                            uint8_t((key&4) ? 255 : 60),uint8_t(key),0,{}};
                        start.modulation.partTune = 1024;
                        unsigned reads = 0, writes = 0;
                        const auto read = [&](uint8_t) -> uint8_t { ++reads; return uint8_t(tone); };
                        const auto write = [&](uint8_t,uint8_t) { ++writes; };
                        const auto prepared = sc55::PrepareNormalVoicePitch(patch,partial,*plan,start,previous,
                            data,startConversion,read,write);
                        require(bool(prepared));
                        const auto actualReads = reads, actualWrites = writes;
                        reads = writes = 0;
                        auto expected = previous;
                        auto expectedInput = *input; expectedInput.flags = start.flags; expectedInput.sourceKey = start.sourceKey;
                        require(expected.prepare(expectedInput,data.pitchKeys(),read,write));
                        require(reads == actualReads && writes == actualWrites);
                        const auto targets = sc55::PreparePitchEnvelopeTargets(expected.values.pitch,patch.partial[partial].raw,
                            start.velocity,expected.cachedRandom,data.pitchEnvelope()->depth,data.pitchEnvelope()->curve);
                        const auto timing = sc55::PreparePitchEnvelopeTiming(patch.partial[partial].raw,
                            start.envelopeKey,start.velocity,*data.pitchTiming(),data.keys()->multipliers,*data.times());
                        require(bool(timing));
                        sc55::VoicePitchRunner runner; runner.glide = expected.glide;
                        runner.installEnvelope(expected.values.pitch,targets,*timing);
                        require(runner.initialize(start.modulation,start.glideRate,*data.glideRates(),expected.values.reference,
                            start.correctionSource,startConversion) == sc55::VoicePitchRunner::Result::updated);
                        auto actual = prepared->runner;
                        require(prepared->part.values.pitch == expected.values.pitch
                            && prepared->part.values.reference == expected.values.reference
                            && prepared->part.values.alternateReference == expected.values.alternateReference
                            && prepared->part.cachedRandom == expected.cachedRandom
                            && prepared->part.glide.increment == expected.glide.increment);
                        for (unsigned tick = 0; tick < 16; ++tick)
                        {
                            require(actual.pcmWord == runner.pcmWord && actual.envelope.output == runner.envelope.output
                                && actual.glide.increment == runner.glide.increment
                                && actual.glide.pitch.accumulator == runner.glide.pitch.accumulator
                                && actual.glide.pitch.correction.offset == runner.glide.pitch.correction.offset);
                            require(actual.advance(1,false,start.modulation,start.glideRate,*data.glideRates(),expected.values.reference,0,startConversion)
                                == runner.advance(1,false,start.modulation,start.glideRate,*data.glideRates(),expected.values.reference,0,startConversion));
                        }
                        require(previous.values.pitch == 46000+tone && previous.glide.increment == 1234
                            && previous.glide.pitch.accumulator == 60000 && previous.glide.pitch.correction.offset == 23);
                        reads = writes = 0;
                        require(!sc55::PrepareNormalVoicePitch(patch,2,*plan,start,previous,data,startConversion,read,write));
                        require(!sc55::PrepareNormalVoicePitch(patch,partial,invalidSample,start,previous,data,startConversion,read,write));
                        start.glideRate = 128;
                        require(!sc55::PrepareNormalVoicePitch(patch,partial,*plan,start,previous,data,startConversion,read,write));
                        require(reads == 0 && writes == 0);
                        ++nativeStarts;
                    }
                    sc55::PreparedPartPitch state;
                    require(state.prepare(*input,data.pitchKeys(),[](uint8_t) -> uint8_t { return 0; },[](uint8_t,uint8_t) {}));
                    if (const auto* envelope = data.pitchEnvelope())
                    {
                        const auto targets = sc55::PreparePitchEnvelopeTargets(state.values.pitch,patch.partial[partial].raw,
                            uint8_t(key),state.cachedRandom,envelope->depth,envelope->curve);
                        require(targets.direction == (targets.pitch[0] < targets.pitch[1] ? 0 : 2));
                        for (auto target : targets.pitch) require(target <= 0xffffff);
                        state.glide.pitch.accumulator = targets.pitch[0];
                        // Old formats expose missing timing explicitly; MD14
                        // uses its owned pitch-specific key curves.
                        uint16_t pitchKeyScale = 256;
                        if (data.pitchTiming())
                        {
                            const auto timing = sc55::PreparePitchEnvelopeTiming(patch.partial[partial].raw,
                                uint8_t(key),uint8_t(key),*data.pitchTiming(),data.keys()->multipliers,*data.times());
                            require(bool(timing)); pitchKeyScale = timing->keyScales[0];
                        }
                        const auto increment = sc55::PreparePitchEnvelopeFirstIncrement(patch.partial[partial].raw,
                            uint8_t(key),pitchKeyScale,*data.times());
                        require(increment.has_value() && *increment >= 8);
                        sc55::PitchEnvelopeSegment segment{{0,0},targets.pitch[0],targets.pitch[1],*increment,targets.direction};
                        for (unsigned tick = 0; tick < 16; ++tick)
                        {
                            state.glide.pitch.accumulator = segment.advance(1);
                            require(state.glide.pitch.accumulator <= 0xffffff);
                        }
                        ++pitchTargetPlans;
                    }
                    require(!sc55::PreparePartialPitchInputs(patch,2,*plan,*data.samples(),0,0,0,0));
                    auto special = *plan; special.sampleId = 0x8000;
                    require(!sc55::PreparePartialPitchInputs(patch,partial,special,*data.samples(),0,0,0,0));
                    ++inputChecks;
                }
        require(inputChecks != 0);
        std::printf("Owned sample selection to pitch preparation: %u cases\n",inputChecks);
        if (data.pitchTiming())
        {
            require(nativeStarts == inputChecks);
            std::printf("Native normal pitch starts: %u with previous-state preservation and rejection before PCM I/O\n",nativeStarts);
        }
        if (data.pitchEnvelope())
        {
            require(pitchTargetPlans == inputChecks);
            std::printf("Owned pitch envelope target plans: %u without control ROM\n",pitchTargetPlans);
        }
        for (unsigned selector = 0; selector < 40; ++selector)
            for (unsigned key = 0; key < 256; ++key)
                require(sc55::ApplyPartPitchKeyTable(60000,uint8_t(selector),uint8_t(key),*data.pitchKeys()).has_value());
        std::puts("Owned pitch key tables: 10240 native lookups without control ROM");
    }
    if (data.glideRates())
    {
        const sc55::PitchConversion conversion;
        for (unsigned index = 0; index < 128; ++index)
        {
            sc55::PitchGlide glide{{81000,{128,0}},0};
            glide.prepare(32,120,60000,60000);
            require(glide.increment == 60000);
            for (unsigned tick = 0; tick < 16; ++tick)
                require(glide.advance(1,uint8_t(index),*data.glideRates(),81000,128,conversion).has_value());
        }
        std::puts("Owned glide rates: 128 rates, 2048 native updates without control ROM");
    }
    // Exercise native velocity preparation without linking an emulator or
    // opening either control ROM. Functional equality is checked by the oracle.
    size_t plans = 0, accepted = 0, targetPlans = 0, runners = 0;
    std::array<uint8_t,128> rhythmAccumulators;
    for (unsigned key = 0; key < 128; ++key) rhythmAccumulators[key] = uint8_t(key*37);
    unsigned rhythmPlans = 0, rhythmInstalled = 0, rhythmUnsupported = 0;
    if (data.levels())
        for (unsigned tone = 0; tone < data.patchCount(); ++tone)
            for (const auto& partial : data.patch(tone)->partial)
                if (partial.used)
                    for (unsigned base = 0; base < 256; ++base)
                    {
                        require(bool(sc55::PrepareEnvelopeTargets(partial,uint8_t(base),*data.levels())));
                        // Synthetic allocation inputs, actual owned patch/tables.
                        // This tests composition, not real note/key allocation.
                        std::array<uint16_t,2> scales{256,256};
                        if (data.keys())
                        {
                            const auto prepared = sc55::PrepareEnvelopeKeyScales(partial,uint8_t(base),*data.keys());
                            require(bool(prepared)); scales = *prepared;
                        }
                        auto runner = sc55::EnvelopeRunner::fromPartial(partial,
                            {uint8_t(base),100,127,data.patch(tone)->common[0],scales[0],scales[1],sc55::EnvelopeStage::attack1,64},
                            *data.levels(),*data.times());
                        if (data.keyLevels())
                            runner = sc55::EnvelopeRunner::fromKey(partial,
                                {uint8_t(base),100,127,data.patch(tone)->common[0],sc55::EnvelopeStage::attack1,64},
                                *data.levels(),*data.keyLevels(),*data.keys(),*data.times());
                        require(bool(runner));
                        runner->activateNewVoice();
                        for (unsigned tick = 0; tick < 8; ++tick) require(runner->tick(1,{},*data.times()));
                        runner->release(runner->state().level);
                        for (unsigned tick = 0; tick < 32 && runner->state().segment.stage != sc55::EnvelopeStage::finished; ++tick)
                            require(runner->tick(65535,{},*data.times()));
                        require(runner->state().segment.stage == sc55::EnvelopeStage::finished);
                        ++runners;
                        ++targetPlans;
                    }
    std::printf("Data-only envelope targets: %zu plans\n",targetPlans);
    std::printf("Data-only composed envelopes: %zu completed\n",runners);
    for (unsigned tone = 0; tone < data.patchCount(); ++tone)
    {
        const auto& patch = *data.patch(tone);
        for (unsigned velocity = 0; velocity < 128; ++velocity)
            for (bool soft : {false,true})
            {
                const auto plan = sc55::PreparePatchVelocity(patch.common[6],uint8_t(velocity),
                    patch.partial[0].raw,patch.partial[1].raw,0,soft,*data.curves());
                if (velocity != 0)
                {
                    const uint8_t key = uint8_t(tone%128);
                    sc55::RhythmKeyMap map; map.tones[key] = uint16_t(tone);
                    map.flags[key] = 0x90; map.groups[key] = 7;
                    map.pitches[key] = uint8_t((key+17)%128);
                    const sc55::MidiDecoder::Event event{sc55::MidiDecoder::Kind::message,0x99,key,uint8_t(velocity),2};
                    for (unsigned program : {0u,127u})
                    {
                        const auto result = sc55::PrepareRhythmNoteVelocity(event,program,map,rhythmAccumulators,0,soft,data);
                        require(result && result->note && result->note->tone == tone && result->note->note == key
                            && result->note->velocity == velocity && result->mapping.flags == 0x90 && result->mapping.group == 7);
                        const auto expected = sc55::PreparePatchVelocity(patch.common[6],uint8_t(velocity),
                            patch.partial[0].raw,patch.partial[1].raw,program == 127 ? rhythmAccumulators[key] : 0,soft,*data.curves());
                        const auto& actual = result->note->partials;
                        require(actual.candidates.flags == expected.candidates.flags && actual.candidates.count == expected.candidates.count
                            && actual.accumulator == expected.accumulator);
                        for (unsigned p = 0; p < 2; ++p)
                        {
                            require(bool(actual.partials[p]) == bool(expected.partials[p]));
                            if (actual.partials[p]) require(actual.partials[p]->amplitude == expected.partials[p]->amplitude
                                && actual.partials[p]->secondary == expected.partials[p]->secondary);
                        }
                        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
                        std::array<sc55::VoiceStopState,24> lifecycle{};
                        std::array<uint8_t,16> retained; retained.fill(255);
                        sc55::RhythmNoteAdmission admission(*result,9,0x10,retained,{});
                        const auto noRead = [](uint8_t)->uint8_t { throw std::runtime_error("Unexpected admission PCM read"); };
                        const auto noWrite = [](uint8_t,uint8_t) { throw std::runtime_error("Unexpected admission PCM write"); };
                        using Status = sc55::RhythmNoteAdmission::Status;
                        const auto status = admission.run(data,allocator,lifecycle,noRead,noWrite);
                        require(status == (actual.candidates.count ? Status::allocated : Status::velocityRejected));
                        if (actual.candidates.count)
                        {
                            const auto& allocated = admission.allocation();
                            require(allocated && allocated->selection && allocated->selection->tone == tone && allocated->group);
                            const auto group = allocated->group->group;
                            require(allocator.groupFieldA2D0[group] == 7 && allocator.groupFieldA300[group] == 0x90
                                && allocator.groupValue[group] == key && allocator.freeCount == 24-actual.candidates.count);
                            const auto dispatch = sc55::PlanPartialVoiceDispatch(patch,actual.candidates.flags,
                                {allocated->group->voices[0],allocated->group->voices[1]});
                            require(bool(dispatch));
                            for (unsigned p = 0; p < 2; ++p)
                                require(allocated->dispatch[p].prepare == (*dispatch)[p].prepare
                                    && allocated->dispatch[p].voice == (*dispatch)[p].voice);
                        }
                        else require(!admission.allocation() && allocator.freeCount == 24);
                        const auto saved = allocator;
                        require(admission.run(data,allocator,lifecycle,noRead,noWrite) == status
                            && std::memcmp(&saved,&allocator,sizeof allocator) == 0);
                        if (admission.allocation() && velocity == 100 && !soft && program == 0)
                        {
                            const auto& allocated = *admission.allocation();
                            std::array<uint8_t,12> scale; scale.fill(64); scale[key%12] = 65;
                            const auto inputs = sc55::PrepareRhythmSampleInputs(allocated,map,64,0,scale,{0,0},{128,128});
                            require(bool(inputs));
                            require((*inputs)[0].originalNote == key && (*inputs)[1].originalNote
                                == (allocated.dispatch[0].prepare ? map.pitches[key] : key));
                            require((*inputs)[0].minimumKey == 255 && (*inputs)[1].minimumKey == 255);
                            sc55::VoiceInstallationState installed;
                            unsigned io = 0;
                            const auto samples = sc55::PrepareAndInstallMelodicSamples(allocated,9,*inputs,data,
                                allocator,installed,lifecycle,[&](uint8_t) { ++io; return uint8_t(0); },
                                [&](uint8_t,uint8_t) { ++io; });
                            if (!samples)
                            {
                                ++rhythmUnsupported;
                                require(io == 0 && std::memcmp(&saved,&allocator,sizeof allocator) == 0);
                                if (argc != 2)
                                    require(patch.partial[0].raw[13] == 0
                                        && !sc55::TrackPartialKey(60,60,patch.partial[0].raw[13]));
                            }
                            else
                            {
                                ++rhythmInstalled;
                                for (unsigned p = 0; p < 2; ++p)
                                    if ((*samples)[p] && (*samples)[p]->installed)
                                    {
                                        const auto& input = (*samples)[p]->installed->input;
                                        require(input.tone == tone && input.originalKey == map.pitches[key]
                                            && input.part == 9 && input.partial == p && input.restarted);
                                    }
                            }
                        }
                        ++rhythmPlans;
                    }
                }
                for (const auto& partial : plan.partials) if (partial) ++accepted;
                ++plans;
            }
    }
    std::printf("Data-only native preparation: %zu plans, %zu accepted partials\n",plans,accepted);
    {
        sc55::SoundData missing;
        sc55::RhythmKeyMap map;
        sc55::MidiDecoder::Event event{sc55::MidiDecoder::Kind::message,0x99,60,100,2};
        const auto absent = sc55::PrepareRhythmNoteVelocity(event,0,map,rhythmAccumulators,0,false,missing);
        require(absent && !absent->note && !absent->mapping.present());
        sc55::VoiceAllocator allocator; require(allocator.initializeTables());
        std::array<sc55::VoiceStopState,24> lifecycle{};
        std::array<uint8_t,16> retained; retained.fill(255);
        const auto checkAdmission = [&](sc55::RhythmNoteVelocity prepared,unsigned part,
            const sc55::SoundData& asset,sc55::RhythmNoteAdmission::Status expected) {
            sc55::RhythmNoteAdmission admission(prepared,part,0x10,retained,{});
            require(admission.run(asset,allocator,lifecycle,
                [](uint8_t)->uint8_t { throw std::runtime_error("Rejected rhythm read"); },
                [](uint8_t,uint8_t) { throw std::runtime_error("Rejected rhythm write"); }) == expected);
            require(!admission.allocation() && allocator.freeCount == 24);
        };
        using Admission = sc55::RhythmNoteAdmission::Status;
        checkAdmission(*absent,9,missing,Admission::absent);
        checkAdmission(*absent,16,data,Admission::invalidInput);
        map.tones[60] = 0;
        const auto present = sc55::PrepareRhythmNoteVelocity(event,0,map,rhythmAccumulators,0,false,data);
        require(present && present->note);
        checkAdmission(*present,9,missing,Admission::invalidInput);
        auto inconsistent = *present; ++inconsistent.note->tone;
        checkAdmission(inconsistent,9,data,Admission::invalidInput);
        require(!sc55::PrepareRhythmNoteVelocity(event,0,map,rhythmAccumulators,0,false,missing));
        map.tones[60] = 386;
        require(!sc55::PrepareRhythmNoteVelocity(event,0,map,rhythmAccumulators,0,false,data));
        map.tones[60] = 0; event.second = 0;
        require(!sc55::PrepareRhythmNoteVelocity(event,0,map,rhythmAccumulators,0,false,data));
    }
    std::printf("Data-only mapped rhythm preparation: %u plans with retained global tone IDs\n",rhythmPlans);
    std::printf("Data-only rhythm sample installation: %u completed, %u unsupported plans rejected before I/O (synthetic PCM)\n",
        rhythmInstalled,rhythmUnsupported);
    // The zero-filled parser fixture intentionally has unsupported tracking0.
    // Imported v1.21 assets, in contrast, must install every patch in this sweep.
    if (argc == 2) require(rhythmInstalled == data.patchCount() && rhythmUnsupported == 0);
    else require(rhythmInstalled == 0 && rhythmUnsupported == data.patchCount());
}
