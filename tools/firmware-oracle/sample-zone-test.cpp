#include "sc55_voice_setup.h"
#include "sc55_sample_bank.h"
#include <array>
#include <stdexcept>
#include <fstream>
#include <iterator>
#include <cstdio>

int main(int argc,char** argv)
{
    const auto require = [](bool ok) { if (!ok) throw std::runtime_error("sample zone regression"); };
    const auto sampleControl = sc55::DecodeSampleControl(0x2fbdfb,0,1,1);
    require(sampleControl.mode == 0x1201 && sampleControl.loopFlag == 0);
    const auto allControlFlags = sc55::DecodeSampleControl(0xffffff,255,23,255);
    require(allControlFlags.mode == 0xff57 && allControlFlags.loopFlag == 2);
    const auto bankBoundary = sc55::DecodeSampleControl(0x0fffff,2,0,0);
    require(bankBoundary.mode == 0 && bankBoundary.loopFlag == 2);
    require(sc55::DecodeSampleControl(0x100000,0,0,0).mode == 0x100);
    sc55::VelocityCurves curves{};
    std::array<uint8_t,92> firstVelocity{},secondVelocity{};
    firstVelocity[0x43]=secondVelocity[0x43]=127;
    firstVelocity[0x42]=20; secondVelocity[0x42]=30;
    auto plan = sc55::PreparePatchVelocity(0x83,64,firstVelocity,secondVelocity,5,false,curves);
    require(plan.candidates.flags == 0x83 && plan.candidates.count == 2 && plan.accumulator == 55);
    require(plan.partials[0]->amplitude == 25 && plan.partials[1]->amplitude == 55);
    firstVelocity[0x43]=63;
    plan = sc55::PreparePatchVelocity(0x83,64,firstVelocity,secondVelocity,5,false,curves);
    require(!plan.partials[0] && plan.partials[1]->accumulator == 35 && plan.candidates.flags == 0x82);
    plan = sc55::PreparePatchVelocity(0x80,64,firstVelocity,secondVelocity,5,false,curves);
    require(plan.accumulator == 5 && plan.candidates.count == 0 && !plan.partials[0] && !plan.partials[1]);
    for (unsigned i = 0; i < 256; ++i) curves[0][i]=uint8_t(i);
    std::array<uint8_t,92> velocityPartial{};
    velocityPartial[0x43]=127; velocityPartial[0x44]=127;
    const auto velocityNormal = sc55::EvaluatePartialVelocity(127,velocityPartial,0,false,curves);
    const auto velocityReduced = sc55::EvaluatePartialVelocity(127,velocityPartial,0,true,curves);
    require(velocityNormal && velocityNormal->amplitude == 127 && velocityNormal->secondary == 127);
    require(velocityReduced && velocityReduced->secondary == 91);
    require(sc55::InterpolateVelocityAmplitude(0,0,0,0).second == 1);
    require(sc55::PartialVelocityCurveIndex(0,0,127) == 0);
    require(sc55::PartialVelocityCurveIndex(127,0,127) == 127);
    require(sc55::PartialVelocityCurveIndex(127,127,0) == 0);
    require(sc55::PartialVelocityCurveIndex(0,127,0) == 127);
    require(!sc55::PartialVelocityCurveIndex(60,61,100));
    require(sc55::PartialVelocityCurveIndex(60,60,60) == 0);
    std::array<uint8_t,92> lower{},upper{};
    lower[0x43]=63; upper[0x41]=64; upper[0x43]=127;
    const auto low = sc55::SelectPartialCandidates(0x83,63,lower,upper);
    const auto high = sc55::SelectPartialCandidates(0x83,64,lower,upper);
    require(low.flags == 0x81 && low.count == 1 && high.flags == 0x82 && high.count == 1);
    sc55::SampleBank bank;
    sc55::SampleBank::Group group{7,{}};
    group.data[12]=127;
    group.data[29]=3;
    std::vector<sc55::SampleBank::Sample> samples{{3,{}}};
    samples[0].data[2]=42;
    require(bank.load({group},samples));
    const auto encoded = bank.encode();
    sc55::SampleBank restored;
    require(restored.loadEncoded(encoded) && restored.encode() == encoded);
    for (size_t size = 0; size < encoded.size(); ++size)
        require(!restored.loadEncoded(std::span(encoded).first(size)));
    auto invalid = encoded;
    invalid.push_back(0);
    require(!restored.loadEncoded(invalid));
    invalid = encoded; invalid[7] = '2';
    require(!restored.loadEncoded(invalid));
    invalid = encoded; invalid[8] = 0xff; invalid[9] = 0xff;
    require(!restored.loadEncoded(invalid));
    invalid = encoded; invalid[75] = 4; // descriptor ID no longer satisfies group reference
    require(!restored.loadEncoded(invalid));
    require(restored.sample(3)->data[2] == 42);
    require(argc >= 1 && argc <= 3);
    if (argc >= 2)
    {
        // This executable links no emulator, ROM loader, or ROM data.
        std::ifstream input(argv[1],std::ios::binary);
        require(bool(input));
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
        sc55::SampleBank imported;
        require(imported.loadEncoded(bytes) && imported.encode() == bytes);
        std::printf("Data-only file loaded: %zu groups, %zu descriptors\n",imported.groupCount(),imported.sampleCount());
        if (argc == 3)
        {
            std::ifstream patchesInput(argv[2],std::ios::binary);
            require(bool(patchesInput));
            const std::vector<uint8_t> patches((std::istreambuf_iterator<char>(patchesInput)),{});
            require(patches.size() == 162*216);
            unsigned checks = 0;
            for (unsigned tone = 0; tone < 162; ++tone)
                for (unsigned partial = 0; partial < 2; ++partial)
                {
                    const unsigned at = tone*216+32+partial*92;
                    const auto group = uint16_t((patches[at+2]<<8)|patches[at+3]);
                    if (group == 0xffff) continue;
                    for (unsigned key = 0; key < 128; ++key)
                    {
                        const auto sample = imported.select(group,uint8_t(key));
                        require(bool(sample));
                        if (!(*sample&0x8000)) require(imported.sample(*sample) != nullptr);
                        ++checks;
                    }
                }
            require(checks != 0);
            std::printf("Supplemental patch closure: %u partial/key queries resolved without control ROM\n",checks);
        }
    }
    samples[0].data[2]=0;
    require(bank.sample(3)->data[2] == 42 && bank.select(7,127) == 3);
    require(!bank.select(8,60) && !bank.sample(4));
    require(!bank.load({group,group},samples));
    require(!bank.load({group},{}));
    require(bank.sample(3)->data[2] == 42); // failed load preserves old data
    group.data[28]=group.data[29]=0xff;
    require(bank.load({group},{}) && bank.select(7,60) == 0xffff);
    group.data[12]=0;
    require(!bank.load({group},{})); // uncovered keys
    std::array<uint8_t,20> common{};
    common[8]=0xff; common[9]=0xff; common[14]=60;
    common[10]=0; common[11]=23; common[15]=61;
    common[12]=1; common[13]=2; common[16]=62;
    require(!sc55::MapHighNote(124,common) && !sc55::MapHighNote(128,common));
    require(sc55::MapHighNote(125,common)->tone == 0xffff);
    require(sc55::MapHighNote(126,common)->tone == 23 && sc55::MapHighNote(126,common)->note == 61);
    require(sc55::MapHighNote(127,common)->tone == 258 && sc55::MapHighNote(127,common)->note == 62);
    require(sc55::TransposePartKey(60,64,0) == 60);
    require(sc55::TransposePartKey(60,76,255) == 71);
    require(sc55::TransposePartKey(0,0,0) == 0);
    require(sc55::TransposePartKey(127,127,0) == 127);
    require(sc55::TransposeMasterKey(255,0) == 255);
    require(sc55::TransposeMasterKey(255,64) == 127);
    require(sc55::TransposeMasterKey(60,52) == 48);
    const auto untouched = sc55::ResolvePartialKey(20,60,0x81,70,64);
    require(untouched.lookupKey == 60 && !untouched.storedOriginalNote && !untouched.storedAdjustedKey);
    const auto stored = sc55::ResolvePartialKey(20,60,0x80,70,64);
    require(stored.lookupKey == 60 && !stored.storedOriginalNote && stored.storedAdjustedKey == 20);
    const auto remapped = sc55::ResolvePartialKey(20,60,0,70,64);
    require(remapped.lookupKey == 60 && remapped.storedOriginalNote == 70 && remapped.storedAdjustedKey == 20);
    // The caller reloads the minimum; its sign controls whether to compare.
    require(sc55::ResolvePartialKey(128,1,0x81,70,64).lookupKey == 127);
    require(sc55::ResolvePartialKey(255,1,0x81,70,64).lookupKey == 1);
    require(sc55::ResolvePartialKey(255,1,0x80,70,64).lookupKey == 1);
    std::array<uint8_t,92> partial{};
    std::array<uint8_t,12> scale{};
    partial[10] = 76;
    partial[13] = 74;
    scale.fill(64);
    scale[0] = 0;
    scale[1] = 127;
    const auto prepared = sc55::PreparePartialPitch(60,60,1,partial,scale);
    require(prepared && prepared->key == 72 && prepared->fraction == 630);
    const auto otherClass = sc55::PreparePartialPitch(60,60,0,partial,scale);
    require(otherClass && otherClass->key == 71 && otherClass->fraction == 360);
    partial[13] = 85;
    require(!sc55::PreparePartialPitch(60,60,1,partial,scale));
    const auto borrow = sc55::ApplyScaleTuning({0,0},0,74);
    require(borrow && borrow->key == 255 && borrow->fraction == 360);
    const auto carry = sc55::ApplyScaleTuning({255,999},127,74);
    require(carry && carry->key == 0 && carry->fraction == 629);
    const auto unchanged = sc55::ApplyScaleTuning({60,1000},64,74);
    require(unchanged && unchanged->key == 60 && unchanged->fraction == 1000);
    const auto disabled = sc55::ApplyScaleTuning({60,1000},0,0);
    require(disabled && disabled->key == 60 && disabled->fraction == 1000);
    require(!sc55::ApplyScaleTuning({60,1001},64,74));
    require(!sc55::ApplyScaleTuning({60,0},128,74));
    require(!sc55::ApplyScaleTuning({60,0},0,85));
    require(!sc55::TrackPartialKey(128,60,74));
    require(!sc55::TrackPartialKey(60,128,74));
    require(!sc55::TrackPartialKey(60,60,43));
    require(!sc55::TrackPartialKey(60,60,85));
    for (unsigned key = 0; key < 128; ++key)
    {
        const auto centre = sc55::TrackPartialKey(uint8_t(key),60,74);
        require(centre && centre->key == key && centre->fraction == 0);
    }
    const auto rounded = sc55::TrackPartialKey(0,0,69);
    require(rounded && rounded->key == 30 && rounded->fraction == 0);
    const auto fractional = sc55::TrackPartialKey(0,1,45);
    require(fractional && fractional->key == 127 && fractional->fraction == 100);
    for (unsigned key = 0; key < 128; ++key)
        for (unsigned param = 0; param < 128; ++param)
        {
            const auto clamp = [](int v) { return v < 0 ? 0 : (v > 127 ? 127 : v); };
            require(sc55::TransposePartialKey(uint8_t(key),uint8_t(param)) == clamp(int(key+param)-64));
            require(sc55::MakeSampleLookupKey(uint8_t(key),uint8_t(param)) == clamp(int(key)+64-int(param)));
        }
    require(sc55::V121MultisampleOffset(0) == 0x1bd00);
    require(sc55::V121MultisampleOffset(143) == 0x1de84);
    require(sc55::V121MultisampleOffset(144) == 0x2bd00);
    require(sc55::V121SampleDescriptorOffset(0) == 0x1dec0);
    require(sc55::V121SampleDescriptorOffset(531) == 0x1fff0);
    require(sc55::V121SampleDescriptorOffset(532) == 0x2dec0);
    require(sc55::V121SampleDescriptorOffset(1064) == 0x20000);
    require(!sc55::V121SampleDescriptorOffset(0x8000));
    require(!sc55::V121SampleDescriptorOffset(0xffff));
    std::array<uint8_t,60> record{};
    for (unsigned i = 0; i < 16; ++i)
    {
        record[12+i] = uint8_t(i*8+7);
        record[28+2*i] = 1;
        record[29+2*i] = uint8_t(i);
    }
    for (unsigned key = 0; key < 128; ++key)
        require(sc55::SelectSampleZone(record,uint8_t(key)) == 256+key/8);
    record[12] = 0;
    require(sc55::SelectSampleZone(record,0) == 256);
    require(sc55::SelectSampleZone(record,1) == 257);
    record[28] = record[29] = 0xff;
    require(sc55::SelectSampleZone(record,0) == 0xffff);
    require(!sc55::SelectSampleZone(record,128));

    // Exercise both carry and borrow in descriptor address calculation.
    const std::array<uint8_t,10> descriptor {0,0xff,0xff,0xfe,0,4,0,3,0,2};
    const auto offset = sc55::DecodeSampleAddresses(descriptor,false);
    require(offset.start == 2 && offset.end == 1 && offset.loop == 0xffffff);
    require(sc55::DecodeSampleAddresses(descriptor,true).start == 0xfffffe);
}
