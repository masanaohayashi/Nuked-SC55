#include "sc55_chorus_oscillator.h"
#include "chorus-reference.h"
#include <cstdio>

int main()
{
    uint32_t random=713;
    auto next=[&] { random=random*1664525u+1013904223u; return random; };
    for(unsigned test=0;test<262144;++test) {
        sc55::ChorusOscillator oscillator;
        oscillator.begin=next()&0xfffff; oscillator.end=next()&0xfffff;
        const uint32_t positions[]{next()&0xfffff,oscillator.begin,oscillator.end,
            (oscillator.begin-1)&0xfffff,(oscillator.end+1)&0xfffff};
        oscillator.position=positions[test%5];
        const auto phaseWord=uint16_t(next()>>8);
        oscillator.phase=phaseWord&0x3fff;
        oscillator.descending=(phaseWord&0x8000)!=0;
        oscillator.increment=uint16_t(next()>>8);
        oscillator.pingPong=(test&1)!=0; oscillator.reverse=(test&2)!=0;
        const bool active=(test&4)!=0,update=(test&8)!=0;
        ChorusReferenceState reference;
        reference.nfs=update;
        reference.ram1[31][0]=oscillator.end;
        reference.ram1[31][2]=oscillator.begin;
        reference.ram1[31][4]=oscillator.position;
        reference.ram2[31][8]=phaseWord;
        reference.ram2[31][0]=oscillator.increment;
        reference.ram2[31][7]=uint16_t(31|(active?0x20:0)
            |(oscillator.pingPong?0x40:0)|(oscillator.reverse?0x80:0));
        for(unsigned frame=0;frame<8;++frame) {
            referenceChorusAdvance(reference);
            oscillator.advance(update,active);
            const auto phase=uint16_t((phaseWord&0x4000)|oscillator.phase
                |(oscillator.descending?0x8000:0));
            if(oscillator.position!=reference.ram1[31][4] || phase!=reference.ram2[31][8]
                || oscillator.leftTap()!=reference.ram2[29][10]
                || oscillator.rightTap()!=reference.ram2[29][11]) {
                std::printf("Chorus mismatch case=%u frame=%u\n",test,frame); return 1;
            }
        }
    }
    std::puts("Independent chorus oscillator: 2097152 exact phase/position/tap comparisons passed");
}
