#pragma once
#include "sc55_envelope_runner.h"
#include "sc55_voice_links.h"
#include "voice-allocator-oracle.h"

// Both machines are initialized once per trajectory. Only external clock,
// controls and release events are injected afterwards; native state is never
// repaired from firmware RAM. Stops before unrelated voice/PCM cleanup.
inline void verifyEnvelopeRunner(mcu_t& cpu,const sc55::EnvelopeTimes& times)
{
    using Runner = sc55::EnvelopeRunner;
    using Stage = sc55::EnvelopeStage;
    unsigned checks = 0;
    verifyVoiceAllocator(cpu);
    unsigned groupDetachChecks = 0;
    for (unsigned voice = 0; voice < 24; ++voice)
        for (unsigned next = 0; next < 26; ++next)
            for (unsigned previous = 0; previous < 26; ++previous)
            {
                sc55::VoiceGroupLinks groups;
                sc55::VoiceLinks links;
                const unsigned group = (voice + next + previous) % 24;
                for (unsigned i = 0; i < 24; ++i)
                {
                    groups.next[i] = uint8_t((i+1)%24); groups.previous[i] = uint8_t((i+2)%24);
                    groups.head[i] = uint8_t((i+3)%24); groups.tail[i] = uint8_t((i+4)%24);
                    links.first[i] = uint8_t((i+5)%24); links.second[i] = uint8_t((i+6)%24);
                }
                const auto index = [](unsigned n) { return uint8_t(n < 24 ? n : n == 24 ? 0xff : 0x80); };
                groups.next[voice] = index(next); groups.previous[voice] = index(previous);
                const std::array<uint16_t,6> addresses{0xa390,0xa3a8,0xa2a0,0xa2b8,0xcac4,0xcadc};
                const std::array<std::array<uint8_t,24>*,6> arrays{
                    &groups.next,&groups.previous,&groups.head,&groups.tail,&links.first,&links.second};
                for (unsigned a = 0; a < arrays.size(); ++a)
                    for (unsigned i = 0; i < 24; ++i) MCU_Write(cpu,addresses[a]+i,(*arrays[a])[i]);
                cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = 0x1e7e; cpu.sr = 0;
                cpu.r[1] = uint16_t(voice); cpu.r[2] = uint16_t(group); cpu.r[3] = 13; cpu.r[7] = 0x9300;
                unsigned instructions = 0;
                while (cpu.pc != 0x1ecd)
                {
                    if (++instructions > 30) throw std::runtime_error("Group detach escaped");
                    const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                }
                if (!groups.detach(voice,group,links) || cpu.r[3] != 13 || cpu.r[7] != 0x9300)
                    throw std::runtime_error("Group detach rejected input or corrupted registers");
                for (unsigned a = 0; a < arrays.size(); ++a)
                    for (unsigned i = 0; i < 24; ++i)
                        if ((*arrays[a])[i] != MCU_Read(cpu,addresses[a]+i))
                            throw std::runtime_error("Group detach differs from firmware");
                ++groupDetachChecks;
            }
    std::printf("Native group detach: %u combinations matched\n",groupDetachChecks);
    unsigned detachChecks = 0;
    for (unsigned voice = 0; voice < 24; ++voice)
        for (unsigned first = 0; first <= 24; ++first)
            for (unsigned second = 0; second <= 24; ++second)
            {
                sc55::VoiceLinks links;
                for (unsigned i = 0; i < 24; ++i)
                { links.first[i] = uint8_t((i+1)%24); links.second[i] = uint8_t((i+2)%24); }
                links.first[voice] = first == 24 ? 0xff : uint8_t(first);
                links.second[voice] = second == 24 ? 0xff : uint8_t(second);
                for (unsigned i = 0; i < 24; ++i)
                { MCU_Write(cpu,0xcac4+i,links.first[i]); MCU_Write(cpu,0xcadc+i,links.second[i]); }
                cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = 0x339b; cpu.sr = 0; cpu.r[1] = uint16_t(voice);
                unsigned instructions = 0;
                while (cpu.pc != 0x33c9)
                {
                    if (++instructions > 25) throw std::runtime_error("Voice detach escaped");
                    const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                }
                if (!links.detach(voice)) throw std::runtime_error("Voice detach rejected valid input");
                for (unsigned i = 0; i < 24; ++i)
                    if (links.first[i] != MCU_Read(cpu,0xcac4+i) || links.second[i] != MCU_Read(cpu,0xcadc+i))
                        throw std::runtime_error("Voice detach differs from firmware");
                ++detachChecks;
            }
    std::printf("Native voice detach: %u combinations matched\n",detachChecks);
    unsigned activationChecks = 0;
    for (uint16_t accumulator : {uint16_t(0),uint16_t(1),uint16_t(65535)})
        for (uint16_t progress : {uint16_t(0),uint16_t(1),uint16_t(65535)})
            for (unsigned pcm = 0; pcm < 65536; pcm += 257)
            {
                Runner runner({{}, {},256,256,1},{{Stage::finished,{progress,7},{10,4},0,100},1234,uint16_t(pcm),accumulator});
                runner.activateNewVoice();
                cpu.cp = cpu.dp = cpu.ep = 0; cpu.pc = 0x57ca; cpu.sr = 0; cpu.r[0] = 0x9400;
                MCU_Write16(cpu,0x940e,accumulator); MCU_Write16(cpu,0x9408,progress);
                MCU_Write16(cpu,0x941e,uint16_t(pcm)); MCU_Write16(cpu,0x9448,0x1234);
                unsigned instructions = 0;
                while (cpu.pc != 0x57ee)
                {
                    if (++instructions > 20) throw std::runtime_error("Envelope activation escaped");
                    const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                }
                const auto wordAt = [&](uint16_t at) { return uint16_t((MCU_Read(cpu,at)<<8)|MCU_Read(cpu,at+1)); };
                if (wordAt(0x9400) != unsigned(runner.state().segment.stage)*2
                    || wordAt(0x9408) != runner.state().segment.progress.position || cpu.r[5] != runner.state().pcmWord)
                    throw std::runtime_error("Envelope activation differs from firmware");
                ++activationChecks;
            }
    std::printf("Native envelope activation: %u combinations matched\n",activationChecks);
    const auto byte = [&](uint16_t a) { return MCU_Read(cpu,a); };
    const auto word = [&](uint16_t a) { return uint16_t((byte(a)<<8)|byte(a+1)); };
    sc55::EnvelopeLevelTables levels;
    std::copy_n(cpu.rom1+0x6b0f,128,levels.attenuation.begin());
    std::copy_n(cpu.rom1+0x6b8f,256,levels.level.begin());
    unsigned baseChecks = 0;
    for (unsigned base = 0; base < 256; ++base)
        for (unsigned value = 0; value < 128; ++value)
            for (unsigned variant = 0; variant < 3; ++variant)
            {
                const uint8_t amplitude = uint8_t(value), record = uint8_t((value+variant*37)%128), patch = uint8_t((value+variant*61)%128);
                cpu.pc = 0x2cda; cpu.cp = cpu.dp = cpu.ep = 0; cpu.sr = 0;
                cpu.r[0] = 0x9400; cpu.r[1] = 0; cpu.r[2] = uint16_t(base); cpu.r[3] = 0;
                MCU_Write(cpu,0xc944,amplitude); MCU_Write(cpu,0x9000,record); MCU_Write(cpu,0x910c,patch);
                MCU_Write(cpu,0x9498,0); MCU_Write(cpu,0x9499,0); MCU_Write(cpu,0x949a,0);
                MCU_Write16(cpu,0x949c,0x9100); MCU_Write16(cpu,0x949e,0x9200); MCU_Write16(cpu,0x94a0,0x9000);
                unsigned instructions = 0;
                while (cpu.pc != 0x2d14)
                {
                    if (++instructions > 40) throw std::runtime_error("Envelope base escaped");
                    const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                }
                if (sc55::PrepareEnvelopeBase(uint8_t(base),amplitude,record,patch,levels) != cpu.r[2])
                    throw std::runtime_error("Envelope base differs from firmware");
                ++baseChecks;
            }
    std::printf("Native envelope base: %u combinations matched\n",baseChecks);
    unsigned targetChecks = 0;
    for (unsigned base = 0; base < 256; ++base)
        for (unsigned parameter = 0; parameter < 128; ++parameter)
        {
            SC55Partial partial{};
            for (unsigned i = 0; i < 4; ++i)
            {
                partial.raw[0x4a+i] = uint8_t((parameter+31*i)%128);
                MCU_Write(cpu,0x924a+i,partial.raw[0x4a+i]);
            }
            cpu.pc = 0x2d14; cpu.sr = 0; cpu.cp = cpu.dp = cpu.ep = 0;
            cpu.r[0] = 0x9400; cpu.r[2] = uint16_t(base); cpu.r[3] = 0; cpu.r[5] = 0x9200;
            unsigned instructions = 0;
            while (cpu.pc != 0x2d67)
            {
                if (++instructions > 60) throw std::runtime_error("Envelope targets escaped");
                const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
            }
            const auto predicted = sc55::PrepareEnvelopeTargets(partial,uint8_t(base),levels);
            for (unsigned i = 0; i < 4; ++i)
                if (!predicted || (*predicted)[i] != byte(0x9461+i))
                    throw std::runtime_error("Envelope targets differ from firmware");
            ++targetChecks;
        }
    std::printf("Native envelope targets: %u four-target combinations matched\n",targetChecks);
    unsigned startChecks = 0;
    for (unsigned parameter = 0; parameter < 128; ++parameter)
        for (unsigned delay = 0; delay < 256; ++delay)
        {
            Runner::Setup setup{{{{{uint8_t(parameter),uint8_t((delay&1) ? 4 : 0)}, {1,0},{2,0},{3,0},{4,0}}},256,256},
                {100,80,60,40},256,256,123};
            const auto predicted = Runner::start(setup,Stage::delay,uint8_t(delay),64,times);
            if (!predicted) throw std::runtime_error("Envelope start rejected valid input");
            cpu.cp = cpu.dp = cpu.ep = 0; cpu.sr = 0; cpu.pc = 0x2f5a;
            cpu.r[0] = 0x9400; cpu.r[5] = 0x9200; cpu.r[7] = 0x9800;
            MCU_Write(cpu,0x9200,uint8_t(delay)); MCU_Write16(cpu,0x942e,0x9000);
            MCU_Write(cpu,0x9014,64); MCU_Write16(cpu,0x93de,256); MCU_Write16(cpu,0x93e2,256);
            MCU_Write(cpu,0x944f,uint8_t(parameter)); MCU_Write(cpu,0x93f8,setup.plan.stages[0].flag);
            MCU_Write(cpu,0x9460,0); MCU_Write(cpu,0x9461,100); MCU_Write(cpu,0x9464,40);
            MCU_Write16(cpu,0x9400,0); MCU_Write16(cpu,0x93fe,0); MCU_Write16(cpu,0xac5a,37);
            unsigned instructions = 0;
            while (cpu.pc != 0x2fcc)
            {
                if (++instructions > 300) throw std::runtime_error("Envelope start escaped routine");
                const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
            }
            const auto& state = predicted->state();
            if (state.segment.progress.position != word(0x9408) || state.segment.progress.deferredTicks != word(0x9412)
                || state.level != word(0x941c) || state.pcmWord != word(0x941e) || state.delayAccumulator != word(0x940e)
                || predicted->setup().delayIncrement != word(0x9410) || predicted->setup().targets[3] != byte(0x9464)
                || word(0x9400) != 0 || word(0xac5a) != 37)
                throw std::runtime_error("Envelope start differs from firmware");
            ++startChecks;
        }
    std::printf("Native envelope start: %u combinations matched\n",startChecks);
    for (unsigned parameter = 0; parameter < 128; ++parameter)
        for (unsigned variant = 0; variant < 3; ++variant)
        {
            Runner::Setup setup{{{},256,256},{100,80,60,uint8_t(variant == 2 ? 0 : 40)},256,257,uint16_t(variant)};
            for (unsigned i = 0; i < 5; ++i)
            {
                setup.plan.stages[i] = {uint8_t((parameter+i)%128),uint8_t((parameter&1) ? 4 : 0)};
                MCU_Write(cpu,0x944f+i,setup.plan.stages[i].value);
                MCU_Write(cpu,0x93f8+i,setup.plan.stages[i].flag);
            }
            Runner::State initial{{variant == 1 ? Stage::delay : Stage::attack1,{0,0},setup.plan.stages[0],0,100},0,0xff00,65534};
            Runner runner(setup,initial);
            MCU_Write16(cpu,0x9400,unsigned(initial.segment.stage)*2);
            MCU_Write16(cpu,0x9408,0); MCU_Write16(cpu,0x9412,0);
            MCU_Write16(cpu,0x940e,65534); MCU_Write16(cpu,0x9410,setup.delayIncrement);
            MCU_Write16(cpu,0x941c,0); MCU_Write16(cpu,0x941e,0xff00);
            MCU_Write16(cpu,0x93fe,0); MCU_Write16(cpu,0x942e,0x9000);
            MCU_Write16(cpu,0x93de,setup.keyScale); MCU_Write16(cpu,0x93e0,setup.releaseKeyScale);
            MCU_Write16(cpu,0x93e2,256); MCU_Write16(cpu,0x93e4,256);
            MCU_Write(cpu,0x9460,0);
            for (unsigned i = 0; i < 4; ++i) MCU_Write(cpu,0x9461+i,setup.targets[i]);
            bool finished = false;
            for (unsigned tick = 0; tick < 256 && !finished; ++tick)
            {
                cpu.cp = cpu.dp = cpu.ep = 0; cpu.sr = 0; cpu.r[0] = 0x9400; cpu.r[7] = 0x9800;
                if (tick == 64)
                {
                    // A PCM readback is an external input to both paths.
                    runner.release(0x2345); MCU_Write16(cpu,0x941c,0x2345);
                    cpu.pc = 0x3220; cpu.r[1] = 0;
                    unsigned instructions = 0;
                    while (cpu.pc != 0x328b && cpu.pc != 0x32f0 && cpu.pc != 0x3363)
                    {
                        if (++instructions > 50) throw std::runtime_error("Runner release escaped");
                        const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                    }
                }
                const uint16_t elapsed[] {0,1,9,65535};
                const Runner::Controls controls{uint8_t(tick%128),uint8_t((tick+32)%128),uint8_t((tick+64)%128)};
                MCU_Write(cpu,0x9014,controls.attack); MCU_Write(cpu,0x9015,controls.decay); MCU_Write(cpu,0x9016,controls.release);
                MCU_Write16(cpu,0xac5a,elapsed[tick%4]);
                if (!runner.tick(elapsed[tick%4],controls,times)) throw std::runtime_error("Runner rejected valid inputs");
                cpu.pc = 0x33f4;
                unsigned instructions = 0;
                while (cpu.pc != 0x36a7 && cpu.pc != 0x36ad && cpu.pc != 0x36da
                    && cpu.pc != 0x348b && cpu.pc != 0x3471 && cpu.pc != 0x3472)
                {
                    if (++instructions > 300) throw std::runtime_error("Runner tick escaped");
                    const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                }
                const auto& state = runner.state();
                finished = cpu.pc == 0x3472;
                const auto stage = finished ? Stage::finished : static_cast<Stage>(word(0x9400)/2);
                if (state.segment.stage != stage || state.segment.progress.position != word(0x9408)
                    || state.segment.progress.deferredTicks != word(0x9412) || state.level != word(0x941c)
                    || state.pcmWord != word(0x941e) || state.delayAccumulator != word(0x940e)
                    || state.segment.start != byte(0x9460) || state.segment.target != byte(0x9461)
                    || state.segment.parameter.value != byte(0x944f) || state.segment.parameter.flag != byte(0x93f8))
                {
                    std::fprintf(stderr,"Runner mismatch parameter=%u variant=%u tick=%u pc=%04x\n",parameter,variant,tick,cpu.pc);
                    throw std::runtime_error("Persistent envelope differs from firmware");
                }
                ++checks;
            }
            if (!finished) throw std::runtime_error("Runner trajectory did not finish");
        }
    std::printf("Native persistent envelope: %u ticks across 384 trajectories matched\n",checks);
}
