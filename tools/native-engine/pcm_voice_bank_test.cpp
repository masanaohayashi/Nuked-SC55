#include "pcm.h"
#include <cstdio>
#include <memory>

// Run with -fsanitize=bounds as well: ASan alone cannot detect an invalid
// subarray access that still lies inside the large pcm_t allocation.
int main()
{
    auto pcm=std::make_unique<pcm_t>();
    pcm->native_voice_count=128;
    for(unsigned slot=0;slot<128;++slot) {
        pcm->effects_dirty=false;
        PCM_SetVoicePitch(*pcm,slot,uint16_t(0x1000+slot));
        if(pcm->voiceRam2(slot)[0]!=0x1000+slot || pcm->effects_dirty) return 1;
        sc55::VoiceRenderUpdate update{};
        update.phaseIncrement=uint16_t(0x2000+slot);
        PCM_ApplyVoiceUpdate(*pcm,slot,update);
        if(pcm->voiceRam2(slot)[0]!=0x2000+slot || pcm->effects_dirty) return 2;
        for(unsigned effect=28;effect<32;++effect)
            if(pcm->ram2[effect][0]) return 3;
    }
    for(unsigned slot=0;slot<128;++slot)
        if(pcm->voiceRam2(slot)[0]!=0x2000+slot) return 4;
    std::puts("PASS: 128 logical pitch banks stay isolated from hardware effects");
}
