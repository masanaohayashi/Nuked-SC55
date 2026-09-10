#pragma once

inline void VerifyDirectVoiceStop()
{
    {
        auto pcm=std::make_unique<pcm_t>();
        auto signal=std::make_unique<sc55::SignalRenderer>();
        pcm->native_signal=signal.get(); pcm->sim_dirty=0;
        signal->voices.voice_count=24;
        signal->voices.envelopes[3].ramps[0].level=123;
        signal->voices.envelopes[3].ramps[1].level=456;
        pcm->native_readback_pending=true;
        pcm->ram2[3][9]=17; pcm->ram2[3][10]=29;
        if(PCM_PeekVoiceGainLevels(*pcm,3)!=std::array<uint16_t,2>{123,456}
            || PCM_VoiceRampLevel(*pcm,3,sc55::EnvelopeRamp::Stage::firstGain)!=123
            || !pcm->native_readback_pending || pcm->ram2[3][9]!=17)
            throw std::runtime_error("Owned gain readback materialized stale compatibility state");
        PCM_Write(*pcm,0x3e,3); PCM_Write(*pcm,0x32,0x12); PCM_Write(*pcm,0x33,0x34);
        if(PCM_PeekVoiceGainLevels(*pcm,3)!=std::array<uint16_t,2>{0x1234,456}
            || PCM_VoiceRampLevel(*pcm,3,sc55::EnvelopeRamp::Stage::firstGain)!=0x1234)
            throw std::runtime_error("Owned gain readback ignored a pending control write");
    }
    auto old=std::make_unique<pcm_t>(),direct=std::make_unique<pcm_t>();
    const auto read=[&](uint8_t a) { return PCM_Read(*old,a); };
    const auto write=[&](uint8_t a,uint8_t v) { PCM_Write(*old,a,v); };
    struct Target {
        pcm_t& pcm;
        auto voiceGainLevels(uint8_t channel) const { return PCM_VoiceGainLevels(pcm,channel); }
        uint16_t voiceRampLevel(uint8_t channel,sc55::EnvelopeRamp::Stage stage) const
        { return PCM_VoiceRampLevel(pcm,channel,stage); }
        void setVoiceRamp(uint8_t channel,sc55::EnvelopeRamp::Stage stage,uint16_t command) const
        { PCM_SetVoiceRamp(pcm,channel,stage,command); }
    } target{*direct};
    for(unsigned slot=0;slot<24;++slot) for(unsigned scenario=0;scenario<4;++scenario) {
        const uint16_t first=scenario==0?0:scenario==1?12000:32000;
        const uint16_t second=scenario==0?0:scenario==2?12000:32000;
        old->ram2[slot][9]=direct->ram2[slot][9]=first;
        old->ram2[slot][10]=direct->ram2[slot][10]=second;
        const auto expected=sc55::StopVoicePcm(uint8_t(slot),read,write);
        const auto actual=sc55::StopVoicePcm(uint8_t(slot),nullptr,target);
        if(!expected || !actual || expected->stageCode!=actual->stageCode
            || expected->cachedWordOffset!=actual->cachedWordOffset)
            throw std::runtime_error("Direct stop selected a different ramp");
        for(uint16_t stage:{14,16}) {
            uint16_t expectedStage=stage,actualStage=stage;
            uint8_t expectedActivity=0,actualActivity=0;
            sc55::VoiceLinks expectedLinks,actualLinks;
            expectedLinks.first[slot]=actualLinks.first[slot]=uint8_t((slot+1)%24);
            const auto a=sc55::PollEnvelopeTermination(slot,expectedStage,expectedActivity,expectedLinks,read,write);
            const auto b=sc55::PollEnvelopeTermination(slot,actualStage,actualActivity,actualLinks,nullptr,target);
            if(a!=b || expectedStage!=actualStage || expectedActivity!=actualActivity
                || expectedLinks.first!=actualLinks.first || expectedLinks.second!=actualLinks.second)
                throw std::runtime_error("Direct termination changed allocator notification");
        }
        if(std::memcmp(old->ram2,direct->ram2,sizeof(old->ram2))
            || old->write_latch!=direct->write_latch || old->read_latch!=direct->read_latch
            || old->select_channel!=direct->select_channel)
            throw std::runtime_error("Direct stop/termination differs from reference state");
    }
    std::puts("Direct voice stop: 24 slots, quieter/equal/silent gain cases and termination/link notification match without byte I/O");
}
