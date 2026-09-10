#pragma once
#include "sc55_sysex.h"
#include "sc55_effects_control.h"
#include "sc55_voice_lifecycle.h"

namespace sc55
{
// Audio-owned global configuration. Parsing mutates values and returns semantic
// requests; it never stops voices, programs PCM or invokes the engine.
struct SystemSettings
{
    MasterControls master;
    EffectsSettings effects;
    VoiceCapacityPolicy capacity;
    std::array<uint8_t,16> name{};
    struct Changes {
        unsigned effects=0;
        bool reset=false,unsupported=false,invalidLength=false;
    };
    Changes write(std::span<const uint8_t> payload,const EffectsTables* tables) noexcept
    {
        Changes changes;
        if(payload.size()<3 || payload[0]!=0x40 || payload[1]!=1) {
            const auto result=master.write(payload);
            changes.reset=result==MasterControls::WriteResult::resetRequested;
            changes.unsupported=result==MasterControls::WriteResult::unsupported;
            changes.invalidLength=result==MasterControls::WriteResult::invalidLength;
            return changes;
        }
        const auto address=payload[2];
        if(address==0) {
            if(payload.size()!=19) changes.invalidLength=true;
            else for(unsigned i=0;i<16;++i) name[i]=std::max<uint8_t>(32,payload[i+3]);
            return changes;
        }
        if(address==0x10) {
            unsigned total=0;
            for(auto value:payload.subspan(3)) total+=value;
            if(payload.size()!=19 || total>24) changes.invalidLength=true;
            else std::copy_n(payload.begin()+3,16,capacity.reserves.begin());
            return changes;
        }
        if(address==0x20) {
            if(payload.size()<4) {changes.invalidLength=true;return changes;}
            capacity.startPartControl=std::min<uint8_t>(payload[3],15);
            if(payload.size()==4 || !tables) return changes;
        }
        if(!tables) {changes.unsupported=true;return changes;}
        // The table successor of start-part20 is reverb macro30. Preserve an
        // applied prefix even if a later effect record is unsupported.
        const auto result=address==0x20 ? effects.writeValues(0x30,payload.subspan(4),*tables)
                                       : effects.write(payload,*tables);
        changes.effects=result.requests;
        changes.unsupported=result.unsupported;
        changes.invalidLength=result.invalidLength;
        return changes;
    }
};
}
