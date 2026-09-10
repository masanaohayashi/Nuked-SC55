#pragma once
#include "mcu.h"
#include "pitch-control-work.h"
#include <map>
#include <cstdio>
#include <stdexcept>

// Full periodic pitch owner checked against real MIDI-driven H8 execution.
// No reference writes and no recorded timing fed into the native product.
struct PitchControlProbe
{
    std::optional<sc55::VoicePitchRunner> expected;
    sc55::PitchConversion conversion;
    sc55::PitchGlideRates rates{};
    bool tablesLoaded=false;
    unsigned base=0,instructions=0,entryStage=0;
    unsigned expectedInstructions=0;
    bool moving=false;
    uint64_t checks=0,movingChecks=0;
    std::array<uint64_t,2> glideDirections{};
    uint64_t correctionRefreshes=0;
    std::map<unsigned,uint64_t> budgets,stages;

    void instruction(mcu_t& cpu,bool inHardwareInterrupt)
    {
        if(MCU_Read16(cpu,0xfdca)!=8 || inHardwareInterrupt) return;
        const auto word=[&](int offset) { return MCU_Read16(cpu,uint16_t(base+offset)); };
        const auto byte=[&](int offset) { return MCU_Read(cpu,uint16_t(base+offset)); };
        const auto pitch=[&](int high,int low) { return (uint32_t(byte(high))<<16)|word(low); };
        if(cpu.cp==0 && cpu.pc==0x4fdb) {
            if(expected) throw std::runtime_error("Overlapping periodic pitch control");
            if(!tablesLoaded) {
                for(unsigned i=0;i<128;++i) rates[i]=MCU_Read16(cpu,0x7a32+2*i);
                tablesLoaded=true;
            }
            base=cpu.r[0]; instructions=0;
            sc55::VoicePitchRunner voice;
            voice.envelope.stage=word(4); entryStage=voice.envelope.stage;
            voice.envelope.segment={{word(0x0c),word(0x16)},pitch(0x6a,0x70),pitch(0x6b,0x72),word(0x7c),byte(-3)};
            for(unsigned i=0;i<3;++i) {
                voice.envelope.nextTargets[i]=pitch(0x6c+i,0x74+2*i);
                voice.envelope.nextIncrements[i]=word(0x7e + 2*i);
            }
            voice.envelope.releaseTarget=pitch(0x6f,0x7a); voice.envelope.releaseIncrement=word(0x84);
            voice.envelope.output=pitch(0x2c,0x44); voice.pcmWord=word(0x48);
            voice.glide.increment=pitch(0x2b,0x42); moving=voice.glide.increment!=0;
            voice.glide.pitch.accumulator=pitch(0x2d,0x46);
            voice.glide.pitch.correction={byte(0xa4),word(0xa6)};
            if(moving) ++glideDirections[bool(voice.glide.increment&0x800000)];
            const auto slot=word(-2);
            if(slot>=24) throw std::runtime_error("Invalid periodic pitch slot");
            const auto part=MCU_Read(cpu,0xc8e4+slot);
            if(part>=16) throw std::runtime_error("Invalid periodic pitch part");
            sc55::PitchModulationInputs input;
            input.offset=word(0x86);
            input.sources[0]={word(-0x76),word(0x90),word(-0x60)};
            input.sources[1]={word(-0x54),word(0x92),word(-0x3e)};
            input.masterTune=MCU_Read16(cpu,0x8000); input.partTune=MCU_Read16(cpu,0xab76+2*part);
            const auto correctionSource=MCU_Read(cpu,word(0x2e)+7);
            correctionRefreshes+=voice.glide.pitch.correction.source!=correctionSource;
            const auto result=PitchControlWork::evaluate(voice,MCU_Read16(cpu,0xac5a),input,
                MCU_Read(cpu,word(-0x3a)),rates,pitch(0x29,0x3e),correctionSource,conversion);
            if(!result)
                throw std::runtime_error("Invalid native periodic pitch input");
            expected=result->voice;expectedInstructions=result->instructions;
        }
        if(expected && cpu.cp==0 && cpu.pc==0x3348) {
            const auto& voice=*expected;
            const auto& segment=voice.envelope.segment;
            if(voice.envelope.stage!=word(4) || voice.envelope.output!=pitch(0x2c,0x44)
                || segment.progress.position!=word(0x0c) || segment.progress.deferredTicks!=word(0x16)
                || segment.start!=pitch(0x6a,0x70) || segment.target!=pitch(0x6b,0x72)
                || segment.increment!=word(0x7c) || segment.direction!=byte(-3)
                || voice.glide.increment!=pitch(0x2b,0x42) || voice.glide.pitch.accumulator!=pitch(0x2d,0x46)
                || voice.glide.pitch.correction.source!=byte(0xa4) || voice.glide.pitch.correction.offset!=word(0xa6)
                || voice.pcmWord!=word(0x48)) {
                std::fprintf(stderr,"[DEBUG-pitch-control] stage=%u/%u envelope=%06x/%06x accumulator=%06x/%06x PCM=%04x/%04x\n",
                    word(4),voice.envelope.stage,pitch(0x2c,0x44),voice.envelope.output,
                    pitch(0x2d,0x46),voice.glide.pitch.accumulator,word(0x48),voice.pcmWord);
                throw std::runtime_error("Complete periodic pitch differs from H8");
            }
            ++checks; movingChecks+=moving; ++budgets[instructions]; ++stages[entryStage]; expected.reset();
            if(instructions!=expectedInstructions) {
                std::fprintf(stderr,"[DEBUG-pitch-control-work] stage=%u glide=%u instructions=%u/%u\n",
                    entryStage,unsigned(moving),instructions,expectedInstructions);
                throw std::runtime_error("Complete periodic pitch work differs from H8");
            }
        }
        if(expected && !(cpu.cp==0 && cpu.pc<0x752)) ++instructions;
    }
    void report() const
    {
        std::printf("[DEBUG-pitch-control] %llu full value/work comparisons PASS, movingGlide=%llu, budgets=%zu\n",
            (unsigned long long)checks,(unsigned long long)movingChecks,budgets.size());
        for(const auto& [stage,count]:stages)
            std::printf("[DEBUG-pitch-stage] stage=%u count=%llu\n",stage,(unsigned long long)count);
        std::printf("[DEBUG-pitch-coverage] positiveGlide=%llu negativeGlide=%llu correctionRefreshes=%llu\n",
            (unsigned long long)glideDirections[0],(unsigned long long)glideDirections[1],
            (unsigned long long)correctionRefreshes);
    }
};
