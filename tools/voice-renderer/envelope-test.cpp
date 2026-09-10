#include "sc55_envelope_ramp.h"
#include "envelope-reference.h"
#include <cstdio>

int main()
{
    using Stage=sc55::EnvelopeRamp::Stage;
    for(unsigned held=0;held<8;++held) {
        sc55::VoiceEnvelopes envelopes;
        std::array<uint16_t,3> commands{},saved{0x1235,0x4567,0xffff};
        for(unsigned i=0;i<3;++i) {
            commands[i]=(held&(1u<<i)) ? 0xff00 : 0x7fba;
            envelopes.ramps[i]={uint16_t(0x2000+i),uint16_t(0x1000+i)};
        }
        const auto levels=envelopes.synchronize(commands,saved);
        for(unsigned i=0;i<3;++i) {
            const bool restore=(held&(1u<<i))!=0;
            if(levels[i]!=(restore?saved[i]:uint16_t((0x1000+i)<<1))
                || envelopes.ramps[i].level!=(restore?uint16_t(saved[i]>>1):uint16_t(0x1000+i))
                || envelopes.ramps[i].command!=(restore?uint16_t(0x2000+i):uint16_t(0xff00)))
                return 2;
        }
    }
    unsigned long long checks=0;
    for(unsigned command=0;command<65536;++command)
        for(uint16_t phase:{0,1,3,4,15,16,63,64,127,128,255,1023,4095,16383})
            for(uint16_t level:{0,1,0x1000,0x3fff,0x7fff,0x8000,0xffff})
                for(int stage=0;stage<3;++stage)
                    for(bool active:{false,true}) for(bool update:{false,true}) {
                        ReferenceEnvelopeClock clock{phase,update};
                        uint16_t expected=level; int gain=0;
                        referenceRamp(clock,stage,int(command),&expected,active,&gain);
                        sc55::EnvelopeRamp ramp{uint16_t(command),level};
                        const auto actual=ramp.advance(Stage(stage),{phase,update},active);
                        if(ramp.level!=expected || (stage<2 && actual!=gain)) {
                            std::printf("Ramp mismatch command=%04x phase=%u level=%04x stage=%d active=%d update=%d readback=%04x/%04x gain=%u/%d\n",
                                command,phase,level,stage,active,update,ramp.level,expected,actual,gain);
                            return 1;
                        }
                        ++checks;
                    }
    std::printf("Independent envelope ramps: %llu exact reference comparisons passed\n",checks);
}
