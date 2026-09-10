#pragma once
#include <array>
#include <cstdint>
#include <span>
#include "sc55_chorus_setup.h"
#include "sc55_reverb_setup.h"
#include "sc55_effects_update.h"

namespace sc55
{
struct EffectsTables
{
    std::array<uint16_t,8> reverbLpf{}, chorusLpf{};
    std::array<std::array<uint16_t,26>,8> reverbPrograms{};
    std::array<std::array<uint8_t,6>,8> reverbMacros{};
    std::array<std::array<uint8_t,7>,8> chorusMacros{};
};
struct EffectsSettings
{
    std::array<uint8_t,6> reverb{};
    std::array<uint8_t,7> chorus{};
    uint8_t reverbMacro=0,chorusMacro=0;
    struct Result { unsigned requests=0; bool unsupported=false, invalidLength=false; };
    Result write(std::span<const uint8_t> payload,const EffectsTables& tables) noexcept
    {
        Result result;
        if (payload.size()<3 || payload[0]!=0x40 || payload[1]!=1)
        { result.unsupported=true; return result; }
        if (payload.size()==3) { result.invalidLength=true; return result; }
        return writeValues(payload[2],payload.subspan(3),tables);
    }
    // Also accepts the suffix of a system-table transaction, without copying
    // or fabricating a second MIDI packet on the audio thread.
    Result writeValues(unsigned address,std::span<const uint8_t> values,const EffectsTables& tables) noexcept
    {
        Result result;
        for (auto value:values)
        {
            if (address==0x30) {
                reverbMacro=value>7 ? 7 : value;
                reverb=tables.reverbMacros[reverbMacro]; result.requests|=1;
            } else if (address>=0x31 && address<=0x36) {
                const auto maximum=address<=0x32 ? 7u : 127u;
                reverb[address-0x31]=uint8_t(value>maximum ? maximum : value); result.requests|=1;
            } else if (address==0x38) {
                chorusMacro=value>7 ? 7 : value;
                chorus=tables.chorusMacros[chorusMacro]; result.requests|=2;
            } else if (address>=0x39 && address<=0x3f) {
                const auto maximum=address==0x39 ? 7u : 127u;
                chorus[address-0x39]=uint8_t(value>maximum ? maximum : value); result.requests|=2;
            } else { result.unsupported=true; break; }
            // Continuation follows the parameter table, not numeric address
            // increments. There is no37 entry between pre-delay and chorus.
            address=address==0x36 ? 0x38 : address+1;
        }
        return result;
    }
};
// Audio-owned request coalescing for the PCM effects controller. Values here
// are normalized control values, not PCM coefficients. Request acceptance is
// separate from periodic fades/tap programming; no H8 or PCM access is needed.
struct EffectsControl
{
    struct Reverb
    {
        // character, pre-LPF, level, time, feedback, pre-delay/2 (CB51..56)
        std::array<uint8_t,6> parameters{};
        uint16_t phase = 0;
        uint8_t dirty = 0;
        uint16_t output = 0; // CB5E: level, pre-delay
        uint16_t lpf = 0; // CB60
        uint16_t lpfTarget = 0; // CB62
        uint8_t drainTicks = 0;

        template<class Writer>
        bool advanceSetup(const EffectsTables& tables,Writer&& write) noexcept
        {
            if (phase!=8 || parameters[0]>=8 || parameters[1]>=8) return false;
            output=uint16_t((parameters[2]<<8)|parameters[5]);
            const auto& program=tables.reverbPrograms[parameters[0]];
            if constexpr(requires { write.configureReverb(ReverbSetup{}); }) {
                ReverbSetup setup;
                for(unsigned i=0;i<12;++i) setup.diffusionTaps[i]=program[i];
                for(unsigned i=0;i<9;++i) setup.tailTaps[i]=program[12+i];
                setup.diffusion={program[21],program[22]}; setup.comb=program[23];
                setup.damping={program[24],program[25]};
                setup.output=output; setup.spreadCommand=uint16_t((decay()<<8)|0xba);
                setup.delayProgram=parameters[0]>=6;
                if(setup.delayProgram) {
                    const auto right=uint16_t(112u*parameters[3]+22);
                    const auto left=uint16_t((parameters[0]==6 ? 112u : 56u)*parameters[3]+22);
                    setup.diffusionTaps[6]=setup.diffusionTaps[10]=left;
                    setup.diffusionTaps[7]=setup.diffusionTaps[11]=right;
                    setup.tailTaps[2]=setup.tailTaps[6]=left;
                    setup.tailTaps[3]=setup.tailTaps[7]=setup.tailTaps[8]=right;
                }
                write.configureReverb(setup);
                lpfTarget=tables.reverbLpf[parameters[1]]; lpf=lpfTarget&0xff00;
                phase=10;
                return true;
            }
            else {
            constexpr std::array<unsigned,12> registers{
                0x10,0x12,0x14,0x16,0x18,0x1a,0x1c,0x1e,0x30,0x32,0x34,0x36};
            for (unsigned i=0;i<12;++i) write(28,registers[i],program[i]);
            for (unsigned i=0;i<9;++i) write(29,registers[i],program[12+i]);
            for (unsigned i=0;i<5;++i) write(30,registers[4+i],program[21+i]);
            PublishEffectUpdate(write,EffectParameter::reverbOutput,output);
            PublishEffectUpdate(write,EffectParameter::reverbSpread,uint16_t((decay()<<8)|0xba));
            lpfTarget=tables.reverbLpf[parameters[1]]; lpf=lpfTarget&0xff00;
            if (parameters[0]>=6)
            {
                const unsigned right=112u*parameters[3]+22;
                const unsigned left=(parameters[0]==6 ? 112u : 56u)*parameters[3]+22;
                write(28,0x1c,left); write(28,0x1e,right);
                write(28,0x34,left); write(28,0x36,right);
                write(29,0x14,left); write(29,0x16,right);
                write(29,0x1c,left); write(29,0x1e,right); write(29,0x30,right);
            }
            phase=10;
            return true;
            }
        }

