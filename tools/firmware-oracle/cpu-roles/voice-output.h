#pragma once
#include "mcu.h"
#include "voice-output-work.h"
#include <map>
#include <cstdio>
#include <stdexcept>

struct VoiceOutputProbe
{
    std::optional<VoiceOutputWork> expected;
    sc55::PanTable pan{};
    bool loaded=false;
    unsigned base=0,instructions=0;
    uint64_t checks=0,frozen=0,scaled=0,movingPan=0,movingSends=0;
    std::map<unsigned,uint64_t> budgets;

    void instruction(mcu_t& cpu,bool inHardwareInterrupt)
    {
        if(MCU_Read16(cpu,0xfdca)!=8 || inHardwareInterrupt) return;
        const auto word=[&](int offset) {return MCU_Read16(cpu,uint16_t(base+offset));};
        if(cpu.cp==0 && cpu.pc==0x36db) {
            if(expected) throw std::runtime_error("Overlapping voice output operation");
            if(!loaded) {
                for(unsigned i=0;i<pan.size();++i) pan[i]=MCU_Read(cpu,0x6c8f+i);
                loaded=true;
            }
            base=cpu.r[0];instructions=0;
            const auto slot=word(-2);
            if(slot>=24) throw std::runtime_error("Invalid output-control slot");
            const auto part=MCU_Read(cpu,0xc8e4+slot);
            if(part>=16) throw std::runtime_error("Invalid output-control part");
            const auto patch=word(0x2e),tone=word(0x30);
            sc55::LevelInputs level;
            level.expression=MCU_Read(cpu,0xab36+part);
            level.velocity=MCU_Read(cpu,patch+8);level.master=MCU_Read(cpu,0x8002);
            level.has_tone_scale=tone!=0;level.tone_scale=tone ? MCU_Read(cpu,tone+0x100) : 0;
            level.bias=int16_t(word(0x8a));
            level.mod1_a=int16_t(word(-122));level.mod1_b=int16_t(word(0x8e));level.mod1_depth=int16_t(word(-96));
            level.mod2_a=int16_t(word(-88));level.mod2_b=int16_t(word(0x96));level.mod2_depth=int16_t(word(-62));
            sc55::SpatialInputs spatial;
            spatial.pan=MCU_Read(cpu,patch+9);spatial.basePan=uint8_t(word(0x38));
            spatial.masterPan=MCU_Read(cpu,0x8006);spatial.hasToneScale=tone!=0;
            spatial.reverb=MCU_Read(cpu,patch+14);spatial.chorus=MCU_Read(cpu,patch+15);
            if(tone) {
                spatial.panScale=MCU_Read(cpu,tone+0x280);
                spatial.reverbScale=MCU_Read(cpu,tone+0x380);spatial.chorusScale=MCU_Read(cpu,tone+0x300);
            }
            sc55::VoiceOutputState output{{word(6),word(0x18),word(0x1a)},
                {word(0x36),word(0x34),word(0x3a)}};
            expected=VoiceOutputWork::evaluate(output,word(0),level,spatial,pan);
            if(!expected) throw std::runtime_error("Invalid native voice output inputs");
            frozen+=output.spatial.pan==65535;scaled+=tone!=0;
            movingPan+=expected->output.spatial.pan!=output.spatial.pan;
            movingSends+=expected->output.spatial.effects!=output.spatial.effects;
        }
        if(expected && cpu.cp==0 && (cpu.pc==0x335e || cpu.pc==0x3363)) {
            const auto& value=expected->output;
            if(value.tva.ramp!=word(6) || value.tva.level!=word(0x18) || value.tva.command!=word(0x1a)
                || value.spatial.pan!=word(0x36) || value.spatial.panWord!=word(0x34)
                || value.spatial.effects!=word(0x3a))
                throw std::runtime_error("Complete native voice output differs from H8");
            if(instructions!=expected->instructions) {
                std::fprintf(stderr,"[DEBUG-voice-output-work] instructions=%u/%u pan=%u effects=%04x\n",
                    instructions,expected->instructions,value.spatial.pan,value.spatial.effects);
                throw std::runtime_error("Complete native voice output work differs from H8");
            }
            ++checks;++budgets[instructions];expected.reset();
        }
        if(expected && !(cpu.cp==0 && cpu.pc<0x752)) ++instructions;
    }
    void report() const
    {
        std::printf("[DEBUG-voice-output] %llu full value/work comparisons PASS, budgets=%zu frozen=%llu scaled=%llu movingPan=%llu movingSends=%llu\n",
            (unsigned long long)checks,budgets.size(),(unsigned long long)frozen,(unsigned long long)scaled,
            (unsigned long long)movingPan,(unsigned long long)movingSends);
    }
};
