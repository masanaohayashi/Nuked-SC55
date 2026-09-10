#include "pcm_effects.h"
#include "effects-reference.h"
#include "chorus-reference.h"
#include "envelope-reference.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

int main(int argc,char**)
{
    const bool owned=argc>1;
    auto fx=std::make_unique<PCMEffects>();
    auto reference=std::make_unique<effects_reference::PCMEffects>();
    effects_reference::pcm_t registers;
    ChorusReferenceState movement;
    uint32_t random=1729;
    auto next=[&] { random=random*1664525u+1013904223u; return uint16_t(random>>16); };
    double energy=0;
    for(unsigned frame=0;frame<65536;++frame) {
        if(frame%257==0) {
            for(unsigned bank=28;bank<32;++bank)
                for(auto& word:registers.ram2[bank]) word=next();
            const auto coefficient=[](uint16_t value) {
                return PCMEffectsSettings::Coefficient{
                    float(int8_t(value>>8))/32,float(int8_t(value>>8))/64,
                    float(int8_t(value&255))/32,bool(value&0x30)};
            };
            auto& settings=fx->settings;
            std::copy_n(registers.ram2[28],12,settings.diffusionTaps.begin());
            std::copy_n(registers.ram2[29],12,settings.tailTaps.begin());
            settings.reverbInput=coefficient(registers.ram2[30][1]);
            settings.chorusInput=coefficient(registers.ram2[31][1]);
            settings.comb=coefficient(registers.ram2[30][6]);
            for(unsigned i=0;i<2;++i) {
                settings.diffusion[i]=coefficient(registers.ram2[30][4+i]);
                settings.damping[i]=coefficient(registers.ram2[30][7+i]);
                settings.reverbReturn[i]=coefficient(registers.ram2[30][2+i]);
            }
            for(unsigned i=0;i<4;++i) settings.chorusReturn[i]=coefficient(registers.ram2[31][2+i]);
            if(owned) {
                movement.ram1[31][0]=next(); movement.ram1[31][2]=next();
                movement.ram1[31][4]=next();
                movement.ram2[31][0]=next(); movement.ram2[31][8]=next();
                movement.ram2[31][7]=uint16_t(31|(next()&0xe0));
                const auto word=movement.ram2[31][8],flags=movement.ram2[31][7];
                fx->chorus={movement.ram1[31][4],movement.ram1[31][2],movement.ram1[31][0],
                    uint16_t(word&0x3fff),movement.ram2[31][0],bool(word&0x8000),bool(flags&0x40),bool(flags&0x80)};
                fx->phaseMarker=(word&0x4000)!=0;
                fx->spread={registers.ram2[30][0],registers.ram2[30][9]};
                fx->interpolation={registers.ram2[31][9],registers.ram2[31][10]};
                fx->leftTap=registers.ram2[29][10]; fx->rightTap=registers.ram2[29][11];
            }
        }
        const auto phase=uint16_t((0x3fff-frame)&0x3fff);
        const bool update=frame%17!=0,active=(movement.ram2[31][7]&0x20)!=0;
        if(owned) {
            const auto word=movement.ram2[31][8];
            if(word&0x8000) registers.ram2[31][9]=word&0x7fff;
            else registers.ram2[31][10]=word&0x7fff;
            if((0x4000-word)&0x8000) registers.ram2[31][10]=(0x4000-word)&0x7fff;
            else registers.ram2[31][9]=(0x4000-word)&0x7fff;
            ReferenceEnvelopeClock clock{phase,update}; int ignored=0;
            referenceRamp(clock,1,registers.ram2[30][0],&registers.ram2[30][9],active,&ignored);
        } else {
            registers.ram2[29][10]=next(); registers.ram2[29][11]=next();
            registers.ram2[31][9]=next(); registers.ram2[31][10]=next();
            registers.ram2[30][9]=next();
        }
        const PCMEffectsModulation heads{registers.ram2[29][10],registers.ram2[29][11],
            float(int8_t(registers.ram2[31][9]>>8))/32,
            float(int8_t(registers.ram2[31][10]>>8))/32,
            float(int8_t(registers.ram2[30][9]>>8))/32};
        const float reverb=frame%313==0?1000:0,chorus=frame%127==0?-300:0;
        float a[6]{},b[6]{},c[6]{},d[6]{};
        if(owned) fx->process({phase,update},active,reverb,chorus,a,b);
        else PCMEffects_Step(*fx,phase,heads,reverb,chorus,a,b);
        effects_reference::PCMEffects_Step(*reference,registers,phase,reverb,chorus,c,d);
        if(owned) {
            movement.nfs=update; referenceChorusAdvance(movement);
            registers.ram2[29][10]=movement.ram2[29][10];
            registers.ram2[29][11]=movement.ram2[29][11];
            const auto word=uint16_t(fx->chorus.phase|(fx->chorus.descending?0x8000:0)|(fx->phaseMarker?0x4000:0));
            if(fx->chorus.position!=movement.ram1[31][4] || word!=movement.ram2[31][8]
                || fx->spread.level!=registers.ram2[30][9]
                || fx->leftTap!=registers.ram2[29][10] || fx->rightTap!=registers.ram2[29][11]) return 5;
        }
        for(unsigned i=0;i<6;++i) {
            if(!std::isfinite(a[i]) || !std::isfinite(b[i]) || a[i]!=c[i] || b[i]!=d[i]) {
                std::printf("Effects mismatch frame=%u bus=%u\n",frame,i); return 1;
            }
            energy+=double(a[i])*a[i]+double(b[i])*b[i];
        }
        if(frame%512==0 && !std::equal(std::begin(fx->delay),std::end(fx->delay),std::begin(reference->delay))) return 2;
    }
    if(energy==0) return 3;
    fx->reset();
    if(std::any_of(std::begin(fx->delay),std::end(fx->delay),[](float value) { return value!=0; })) return 4;
    std::printf("Independent effects (%s): 65536 frames, changing coefficients/taps, exact outputs and delay state\n",
        owned?"owned modulation and ramps":"external modulation");
}