        unsigned decay() const noexcept
        {
            const unsigned value=parameters[0]<6 ? (454u*parameters[3])>>8
                : uint8_t(parameters[4]*2);
            return value>191 ? 191 : value;
        }

        void raiseLpf() noexcept
        {
            const auto low=uint8_t((lpf&255)+2);
            lpf=uint16_t((lpf&0xff00)|(low>(lpfTarget&255) ? lpfTarget&255 : low));
        }

        // currentCharacter is the live configuration byte802B, not the cached
        // request byteCB51: pending task8 events can leave these different.
        template<class Writer>
        bool advanceCoefficients(const EffectsTables& tables,uint8_t currentCharacter,Writer&& write) noexcept
        {
            if (phase==10)
            {
                PublishEffectUpdate(write,EffectParameter::reverbInput,lpf);
                if (lpf==lpfTarget) phase=0; else raiseLpf();
                return true;
            }
            if (phase!=12 || parameters[1]>=8) return false;
            if (dirty&1)
            {
                PublishEffectUpdate(write,EffectParameter::reverbInput,lpf);
                const auto low=lpf&255;
                if (!low) {
                    dirty=uint8_t((dirty&~1u)|0x20);
                    lpfTarget=tables.reverbLpf[parameters[1]]; lpf=lpfTarget&0xff00;
                } else lpf=uint16_t((lpf&0xff00)|(low>3 ? low-3 : 0));
            }
            const auto ramp=[&](uint8_t value,uint8_t target,unsigned bit) {
                bool done=value==target;
                if (value<target) {
                    done=unsigned(value)+2>=target;
                    value=done ? target : uint8_t(value+2);
                } else if (value>target) {
                    done=value<3 || value-3<target;
                    value=done ? target : uint8_t(value-3);
                }
                if (done) dirty &= uint8_t(~bit);
                return value;
            };
            if (dirty&2) {
                output=uint16_t((ramp(uint8_t(output>>8),parameters[2],2)<<8)|(output&255));
                PublishEffectUpdate(write,EffectParameter::reverbOutput,output);
            }
            if (dirty&4) {
                dirty &= uint8_t(~4u);
                if (currentCharacter<6) {
                    const unsigned value=454u*parameters[3];
                    PublishEffectUpdate(write,EffectParameter::reverbSpread,uint16_t(((value>0xbf00 ? 0xbf00 : value)&0xff00)|0x0a));
                }
            }
            if (dirty&8) {
                dirty &= uint8_t(~8u);
                if (currentCharacter>=6) {
                    const unsigned value=uint8_t(parameters[4]*2);
                    PublishEffectUpdate(write,EffectParameter::reverbSpread,uint16_t(((value>191 ? 191 : value)<<8)|0x0a));
                }
            }
            if (dirty&16) {
                output=uint16_t((output&0xff00)|ramp(uint8_t(output),parameters[5],16));
                PublishEffectUpdate(write,EffectParameter::reverbOutput,output);
            }
            if (dirty&32) {
                PublishEffectUpdate(write,EffectParameter::reverbInput,lpf);
                if (lpf==lpfTarget) dirty &= uint8_t(~32u); else raiseLpf();
            }
            if (!dirty) phase=0;
            return true;
        }

