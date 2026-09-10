#pragma once

inline void VerifyDirectChorusSetup()
{
    auto old=std::make_unique<pcm_t>(),direct=std::make_unique<pcm_t>();
    auto oldFx=std::make_unique<PCMEffects>(),directFx=std::make_unique<PCMEffects>();
    for(auto* pcm:{old.get(),direct.get()}) {
        pcm->is_mk1=true; pcm->config.reg_slots=24; pcm->nfs=true;
        pcm->use_float_effects=true; PCM_UseSimulation(*pcm,true);
    }
    old->effects=oldFx.get(); direct->effects=directFx.get();
    const auto word=[&](unsigned bank,unsigned reg,unsigned value) {
        PCM_Write(*old,0x3e,uint8_t(bank));
        PCM_Write(*old,reg,uint8_t(value>>8)); PCM_Write(*old,reg+1,uint8_t(value));
    };
    const auto address=[&](unsigned bank,unsigned reg,unsigned value) {
        PCM_Write(*old,0x3e,uint8_t(bank)); PCM_Write(*old,reg+1,uint8_t(value>>16));
        PCM_Write(*old,reg+2,uint8_t(value>>8)); PCM_Write(*old,reg+3,uint8_t(value));
    };
    const auto read=[&](unsigned bank,unsigned reg) {
        PCM_Write(*old,0x3e,uint8_t(bank));
        PCM_Read(*old,reg+(reg<0x10?1:0));
        const unsigned high=reg<0x10?PCM_Read(*old,0x39):0;
        const unsigned mid=PCM_Read(*old,0x3a);
        return (high<<16)|(mid<<8)|PCM_Read(*old,0x3b);
    };
    sc55::EffectsTables tables;
    for(unsigned test=0;test<128;++test) {
        sc55::EffectsControl::Chorus legacy;
        legacy.phase=6;
        legacy.parameters={0,uint8_t(test),uint8_t(test/2),uint8_t(test),
            uint8_t(127-test),uint8_t((test*17)%128),uint8_t(test/2)};
        const auto& p=legacy.parameters;
        const unsigned base=0x3801+6u*p[3];
        const sc55::ChorusSetup setup{base,base+10u*p[5]+10,base+1,
            uint16_t((10u*p[5]*p[4])/8),
            {uint16_t((p[1]<<8)|p[6]),p[2],0,uint16_t(p[1]<<8)}};
        if(!legacy.advanceSetup(tables,read,word,address) || legacy.phase!=8)
            throw std::runtime_error("Legacy chorus setup did not complete");
        PCM_ConfigureChorus(*direct,setup);
        if(std::memcmp(old->ram1,direct->ram1,sizeof(old->ram1))
            || std::memcmp(old->ram2,direct->ram2,sizeof(old->ram2))
            || old->write_latch!=direct->write_latch || old->read_latch!=direct->read_latch)
            throw std::runtime_error("Direct chorus setup differs from byte transaction");
        for(unsigned frame=0;frame<32;++frame) {
            PCM_Update(*old,old->cycles+625); PCM_Update(*direct,direct->cycles+625);
            if(std::memcmp(old->ram2,direct->ram2,sizeof(old->ram2))
                || std::memcmp(oldFx->delay,directFx->delay,sizeof(oldFx->delay))
                || old->accum_l!=direct->accum_l || old->accum_r!=direct->accum_r)
                throw std::runtime_error("Direct chorus modulation diverged");
        }
    }
    std::puts("Direct chorus setup: 128 parameter sets match byte transactions and subsequent effect frames");
}
