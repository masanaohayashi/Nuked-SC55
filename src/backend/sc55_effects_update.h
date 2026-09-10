#pragma once
#include <array>
#include <cstdint>
#include "sc55_effect_parameter.h"

namespace sc55
{
// Compatibility fallback for oracle callers. Native controllers submit the
// semantic operation; device bank/address knowledge stays in this adapter.
template<class Writer>
void PublishEffectUpdate(Writer& write,EffectParameter parameter,uint16_t value) noexcept
{
    if constexpr(requires { write.updateEffect(parameter,value); })
        write.updateEffect(parameter,value);
    else switch(parameter) {
    case EffectParameter::reverbInput: write(30,0x12,value); break;
    case EffectParameter::reverbOutput: write(30,0x14,value); write(30,0x16,value); break;
    case EffectParameter::reverbSpread: write(30,0x10,value); break;
    case EffectParameter::chorusInput: write(31,0x12,value); break;
    case EffectParameter::chorusLevel: write(31,0x14,value); write(31,0x1a,value&0xff00); break;
    case EffectParameter::chorusFeedback: write(31,0x16,value); break;
    case EffectParameter::chorusSend: write(31,0x14,value); break;
    }
}

template<class Writer>
void PublishChorusMix(Writer& write,const std::array<uint8_t,3>& mix) noexcept
{
    if constexpr(requires { write.setChorusMix(mix); }) write.setChorusMix(mix);
    else {
        write(31,0x14,uint16_t((mix[0]<<8)|mix[2]));
        write(31,0x16,mix[1]); write(31,0x18,0);
        write(31,0x1a,uint16_t(mix[0]<<8));
    }
}
}