        // One periodic call, not a busy wait. Writer takes PCM bank, register,
        // word; the owner serializes these writes with voice/PCM service.
        template<class Writer>
        bool advanceDrain(Writer&& write) noexcept
        {
            if (phase==2)
            {
                const auto down=[](unsigned v) { return v>3 ? v-3 : 0; };
                output=uint16_t((down(output>>8)<<8)|down(output&255));
                PublishEffectUpdate(write,EffectParameter::reverbOutput,output);
                if (!output) phase=4;
                return true;
            }
            if (phase==4)
            {
                if constexpr(requires { write.beginReverbDrain(); }) {
                    write.beginReverbDrain();
                    lpf=0; drainTicks=33; phase=6;
                    return true;
                }
                else {
                write(30,0x10,0xb6);
                for (unsigned reg=0x12;reg<=0x1e;reg+=2) write(30,reg,0);
                write(30,0x30,0);
                constexpr std::array<unsigned,8> taps{0x10,0x12,0x14,0x16,0x18,0x1a,0x30,0x32};
                for (unsigned i=0;i<taps.size();++i) write(28,taps[i],i);
                write(29,0x10,0x2000); write(29,0x12,0x2001);
                write(29,0x18,0x2002); write(29,0x1a,0x2003);
                lpf=0; drainTicks=33; phase=6;
                return true;
                }
            }
            if (phase==6)
            {
                if (--drainTicks==0) phase=8;
                return true;
            }
            return false; // Idle or coefficient/setup phase, not serviced here.
        }

        // Input is the six raw configuration bytes at 802B..30.
        void request(std::array<uint8_t,6> next) noexcept
        {
            next[5] >>= 1;
            // Unlike chorus, even an in-progress differential update must
            // restart the full transition when another request arrives.
            if (next[0]!=parameters[0] || phase!=0)
            { parameters=next; phase=2; return; }
            phase=12;
            for (unsigned i=1;i<6;++i)
            {
                if (next[i]==parameters[i]) continue;
                parameters[i]=next[i];
                dirty |= uint8_t(1u<<(i-1));
                if (i==1) dirty &= uint8_t(~0x20u);
                // Delay characters rebuild taps when time changes. Preserve
                // dirty bits already accumulated before choosing full setup.
                if (i==3 && parameters[0]>=6)
                { parameters=next; phase=2; return; }
            }
        }
    } reverb;

    struct Chorus
    {
        // pre-LPF, level, feedback/2, delay, rate, depth, reverb-send/2
        std::array<uint8_t,7> parameters{}; // CB57..5D
        uint16_t phase = 0;
        uint8_t dirty = 0;
        std::array<uint8_t,3> output{}; // CB64..66: level, feedback, send
        uint16_t lpf = 0; // CB68
        uint16_t lpfTarget = 0; // CB6A
        uint8_t drainTicks = 0;
        enum class Setup { idle, mask, position, modulation };
        Setup setup = Setup::idle;

        // Phase6 (64B3..65AF). read returns a latched PCM register value;
        // writeAddress handles a20-bit address, write handles16-bit words.
        // At most three readback attempts per call. The owner advances PCM
        // and calls again on a mismatch, holding later control requests until
        // this transaction finishes (as task8 does inside the H8 routine).
        template<class Reader,class Writer,class AddressWriter>
        bool advanceSetup(const EffectsTables& tables,Reader&& read,
            Writer&& write,AddressWriter&& writeAddress) noexcept
        {
            if (phase!=6 || parameters[0]>=tables.chorusLpf.size()) return false;
            const unsigned base=0x3801+6u*parameters[3];
            if constexpr(requires { write.configureChorus(ChorusSetup{}); }) {
                output={parameters[1],parameters[2],parameters[6]};
                write.configureChorus(ChorusSetup{base,base+10u*parameters[5]+10,base+1,
                    uint16_t((10u*parameters[5]*parameters[4])/8),
                    {uint16_t((output[0]<<8)|output[2]),output[1],0,uint16_t(output[0]<<8)}});
                lpfTarget=tables.chorusLpf[parameters[0]]; lpf=lpfTarget&0xff00;
                phase=8; setup=Setup::idle;
                return true;
            }
            else {
            if (setup==Setup::idle)
            { write(29,0x32,0x3800); setup=Setup::mask; }
            if (setup==Setup::mask)
            {
                write(31,0x1e,0x7f);
                if ((read(31,0x1e)&255)!=0x7f) return true;
                writeAddress(31,0x08,base);
                writeAddress(31,0x0c,base+10u*parameters[5]+10);
                write(31,0x10,0);
                setup=Setup::position;
            }
            if (setup==Setup::position)
            {
                writeAddress(31,0x04,base+1);
                if (read(31,0x04)!=base+1) return true;
                setup=Setup::modulation;
            }
            write(31,0x30,0);
            if ((read(31,0x30)&255)!=0) return true;
            write(31,0x10,(10u*parameters[5]*parameters[4])/8);
            output={parameters[1],parameters[2],parameters[6]};
            write(31,0x14,uint16_t((output[0]<<8)|output[2]));
            write(31,0x16,output[1]); write(31,0x18,0);
            write(31,0x1a,uint16_t(output[0]<<8));
            lpfTarget=tables.chorusLpf[parameters[0]]; lpf=lpfTarget&0xff00;
            phase=8; setup=Setup::idle;
            return true;
            }
        }

