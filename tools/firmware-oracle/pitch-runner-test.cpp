#include "sc55_pitch.h"
#include "sc55_voice_lifecycle.h"
#include <stdexcept>

int main()
{
    using Runner = sc55::PitchEnvelopeRunner;
    const auto require = [](bool ok) {
        if (!ok) throw std::runtime_error("pitch runner regression");
    };
    // The second envelope deliberately differs from the pitch path when both
    // depths are negative: only the first magnitude is used by the firmware.
    require(sc55::ApplySecondEnvelopeModulation(10000,64536,63536,16384) == 9500);
    require(sc55::ApplySecondEnvelopeModulation(10000,64536,63536,49152) == 10500);
    require(sc55::ApplySecondEnvelopeModulation(10000,6000,6000,16384) == 13072);
    require(sc55::ApplySecondEnvelopeModulation(32760,1000,0,16384) == 32767);
    require(sc55::ApplySecondEnvelopeModulation(1,1000,0,49152) == 0);
    require(sc55::ApplySecondEnvelopeModulation(10000,1000,0,32768) == 10000);
    {
        sc55::SecondEnvelopeOutputInputs input;
        input.base = 64;
        require(sc55::PrepareSecondEnvelopeOutput(65535,input) == 16383);
        input.control = 127;
        require(sc55::PrepareSecondEnvelopeOutput(0,input) == 20480);
        input.suppressPositiveControl = true;
        require(sc55::PrepareSecondEnvelopeOutput(0,input) == 16384);
        input.control = 0;
        require(sc55::PrepareSecondEnvelopeOutput(65535,input) == 0);
        input.control = 64; input.offset = 32767;
        require(sc55::PrepareSecondEnvelopeOutput(0,input) == 32767);
        input.offset = 32768;
        require(sc55::PrepareSecondEnvelopeOutput(0,input) == 0);
        input.base = 128;
        require(!sc55::PrepareSecondEnvelopeOutput(0,input));
        input.base = 0; input.control = 128;
        require(!sc55::PrepareSecondEnvelopeOutput(0,input));
    }
    {
        sc55::SecondEnvelopePcmTables tables;
        tables.smoothingCeiling.fill(127); tables.outputCeiling.fill(255);
        for (unsigned i = 0; i < tables.levelCurve.size(); ++i)
            tables.levelCurve[i] = uint16_t(i*128);
        require(sc55::AdvanceSecondEnvelopeControl(20,64,64,127,0,tables) == 21);
        require(sc55::AdvanceSecondEnvelopeControl(20,64,127,127,0,tables) == 19);
        tables.smoothingCeiling[1] = 10;
        require(sc55::AdvanceSecondEnvelopeControl(20,64,64,127,1,tables) == 10);
        require(sc55::AdvanceSecondEnvelopeControl(20,20,64,127,1,tables) == 20);
        tables.smoothingCeiling[255] = 9;
        require(sc55::AdvanceSecondEnvelopeControl(20,64,64,127,65535,tables) == 9);
        require(!sc55::AdvanceSecondEnvelopeControl(128,64,64,127,0,tables));
        uint8_t control = 0;
        require(sc55::ConvertSecondEnvelopePcmLevel(257,control,tables) == 256);
        require(control == 8);
        tables.outputCeiling[8] = 1;
        require(sc55::ConvertSecondEnvelopePcmLevel(32767,control,tables) == 256);
        tables.outputCeiling[8] = 255; tables.levelCurve.fill(32767);
        require(sc55::ConvertSecondEnvelopePcmLevel(0,control,tables) == 0xe600);
        tables.levelCurve.fill(32768);
        require(sc55::ConvertSecondEnvelopePcmLevel(0,control,tables) == 0);
        control = 0;
        require(!sc55::ConvertSecondEnvelopePcmLevel(32768,control,tables));
        require(control == 0);
        control = 128;
        require(!sc55::ConvertSecondEnvelopePcmLevel(0,control,tables));
        require(control == 128);
        tables.outputCeiling.fill(255);
        require(sc55::EncodeSecondEnvelopePcmCommand(1000,1000,8,0,tables) == 0xff00);
        require(sc55::EncodeSecondEnvelopePcmCommand(0x1000,0x1001,8,0,tables) == 0x11af);
        tables.outputCeiling[8] = 0x10;
        require(sc55::EncodeSecondEnvelopePcmCommand(0x1000,0x1001,8,0,tables) == 0x10af);
        require(sc55::EncodeSecondEnvelopePcmCommand(0x1002,0x1001,8,0,tables) == 0x10af);
        tables.outputCeiling[8] = 255;
        require(sc55::EncodeSecondEnvelopePcmCommand(0xe500,0xe501,8,0,tables) == 0xe6af);
        require(sc55::EncodeSecondEnvelopePcmCommand(0,0xe600,8,8,tables)
            == sc55::EncodeCutoff(0xe600,0xe600,7,false));
        require(!sc55::EncodeSecondEnvelopePcmCommand(0,0,7,0,tables));
        require(!sc55::EncodeSecondEnvelopePcmCommand(0,0,128,0,tables));
        require(!sc55::EncodeSecondEnvelopePcmCommand(0,0,8,9,tables));
        require(!sc55::EncodeSecondEnvelopePcmCommand(0,0xe601,8,0,tables));
        sc55::SecondEnvelopePcmState state{8,12,34,56};
        sc55::SecondEnvelopeOutputInputs input;
        require(!state.advance(0,input,64,64,127,9,tables));
        require(state.control == 8 && state.output == 12 && state.level == 34 && state.command == 56);
        tables.levelCurve.fill(0);
        require(state.advance(0,input,64,64,127,0,tables));
        require(state.control == 9 && state.output == 0 && state.level == 0 && state.command == 0x00af);
        require(state.advance(0,input,64,64,127,0,tables));
        require(state.command == 0xff00);
    }
    require(sc55::ApplyPitchModulation(60000,1000,2000,16384) == 61500);
    require(sc55::ApplyPitchModulation(60000,1000,2000,49152) == 58500);
    require(sc55::ApplyPitchModulation(1,1000,2000,49152) == 0);
    require(sc55::ApplyPitchModulation(60000,6000,6000,16384) == 63000);
    require(sc55::ApplyPitchModulation(60000,32768,32768,16384) == 60000);
    require(sc55::ApplyPitchModulation(60000,1000,0,32768) == 60000);
    require(sc55::ApplyPitchModulation(0x800000,65535,0,0) == 0);
    sc55::PitchModulationInputs modulation;
    require(sc55::PrepareModulatedPitch(130000,modulation) == 127000);
    modulation.offset = 65535;
    require(sc55::PrepareModulatedPitch(130000,modulation) == 129999);
    require(sc55::PrepareModulatedPitch(0,modulation) == 0);
    modulation.offset = 0;
    modulation.sources[0] = {1000,2000,16384};
    modulation.sources[1] = {1000,0,49152};
    modulation.masterTune = 1034; modulation.partTune = 65516;
    require(sc55::PrepareModulatedPitch(60000,modulation) == 60990);
    {
        sc55::VoicePitchRunner voice;
        sc55::PitchGlideRates rates; rates.fill(1);
        const sc55::PitchConversion conversion;
        sc55::PitchModulationInputs input; input.offset = 1000;
        voice.envelope.stage = 10; voice.envelope.segment.target = 81000;
        voice.glide.increment = 100;
        using Result = sc55::VoicePitchRunner::Result;
        require(voice.advance(1,false,input,0,rates,81000,128,conversion) == Result::updated);
        require(voice.envelope.output == 81000 && voice.glide.pitch.accumulator == 82099);
        require(voice.advance(1,false,input,0,rates,81000,128,conversion) == Result::updated);
        require(voice.glide.pitch.accumulator == 82098 && voice.glide.increment == 98);
        require(voice.pcmWord == conversion.fromDelta(82098u-81000u-12000u));
        const auto oldWord = voice.pcmWord;
        voice.envelope.stage = 2; voice.envelope.segment.progress = {65535,7};
        require(voice.advance(1,false,input,128,rates,81000,128,conversion) == Result::invalidInput);
        require(voice.envelope.stage == 2 && voice.envelope.segment.progress.position == 65535);
        require(voice.envelope.segment.progress.deferredTicks == 7 && voice.glide.increment == 98);
        require(voice.pcmWord == oldWord && voice.glide.pitch.accumulator == 82098);
        voice.envelope.stage = 14; voice.envelope.segment.progress.position = 0;
        require(voice.advance(1,false,input,255,rates,81000,128,conversion) == Result::idle);
        require(voice.pcmWord == oldWord && voice.glide.increment == 98);
        voice.envelope.stage = 10; voice.glide.increment = 0;
        require(voice.advance(1,false,input,255,rates,81000,128,conversion) == Result::updated);
        require(voice.glide.pitch.accumulator == 82000);
        const sc55::PitchEnvelopeTargets targets{256,{60000,61000,62000,63000,55000},0};
        const sc55::PitchEnvelopeTiming timing{{256,256},256,{65535,32768,16384,8192,4096}};
        voice.envelope.stage = 12; voice.envelope.segment.progress = {32768,0};
        voice.glide.increment = 99;
        voice.installEnvelope(64000,targets,timing);
        require(voice.envelope.stage == 12 && voice.envelope.segment.progress.position == 32768);
        require(voice.glide.increment == 99 && voice.glide.pitch.accumulator == 60000);
        require(voice.envelope.nextTargets == std::array<uint32_t,3>{62000,63000,64000});
        require(voice.envelope.nextIncrements == std::array<uint16_t,3>{32768,16384,8192});
        require(voice.envelope.segment.increment == 65535 && voice.envelope.releaseIncrement == 4096);
        require(voice.envelope.reenter(0) == sc55::PitchEnvelopeRunner::Result::updated);
        require(voice.envelope.segment.start == 64000 && voice.envelope.segment.target == 55000);
        require(voice.envelope.output == 59500);
        voice.envelope.stage = 14; // Initialization bypasses inactive dispatch.
        voice.envelope.segment = {{65535,500},60000,62000,32768,0};
        voice.glide.increment = 100; voice.glide.pitch.correction = {128,25};
        input = {};
        require(voice.initialize(input,0,rates,81000,0,conversion) == Result::updated);
        require(voice.envelope.stage == 14 && voice.envelope.output == 61000);
        require(voice.envelope.segment.progress.position == 32768 && voice.envelope.segment.progress.deferredTicks == 0);
        require(voice.glide.increment == 99 && voice.glide.pitch.accumulator == 61099);
        require(voice.glide.pitch.correction.source == 0 && voice.glide.pitch.correction.offset == 25);
        const auto initializedWord = voice.pcmWord;
        require(voice.initialize(input,128,rates,81000,128,conversion) == Result::invalidInput);
        require(voice.glide.pitch.correction.offset == 25 && voice.glide.increment == 99);
        require(voice.envelope.segment.progress.position == 32768 && voice.pcmWord == initializedWord);
    }
    Runner runner;
    {
        sc55::SecondEnvelopeKeyTables keys{}; sc55::SecondEnvelopeTargetTables targets;
        sc55::SecondEnvelopeTimingKeyCurves curves; sc55::SecondEnvelopePcmTables tables;
        std::array<uint8_t,256> curve{}; std::array<uint16_t,256> multipliers{}; sc55::EnvelopeTimes times{};
        SC55Partial partial;
        for (unsigned at : {0x3bu,0x3cu,0x3eu,0x3fu}) partial.raw[at] = 64;
        sc55::SecondEnvelopeReleaseState segment; segment.stage = 12;
        sc55::SecondEnvelopePcmState pcm{17,2345,3456,4567};
        sc55::SecondEnvelopeSetup setup;
        const auto prepare = [&] { return setup.prepare(partial,60,100,segment,pcm,keys,targets,curve,curves,multipliers,times,tables); };
        partial.raw[0x3b] = 43;
        require(!prepare());
        require(segment.stage == 12 && pcm.control == 17 && pcm.output == 2345 && pcm.level == 3456 && pcm.command == 4567);
        partial.raw[0x27] = 2;
        require(prepare() && setup.mode.bypass && pcm.level == 0 && pcm.output == 2345);
        partial.raw[0x27] = 0; partial.raw[0x3b] = 64;
        require(prepare() && !setup.mode.bypass && setup.mode.pcmMode == 1);
        require(segment.stage == 12 && segment.progress.position == 65535 && segment.progress.deferredTicks == 0);
        require(pcm.control == 8 && pcm.level == 0 && setup.activation.command == 0xffba);
    }
    {
        sc55::SecondEnvelopePcmState pcm{17,2345,3456,4567};
        sc55::SecondEnvelopeActivation activation{5678,6789};
        auto mode = sc55::ConfigureSecondEnvelopeMode(0,pcm,activation);
        require(!mode.bypass && mode.pcmMode == 1 && pcm.control == 17 && pcm.level == 3456 && activation.command == 6789);
        mode = sc55::ConfigureSecondEnvelopeMode(1,pcm,activation);
        require(!mode.bypass && mode.pcmMode == 3 && pcm.command == 4567);
        mode = sc55::ConfigureSecondEnvelopeMode(255,pcm,activation);
        require(mode.bypass && mode.pcmMode == 3 && pcm.control == 64 && pcm.output == 2345);
        require(pcm.level == 0 && pcm.command == 0 && activation.level == 0 && activation.command == 0);
        mode = sc55::ConfigureSecondEnvelopeMode(0,pcm,activation);
        require(!mode.bypass && mode.pcmMode == 1 && pcm.control == 64 && pcm.command == 0);
    }
    {
        sc55::SecondEnvelopeTimingKeyCurves curves;
        curves.attack[15].fill(0); curves.release[14].fill(255);
        std::array<uint16_t,256> multipliers;
        for (unsigned i = 0; i < 256; ++i) multipliers[i] = uint16_t(i*257);
        SC55Partial partial;
        partial.raw[0x39] = 15; partial.raw[0x3a] = 14;
        partial.raw[0x3b] = 44; partial.raw[0x3c] = 84;
        partial.raw[0x3e] = 44; partial.raw[0x3f] = 84;
        auto timing = sc55::PrepareSecondEnvelopeTiming(partial,255,0,curves,multipliers);
        require(timing && timing->keyScale == *sc55::EnvelopeKeyScale(0,44,multipliers));
        require(timing->releaseKeyScale == *sc55::EnvelopeKeyScale(255,84,multipliers));
        require(timing->attackVelocityScale == 4 && timing->decayReleaseVelocityScale == *sc55::EnvelopeVelocityScale(0,84));
        require(timing->attack == 64 && timing->decay == 64 && timing->release == 64 && !timing->attackControlEnabled);
        partial.raw[0x39] = 16;
        require(!sc55::PrepareSecondEnvelopeTiming(partial,0,0,curves,multipliers));
        partial.raw[0x39] = 15; partial.raw[0x3e] = 43;
        require(!sc55::PrepareSecondEnvelopeTiming(partial,0,0,curves,multipliers));
    }
    {
        sc55::SecondEnvelopeReleaseState segment;
        sc55::SecondEnvelopePcmTables tables;
        for (unsigned i = 0; i < 256; ++i) tables.smoothingCeiling[i] = uint8_t(i);
        for (unsigned i = 0; i < 129; ++i) tables.levelCurve[i] = uint16_t(i*128);
        segment.start = segment.target = segment.releaseTarget = 65535; segment.nextTargets.fill(65535);
        auto initial = sc55::PrepareSecondEnvelopeInitialControl(segment,0,64,false,64,64,tables);
        require(initial && initial->limit == 0 && initial->control == 0);
        segment.releaseTarget = 257;
        initial = sc55::PrepareSecondEnvelopeInitialControl(segment,0,64,false,64,64,tables);
        require(initial && initial->limit == 2 && initial->control == 2);
        initial = sc55::PrepareSecondEnvelopeInitialControl(segment,0,64,false,64,127,tables);
        require(initial && initial->limit == 2 && initial->control == 0);
        segment.start = 32767;
        initial = sc55::PrepareSecondEnvelopeInitialControl(segment,0,64,false,64,64,tables);
        require(initial && initial->limit == 127 && initial->control == 64);
        require(!sc55::PrepareSecondEnvelopeInitialControl(segment,128,64,false,64,64,tables));
        require(!sc55::PrepareSecondEnvelopeInitialControl(segment,0,128,false,64,64,tables));
        require(!sc55::PrepareSecondEnvelopeInitialControl(segment,0,64,false,128,64,tables));
        require(!sc55::PrepareSecondEnvelopeInitialControl(segment,0,64,false,64,128,tables));
    }
    {
        sc55::SecondEnvelopeTargetTables tables;
        tables.startSensitivity.fill(256); tables.velocitySensitivity.fill(1000); tables.scale.fill(12345);
        SC55Partial partial; partial.raw[0x29] = 65; partial.raw[0x3d] = 64;
        auto values = sc55::PrepareSecondEnvelopeTargetInputs(partial,16484,127,tables);
        require(values.start == 100 && values.depth == 32767 && values.scale == 12345);
        partial.raw[0x29] = 63;
        require(sc55::PrepareSecondEnvelopeTargetInputs(partial,16484,127,tables).start == 65436);
        require(sc55::PrepareSecondEnvelopeTargetInputs(partial,16284,127,tables).start == 100);
        partial.raw[0x3d] = 65;
        require(sc55::PrepareSecondEnvelopeTargetInputs(partial,16384,0,tables).depth == uint16_t(32767u-uint16_t(127000)));
        partial.raw[0x3d] = 63;
        require(sc55::PrepareSecondEnvelopeTargetInputs(partial,16384,0,tables).depth == uint16_t(uint16_t(127000)-32767u));
        require(sc55::PrepareSecondEnvelopeTargetInputs(partial,16384,127,tables).start == 0);
        sc55::SecondEnvelopeKeyTables keys{};
        keys[15][255] = 16484;
        partial.raw[0x28] = 255; partial.raw[0x29] = 65;
        std::array<uint8_t,256> curve{};
        sc55::SecondEnvelopeReleaseState state;
        state.stage = 12; state.level = 789; state.progress = {123,456};
        state.prepareTargets(partial,255,127,keys,tables,curve);
        require(state.start == 100 && state.target == 100 && state.releaseTarget == 100);
        require(state.nextTargets == std::array<uint16_t,3>{100,100,100});
        require(state.stage == 12 && state.level == 789 && state.progress.position == 123 && state.progress.deferredTicks == 456);
    }
    {
        std::array<uint8_t,256> curve{}; curve.fill(128); curve[0] = 0;
        require(sc55::PrepareSecondEnvelopeTarget(1000,1000,32768,65,curve) == 2000);
        require(sc55::PrepareSecondEnvelopeTarget(1000,1000,32768,63,curve) == 0);
        require(sc55::PrepareSecondEnvelopeTarget(1000,64536,32768,63,curve) == 2000);
        require(sc55::PrepareSecondEnvelopeTarget(1000,1000,32768,64,curve) == 1000);
        require(sc55::PrepareSecondEnvelopeTarget(32760,1000,32768,65,curve) == 32767);
        require(sc55::PrepareSecondEnvelopeTarget(32776,1000,32768,63,curve) == 32769);
        require(sc55::PrepareSecondEnvelopeTarget(100,1000,32768,63,curve) == 64636);
        require(sc55::PrepareSecondEnvelopeTarget(65436,1000,32768,65,curve) == 900);
        SC55Partial partial;
        for (unsigned i = 0; i < 5; ++i)
        { partial.raw[0x2d+i] = uint8_t(62+i); partial.raw[0x32+i] = uint8_t(128+i); }
        sc55::SecondEnvelopeReleaseState state;
        state.stage = 12; state.progress = {123,456}; state.level = 789; state.stepTime = 8;
        state.installTargets(partial,1000,1000,32768,curve);
        require(state.start == 1000 && state.target == 0 && state.releaseTarget == 2000);
        require(state.nextTargets == std::array<uint16_t,3>{0,1000,2000});
        require(state.parameter == 0 && state.nextParameters == std::array<uint8_t,3>{1,2,3} && state.releaseParameter == 4);
        require(state.stage == 12 && state.progress.position == 123 && state.progress.deferredTicks == 456 && state.level == 789 && state.stepTime == 8);
    }
    {
        using Result = sc55::SecondEnvelopeReleaseState::Result;
        sc55::SecondEnvelopeReleaseState segment;
        sc55::SecondEnvelopePcmState pcm{8,12,34,56};
        sc55::SecondEnvelopeTiming timing;
        sc55::EnvelopeTimes times{};
        sc55::SecondEnvelopePcmTables tables;
        tables.smoothingCeiling.fill(127); tables.outputCeiling.fill(255);
        sc55::SecondEnvelopeOutputInputs input;
        segment.start = 100; segment.level = 17; segment.progress = {0,19};
        input.base = 128;
        require(sc55::AdvanceSecondEnvelope(segment,pcm,false,1,timing,times,input,64,64,127,tables) == Result::invalidInput);
        require(segment.level == 17 && segment.progress.deferredTicks == 19);
        require(pcm.level == 34 && pcm.output == 12 && pcm.command == 56 && pcm.control == 8);
        require(sc55::AdvanceSecondEnvelope(segment,pcm,true,1,timing,times,input,64,64,127,tables) == Result::idle);
        require(pcm.level == 34 && segment.level == 17);
        segment.stage = 14;
        require(sc55::AdvanceSecondEnvelope(segment,pcm,false,1,timing,times,input,64,64,127,tables) == Result::idle);
        require(pcm.command == 56 && segment.stage == 14);
        segment.stage = 0; input.base = 0;
        require(sc55::AdvanceSecondEnvelope(segment,pcm,false,1,timing,times,input,64,64,127,tables) == Result::updated);
        require(segment.level == 100 && segment.progress.deferredTicks == 19);
        require(pcm.level == 0 && pcm.command == 0xff00 && pcm.output == 100);
        segment.stage = 14; segment.target = 200; segment.progress = {123,19};
        auto activation = sc55::InitializeSecondEnvelope(segment,pcm,timing,times,input,64,64,127,tables);
        require(activation && activation->level == 0 && activation->command == 0xffba);
        require(segment.stage == 14 && segment.level == 200 && segment.progress.position == 65535);
        require(segment.progress.deferredTicks == 0 && pcm.output == 200);
        segment.parameter = 128; segment.level = 123; segment.progress = {456,7};
        const auto previousControl = pcm.control;
        require(!sc55::InitializeSecondEnvelope(segment,pcm,timing,times,input,64,64,127,tables));
        require(segment.level == 123 && segment.progress.position == 456 && segment.progress.deferredTicks == 7);
        require(pcm.control == previousControl && pcm.output == 200);
    }
    {
        sc55::SecondEnvelopeReleaseState state;
        sc55::SecondEnvelopeTiming timing;
        sc55::EnvelopeTimes times{};
        uint16_t pcmLevel = 12345;
        using Result = sc55::SecondEnvelopeReleaseState::Result;
        state.start = 100; state.target = 200; state.progress = {0,17};
        require(state.advance(false,1,timing,times,pcmLevel) == Result::updated);
        require(state.level == 100 && pcmLevel == 0 && state.progress.deferredTicks == 17);
        state.stage = 2; state.nextTargets = {300,400,500}; state.nextParameters = {1,2,3};
        require(state.advance(false,1,timing,times,pcmLevel) == Result::updated && state.stage == 2);
        for (uint16_t stage : {uint16_t(4),uint16_t(6),uint16_t(8),uint16_t(10)})
            require(state.advance(false,0,timing,times,pcmLevel) == Result::updated && state.stage == stage);
        require(state.level == 500 && state.progress.deferredTicks == 0);
        state.stage = 2; state.progress = {65535,9}; state.nextParameters[0] = 128;
        require(state.advance(false,1,timing,times,pcmLevel) == Result::invalidInput);
        require(state.stage == 2 && state.progress.position == 65535 && state.progress.deferredTicks == 9);
        state.stage = 65535;
        require(state.advance(true,1,timing,times,pcmLevel) == Result::idle);
        require(state.stage == 65535 && state.progress.position == 65535);
    }
    {
        sc55::EnvelopeTimes times;
        for (unsigned i = 0; i < times.size(); ++i) times[i] = uint16_t(i*10);
        sc55::SecondEnvelopeTiming timing;
        timing.attack = 127; timing.decay = 63; timing.release = 65;
        timing.releaseKeyScale = 512; timing.decayReleaseVelocityScale = 512;
        using Interval = sc55::SecondEnvelopeInterval;
        require(timing.duration(10,Interval::attack,times) == 100);
        timing.attackControlEnabled = true;
        require(timing.duration(10,Interval::attack,times) == 1270);
        require(timing.duration(10,Interval::decay,times) == 160);
        require(timing.duration(10,Interval::release,times) == 480);
        sc55::SecondEnvelopeReleaseState state;
        state.parameter = 128; state.level = 42; state.progress = {123,17};
        require(!state.advanceInterval(Interval::attack,timing,times,1));
        require(state.level == 42 && state.progress.position == 123 && state.progress.deferredTicks == 17);
    }
    {
        sc55::SecondEnvelopeReleaseState state;
        state.start = 0; state.target = 32768; state.progress = {32768,0};
        state.advanceSegment(65535,0);
        require(state.level == uint16_t(0u-16383u) && state.stepTime == 8);
        state.start = 60000; state.target = 65000; state.progress = {65535,0};
        state.advanceSegment(65535,0);
        require(state.level == 64999); // Exact progress, no overflow.
        state.advanceSegment(65535,1);
        require(state.level == 65000 && state.progress.deferredTicks == 1);
        state.progress = {123,17}; state.advanceSegment(8,65535);
        require(state.level == 65000 && state.progress.position == 65535);
        require(state.progress.deferredTicks == 17 && state.stepTime == 8);
    }
    {
        sc55::EnvelopeRunner amplitude({},{{sc55::EnvelopeStage::sustain,{},{},0,0},1,0xb6,0});
        sc55::VoiceReleaseAuxiliary aux; aux.pending = 1;
        sc55::VoicePcmLevelState levels;
        unsigned reads = 0, writes = 0;
        const std::array<uint8_t,9> readOrder{0x32,0x3a,0x3b,0x34,0x3a,0x3b,0x36,0x3a,0x3b};
        const std::array<uint8_t,7> writeOrder{0x3e,0x16,0x17,0x18,0x19,0x1a,0x1b};
        const auto read = [&](uint8_t a) -> uint8_t {
            require(reads < readOrder.size() && a == readOrder[reads++]);
            return a == 0x3a ? 0x40 : 0;
        };
        const auto write = [&](uint8_t a,uint8_t v) {
            require(writes < writeOrder.size() && a == writeOrder[writes++]);
            require(v == (a == 0x3e ? 5 : a%2 == 0 ? 255 : 0));
        };
        require(sc55::SynchronizeVoicePcm(5,amplitude,aux,levels,read,write));
        require(reads == 9 && writes == 7 && levels.level32 == 32768 && levels.level36 == 32768);
        require(amplitude.state().level == 32768 && aux.activity == 128 && aux.pending == 1);
        sc55::SecondEnvelopePcmState second{57,9876,123,0xb6};
        reads = writes = 0;
        require(sc55::SynchronizeVoicePcm(5,amplitude,aux,levels.level32,levels.command16,second,read,write));
        require(reads == 9 && writes == 7 && second.level == 32768);
        require(second.control == 57 && second.output == 9876 && second.command == 0xb6);
        require(!sc55::SynchronizeVoicePcm(24,amplitude,aux,levels.level32,levels.command16,second,read,write));
        require(second.level == 32768 && reads == 9 && writes == 7);
        Runner pitch;
        require(sc55::ConsumeVoiceRelease(aux,amplitude,pitch) == sc55::VoiceReleaseAction::release);
        require(amplitude.state().segment.start == 128 && aux.pending == 0);
        require(!sc55::SynchronizeVoicePcm(24,amplitude,aux,levels,read,write));
        require(reads == 9 && writes == 7);
    }
    {
        sc55::EnvelopeRunner::Setup setup{};
        setup.plan.stages[4] = {55,66};
        sc55::EnvelopeRunner amplitude(setup,{{sc55::EnvelopeStage::sustain,{123,17},{10,20},30,40},32768,0xff00,0});
        Runner pitch; pitch.output = 60000; pitch.releaseTarget = 55000;
        sc55::VoiceReleaseAuxiliary aux;
        aux.second = {6,{456,7},22,33,4444,5555,6666,7777};
        aux.modulationReleaseWords = {0,123}; aux.activity = 99;
        require(!sc55::ConsumeVoiceRelease(aux,amplitude,pitch));
        require(amplitude.state().segment.stage == sc55::EnvelopeStage::sustain);
        aux.pending = 255;
        require(sc55::ConsumeVoiceRelease(aux,amplitude,pitch) == sc55::VoiceReleaseAction::release);
        require(aux.pending == 0 && aux.activity == 99);
        require(amplitude.state().segment.start == 128 && amplitude.state().segment.target == 0);
        require(aux.second.stage == 12 && aux.second.start == 4444 && aux.second.target == 7777 && aux.second.parameter == 33);
        require(aux.modulationReleaseWords == std::array<uint16_t,2>{65535,123});
        require(pitch.stage == 12 && pitch.segment.start == 60000 && pitch.segment.target == 55000);
        pitch.segment.progress.position = 321;
        aux.pending = 1;
        require(sc55::ConsumeVoiceRelease(aux,amplitude,pitch) == sc55::VoiceReleaseAction::unchanged);
        require(aux.pending == 0 && pitch.segment.progress.position == 321);
    }
    {
        Runner pitch;
        pitch.stage = 10; pitch.output = 60000; pitch.releaseTarget = 55000;
        pitch.segment = {{123,17},12345,65000,32768,0};
        require(pitch.requestRelease(0,0) == sc55::VoiceReleaseAction::start);
        require(pitch.stage == 2 && pitch.segment.progress.position == 123 && pitch.segment.progress.deferredTicks == 17);
        require(pitch.requestRelease(0,1) == sc55::VoiceReleaseAction::cancel);
        require(pitch.stage == 22 && pitch.segment.start == 12345 && pitch.output == 60000);
        // First-envelope state, not this pitch's already-finished state, wins.
        require(pitch.requestRelease(10,1) == sc55::VoiceReleaseAction::release);
        require(pitch.stage == 12 && pitch.segment.start == 60000 && pitch.segment.target == 55000);
        pitch.segment.progress = {321,9};
        require(pitch.requestRelease(12,0) == sc55::VoiceReleaseAction::unchanged);
        require(pitch.segment.progress.position == 321 && pitch.segment.progress.deferredTicks == 9);
    }
    for (uint32_t current : {0u,60000u,0xffffffu})
        for (uint32_t target : {0u,60000u,0xffffffu})
        {
            Runner release;
            release.output = current; release.nextTargets[2] = 12345;
            release.segment = {{65535,17},123,456,32768,2};
            release.releaseTarget = target; release.releaseIncrement = 65535;
            release.release();
            require(release.stage == 12 && release.output == current);
            require(release.segment.start == current && release.segment.target == target);
            require(release.segment.direction == (target < current ? 2 : 0));
            require(release.segment.progress.position == 0 && release.segment.progress.deferredTicks == 0);
            require(release.advance(1) == Runner::Result::updated);
            require(release.stage == 12 && release.segment.progress.position == 65535);
            require(release.advance(0) == Runner::Result::updated && release.stage == 22);
            require(release.output == target);
        }
    {
        sc55::VoiceStopState voice; voice.stages = {2,0,0};
        voice.cached18 = 0x1234; voice.cached16 = 0x5678; voice.pcm10 = 0x9abc;
        sc55::PreparedVoicePcm prepared;
        prepared.pcm12 = 0x1122; prepared.pcm14 = 0x3344; prepared.pcm1c = 0x5566;
        prepared.pcm1a = 0xdead;
        const std::array<std::pair<uint8_t,uint8_t>,15> expected{{
            {0x3e,23},{0x18,0x12},{0x19,0x34},{0x16,0x56},{0x17,0x78},
            {0x12,0x11},{0x13,0x22},{0x14,0x33},{0x15,0x44},
            {0x1a,0x77},{0x1b,0x88},{0x1c,0x55},{0x1d,0x66},{0x10,0x9a},{0x11,0xbc}}};
        unsigned count = 0;
        const auto write = [&](uint8_t address,uint8_t value) {
            require(count < expected.size() && expected[count] == std::pair{address,value}); ++count;
        };
        using Result = sc55::VoicePcmUpdateResult;
        require(sc55::UpdateVoicePcm(23,voice,prepared,0x7788,write) == Result::written);
        require(count == expected.size());
        sc55::SecondEnvelopePcmState second{0x55,123,456,0x7788};
        prepared.pcm1c = 0xaa66;
        count = 0;
        require(sc55::UpdateVoicePcm(23,voice,prepared,second,write) == Result::written);
        require(count == expected.size() && prepared.pcm1c == 0xaa66);
        require(second.level == 456 && second.output == 123);
        require(sc55::UpdateVoicePcm(24,voice,prepared,second,write) == Result::invalidChannel);
        for (uint16_t stage : {uint16_t(0),uint16_t(14),uint16_t(22),uint16_t(65535)})
        {
            voice.stages[0] = stage;
            require(sc55::UpdateVoicePcm(23,voice,prepared,0,write) == Result::idle);
        }
        voice.stages[0] = 2;
        require(sc55::UpdateVoicePcm(24,voice,prepared,0,write) == Result::invalidChannel);
        require(count == expected.size());
    }
    for (uint8_t flags : {uint8_t(0),uint8_t(128)})
        for (uint16_t delay : {uint16_t(0),uint16_t(1)})
        {
            sc55::VoicePitchRunner pitch;
            pitch.envelope.stage = 12; pitch.envelope.segment.progress = {123,17};
            pitch.pcmWord = 0x3456; pitch.glide.increment = 789;
            sc55::VoiceStopState voice;
            voice.stages = {8,6,4}; voice.cached18 = 0x1234;
            voice.flagMinus3B = flags; voice.delayAccumulator = delay; voice.savedStage = 10;
            sc55::PrepareVoicePitch(voice,pitch);
            require(voice.stages[0] == 8 && voice.stages[1] == 6 && voice.stages[2] == 12);
            std::array<std::pair<uint8_t,uint8_t>,4> writes{};
            unsigned count = 0;
            sc55::ActivatePreparedVoice(voice,[&](uint8_t address,uint8_t value) {
                require(count < writes.size()); writes[count++] = {address,value};
            });
            const bool delayed = flags == 128 && delay == 0;
            require(count == 4 && writes[0].first == 0x18 && writes[1].first == 0x19);
            require(writes[2] == std::pair<uint8_t,uint8_t>{0x10,uint8_t(delayed ? 0 : 0x34)});
            require(writes[3] == std::pair<uint8_t,uint8_t>{0x11,uint8_t(delayed ? 0 : 0x56)});
            const auto continued = sc55::ContinueVoicePitch(voice,pitch);
            require(continued && continued->envelope.stage == (flags == 0 ? 12 : delayed ? 0 : 2));
            require(continued->pcmWord == 0x3456 && continued->glide.increment == 789);
            require(continued->envelope.segment.progress.position == 123 && continued->envelope.segment.progress.deferredTicks == 17);
            voice.stages[2] = 23;
            require(!sc55::ContinueVoicePitch(voice,pitch));
        }
    runner.segment = {{65535,3},60000,61000,65535,0};
    runner.nextTargets = {59000,62000,60000};
    runner.nextIncrements = {65535,65535,65535};
    // One transition per call, even with enough leftover time for more.
    require(runner.advance(0) == Runner::Result::updated);
    require(runner.stage == 4 && runner.segment.start == 61000);
    require(runner.segment.target == 59000 && runner.segment.direction == 2);
    require(runner.segment.progress.position == 65535);
    require(runner.segment.progress.deferredTicks == 2);
    require(runner.output == 59001); // Full progress still truncates interpolation.
    require(runner.advance(0) == Runner::Result::updated && runner.stage == 6);
    require(runner.segment.progress.deferredTicks == 1 && runner.output == 61999);
    require(runner.advance(0) == Runner::Result::updated && runner.stage == 8);
    require(runner.segment.progress.deferredTicks == 0 && runner.output == 60001);
    require(runner.advance(0) == Runner::Result::updated && runner.stage == 10);
    require(runner.output == 60000 && runner.segment.progress.position == 0);
    require(runner.advance(65535) == Runner::Result::updated && runner.stage == 10);
    require(runner.output == 60000);

    runner.stage = 12; runner.segment.progress = {65535,17};
    require(runner.advance(1) == Runner::Result::updated && runner.stage == 22);
    require(runner.segment.progress.position == 0 && runner.segment.progress.deferredTicks == 0);

    runner.stage = 12;
    runner.releaseTarget = 55000; runner.releaseIncrement = 32768;
    runner.segment.progress = {0,0};
    require(runner.reenter(1) == Runner::Result::updated);
    require(runner.stage == 12 && runner.segment.start == 60000);
    require(runner.segment.target == 55000 && runner.output == 57500);
    require(runner.segment.progress.position == 32768);
    // Re-entry does not restart progress, nor start from the current output.
    runner.releaseTarget = 65000;
    require(runner.reenter(0) == Runner::Result::updated);
    require(runner.segment.start == 60000 && runner.output == 62500);
    require(runner.segment.direction == 0 && runner.segment.progress.position == 32768);
    runner.stage = 10;
    require(runner.reenter(65535) == Runner::Result::updated);
    require(runner.stage == 10 && runner.segment.start == 62000 && runner.output == 60000);

    runner.stage = 14; runner.segment.progress = {123,17}; runner.output = 42;
    require(runner.advance(100) == Runner::Result::idle);
    require(runner.output == 42 && runner.segment.progress.position == 123);
    require(runner.segment.progress.deferredTicks == 17);
    for (uint16_t invalid : {uint16_t(1),uint16_t(23),uint16_t(24),uint16_t(65535)})
    {
        runner.stage = invalid;
        require(runner.advance(65535) == Runner::Result::invalidStage);
        require(runner.stage == invalid && runner.output == 42);
        require(runner.segment.progress.position == 123 && runner.segment.progress.deferredTicks == 17);
        require(runner.reenter(65535) == Runner::Result::invalidStage);
        require(runner.stage == invalid && runner.output == 42);
        require(runner.segment.progress.position == 123 && runner.segment.progress.deferredTicks == 17);
    }
}
