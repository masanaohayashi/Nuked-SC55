#pragma once

inline void VerifyDirectReverbSetup(const sc55::EffectsTables& tables)
{
    auto old=std::make_unique<pcm_t>(),direct=std::make_unique<pcm_t>();
    auto oldFx=std::make_unique<PCMEffects>(),directFx=std::make_unique<PCMEffects>();
    for(auto* pcm:{old.get(),direct.get()}) {
        pcm->is_mk1=true; pcm->config.reg_slots=24; pcm->nfs=true;
        pcm->ram2[30][1]=0x0020;
        pcm->use_float_effects=true; PCM_UseSimulation(*pcm,true);
    }
    old->effects=oldFx.get(); direct->effects=directFx.get();
    const auto word=[&](unsigned bank,unsigned reg,unsigned value) {
        PCM_Write(*old,0x3e,uint8_t(bank));
        PCM_Write(*old,reg,uint8_t(value>>8)); PCM_Write(*old,reg+1,uint8_t(value));
    };
    struct Prepared {
        pcm_t& pcm;
        void configureReverb(const sc55::ReverbSetup& setup) const noexcept
        { PCM_ConfigureReverb(pcm,setup); }
        void operator()(unsigned bank,unsigned reg,unsigned value) const noexcept {
            PCM_Write(pcm,0x3e,uint8_t(bank)); PCM_Write(pcm,reg,uint8_t(value>>8));
            PCM_Write(pcm,reg+1,uint8_t(value));
        }
    };
    uint64_t audibleFrames=0;
    for(unsigned character=0;character<8;++character) for(uint8_t time:{0,1,64,127}) {
        sc55::EffectsControl::Reverb legacy;
        legacy.phase=8;
        legacy.parameters={uint8_t(character),uint8_t(character),127,time,uint8_t(127-time),63};
        auto native=legacy;
        if(!legacy.advanceSetup(tables,word) || !native.advanceSetup(tables,Prepared{*direct})
            || native.phase!=legacy.phase || native.lpf!=legacy.lpf || native.output!=legacy.output)
            throw std::runtime_error("Reverb setup controller differs");
        if(std::memcmp(old->ram2,direct->ram2,sizeof(old->ram2))
            || old->write_latch!=direct->write_latch || old->select_channel!=direct->select_channel)
            throw std::runtime_error("Direct reverb setup differs from byte transaction");
        old->rcsum[0]=direct->rcsum[0]=12345;
        for(unsigned frame=0;frame<1024;++frame) {
            PCM_Update(*old,old->cycles+625); PCM_Update(*direct,direct->cycles+625);
            audibleFrames+=old->accum_l!=0 || old->accum_r!=0;
            if(std::memcmp(old->ram2,direct->ram2,sizeof(old->ram2))
                || std::memcmp(oldFx->delay,directFx->delay,sizeof(oldFx->delay))
                || old->accum_l!=direct->accum_l || old->accum_r!=direct->accum_r)
                throw std::runtime_error("Direct reverb processing diverged");
        }
    }
    if(!audibleFrames) throw std::runtime_error("Reverb setup comparison did not exercise audible tails");
    std::puts("Direct reverb setup: all 8 characters, 4 time values, preserved tails match byte path");
}