        // Full-setup fade-in and differential updates (65B0..674C).
        // Writes precede LPF progression: reaching the target internally does
        // not mean the target has already been published to PCM this tick.
        template<class Writer>
        bool advanceCoefficients(const EffectsTables& tables,Writer&& write) noexcept
        {
            if (phase==8)
            {
                PublishEffectUpdate(write,EffectParameter::chorusInput,lpf);
                if (lpf==lpfTarget) phase=0;
                else raiseLpf(2);
                return true;
            }
            if (phase!=10 || parameters[0]>=tables.chorusLpf.size()) return false;
            if (dirty&1)
            {
                PublishEffectUpdate(write,EffectParameter::chorusInput,lpf);
                const auto low=lpf&255;
                if (!low)
                {
                    dirty=uint8_t((dirty&~1u)|0x10);
                    lpfTarget=tables.chorusLpf[parameters[0]];
                    lpf=lpfTarget&0xff00;
                }
                else lpf=uint16_t((lpf&0xff00)|(low>3 ? low-3 : 0));
            }
            constexpr std::array<unsigned,3> targets{1,2,6};
            for (unsigned i=0;i<3;++i)
            {
                const unsigned bit=2u<<i;
                if (!(dirty&bit)) continue;
                auto& value=output[i];
                const auto target=parameters[targets[i]];
                // Downward exact hits clear on the following call; upward
                // hits clear immediately, as in 6634..6659 and its siblings.
                bool done=value==target;
                if (value<target) {
                    done=unsigned(value)+2>=target;
                    value=done ? target : uint8_t(value+2);
                } else if (value>target) {
                    done=value<3 || value-3<target;
                    value=done ? target : uint8_t(value-3);
                }
                if (done) dirty &= uint8_t(~bit);
                if (i==1) PublishEffectUpdate(write,EffectParameter::chorusFeedback,value);
                else PublishEffectUpdate(write,i==0 ? EffectParameter::chorusLevel : EffectParameter::chorusSend,
                    uint16_t((output[0]<<8)|output[2]));
            }
            if (dirty&0x10)
            {
                PublishEffectUpdate(write,EffectParameter::chorusInput,lpf);
                if (lpf==lpfTarget) dirty &= uint8_t(~0x10u);
                else raiseLpf(3);
            }
            if (!dirty) phase=0;
            return true;
        }

        void raiseLpf(unsigned increment) noexcept
        {
            const auto low=uint8_t((lpf&255)+increment);
            lpf=uint16_t((lpf&0xff00)|(low>(lpfTarget&255) ? lpfTarget&255 : low));
        }

        template<class Writer>
        bool advanceDrain(Writer&& write) noexcept
        {
            if (phase==2)
            {
                for (auto& value:output) value=value>3 ? uint8_t(value-3) : 0;
                PublishChorusMix(write,output);
                if (!(output[0]|output[1]|output[2]))
                { PublishEffectUpdate(write,EffectParameter::chorusInput,0); lpf=0; drainTicks=33; phase=4; }
                return true;
            }
            if (phase==4)
            {
                if (--drainTicks==0) phase=6;
                return true;
            }
            return false;
        }

        void request(std::array<uint8_t,7> next) noexcept
        {
            next[2] >>= 1; next[6] >>= 1;
            if (next[3]!=parameters[3] || next[4]!=parameters[4]
                || next[5]!=parameters[5] || (phase!=0 && phase!=10))
            { parameters=next; phase=2; return; }
            phase=10;
            constexpr std::array<unsigned,4> indices{0,1,2,6};
            for (unsigned bit=0;bit<indices.size();++bit)
            {
                const auto i=indices[bit];
                if (next[i]==parameters[i]) continue;
                parameters[i]=next[i]; dirty |= uint8_t(1u<<bit);
                if (bit==0) dirty &= uint8_t(~0x10u);
            }
        }
    } chorus;

    // One control tick for an audio-owned effects target. Prepared operations
    // are immediate, so there is no hardware readback/retry protocol here.
    template<class Target>
    bool advance(const EffectsTables& tables,uint8_t currentCharacter,Target& target) noexcept
    {
        if(reverb.phase && !reverb.advanceDrain(target) && !reverb.advanceSetup(tables,target)
            && !reverb.advanceCoefficients(tables,currentCharacter,target)) return false;
        if(chorus.phase && !chorus.advanceDrain(target) && !chorus.advanceSetup(tables,nullptr,target,nullptr)
            && !chorus.advanceCoefficients(tables,target)) return false;
        return true;
    }
};
}
