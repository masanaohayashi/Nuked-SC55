#pragma once

inline void VerifyDirectEffectTransitions(const sc55::EffectsTables& tables)
{
    auto old=std::make_unique<pcm_t>(),direct=std::make_unique<pcm_t>();
    auto oldFx=std::make_unique<PCMEffects>(),directFx=std::make_unique<PCMEffects>();
    for(auto* pcm:{old.get(),direct.get()}) {
        pcm->is_mk1=true; pcm->config.reg_slots=24; pcm->nfs=true;
        pcm->use_float_effects=true; PCM_UseSimulation(*pcm,true);
    }
    old->effects=oldFx.get(); direct->effects=directFx.get();
    const auto word=[&](unsigned bank,unsigned reg,unsigned value) {
        PCM_Write(*old,0x3e,uint8_t(bank)); PCM_Write(*old,reg,uint8_t(value>>8));
        PCM_Write(*old,reg+1,uint8_t(value));
    };
    const auto address=[&](unsigned bank,unsigned reg,unsigned value) {
        PCM_Write(*old,0x3e,uint8_t(bank)); PCM_Write(*old,reg+1,uint8_t(value>>16));
        PCM_Write(*old,reg+2,uint8_t(value>>8)); PCM_Write(*old,reg+3,uint8_t(value));
    };
    const auto read=[&](unsigned bank,unsigned reg) {
        PCM_Write(*old,0x3e,uint8_t(bank)); PCM_Read(*old,reg+(reg<0x10?1:0));
        const unsigned high=reg<0x10?PCM_Read(*old,0x39):0;
        const unsigned mid=PCM_Read(*old,0x3a);
        return (high<<16)|(mid<<8)|PCM_Read(*old,0x3b);
    };
    struct Prepared {
        pcm_t& pcm;
        void configureReverb(const sc55::ReverbSetup& s) const { PCM_ConfigureReverb(pcm,s); }
        void configureChorus(const sc55::ChorusSetup& s) const { PCM_ConfigureChorus(pcm,s); }
        void updateEffect(sc55::EffectParameter p,uint16_t v) const { PCM_UpdateEffect(pcm,p,v); }
        void setChorusMix(const std::array<uint8_t,3>& m) const { PCM_SetChorusMix(pcm,m); }
        void beginReverbDrain() const { PCM_BeginReverbDrain(pcm); }
    } prepared{*direct};
    sc55::EffectsControl expected,actual;
    std::array<uint8_t,6> reverb{};
    std::array<uint8_t,7> chorus{};
    uint64_t audibleFrames=0;
    for(unsigned tick=0;tick<1600;++tick) {
        if(tick%200==0) {
            reverb=tables.reverbMacros[tick/200]; chorus=tables.chorusMacros[tick/200];
            for(auto* c:{&expected,&actual}) { c->reverb.request(reverb); c->chorus.request(chorus); }
        }
        if(tick%200==120) {
            reverb[1]=uint8_t((tick/200+3)%8); reverb[2]=17; reverb[3]=91;
            chorus[0]=uint8_t((tick/200+5)%8); chorus[1]=25; chorus[2]=99; chorus[6]=47;
            for(auto* c:{&expected,&actual}) { c->reverb.request(reverb); c->chorus.request(chorus); }
        }
        const auto advance=[&](auto& c,auto& write,auto& load,auto& addr) {
            if(c.reverb.phase && !c.reverb.advanceDrain(write) && !c.reverb.advanceSetup(tables,write)
                && !c.reverb.advanceCoefficients(tables,reverb[0],write))
                throw std::runtime_error("Reverb transition stalled");
            if(c.chorus.phase && !c.chorus.advanceDrain(write) && !c.chorus.advanceSetup(tables,load,write,addr)
                && !c.chorus.advanceCoefficients(tables,write))
                throw std::runtime_error("Chorus transition stalled");
        };
        advance(expected,word,read,address);
        if(!actual.advance(tables,reverb[0],prepared)) throw std::runtime_error("Native effects stalled");
        if(expected.reverb.phase!=actual.reverb.phase || expected.reverb.dirty!=actual.reverb.dirty
            || expected.chorus.phase!=actual.chorus.phase || expected.chorus.dirty!=actual.chorus.dirty
            || std::memcmp(old->ram2,direct->ram2,sizeof(old->ram2))
            || old->select_channel!=direct->select_channel || old->write_latch!=direct->write_latch
            || old->read_latch!=direct->read_latch)
            throw std::runtime_error("Effect control transitions differ");
        old->rcsum[0]=direct->rcsum[0]=12345;
        old->rcsum[1]=direct->rcsum[1]=8765;
        for(unsigned frame=0;frame<128;++frame) {
            PCM_Update(*old,old->cycles+625); PCM_Update(*direct,direct->cycles+625);
            audibleFrames+=old->accum_l!=0 || old->accum_r!=0;
            if(old->accum_l!=direct->accum_l || old->accum_r!=direct->accum_r)
                throw std::runtime_error("Effect transition audio differs");
        }
        if(std::memcmp(oldFx->delay,directFx->delay,sizeof(oldFx->delay)))
            throw std::runtime_error("Effect transition histories differ");
    }
    if(!audibleFrames) throw std::runtime_error("Effect transition comparison was silent");
    std::puts("Effect transitions: all 8 macros, fades, drain, setup and differential updates match; no native register fallback");
}
