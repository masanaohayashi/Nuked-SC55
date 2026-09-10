// Intentionally links no PCM chip, H8, ROM loader or JUCE implementation.
// The product's renderer is driven through the same voice-state interface.
#include "pcm_sim.h"
#include "sc55_audio_buses.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

int main()
{
    PCMSim_Init();
    PCMSimVoices notifications{};
    uint32_t random=7163;
    const auto next=[&]() { random=random*1664525u+1013904223u; return random; };
    for(unsigned trial=0;trial<65536;++trial) {
        float input[4]; int dry[6],sends[6];
        for(auto& value:input) value=float(int32_t(next()&0x1fffff)-0x100000)/2.0f;
        for(auto& value:dry) value=int32_t(next()&0x7fffff)-0x400000;
        for(auto& value:sends) value=int32_t(next()&0x7fffff)-0x400000;
        // Original adapter arithmetic, with an unsigned shift for defined sign
        // extension. Exercise signed return rounding and20-bit wrap points.
        const auto add=[](int32_t a,int32_t b,int32_t carry) {
            return (int32_t(uint32_t(a)<<12)>>12)+(int32_t(uint32_t(b)<<12)>>12)+carry;
        };
        int32_t expected[4]; for(unsigned i=0;i<4;++i) expected[i]=int32_t(std::lrint(input[i]));
        for(unsigned channel=0;channel<2;++channel)
            for(unsigned stage=0;stage<3;++stage) {
                const auto value=dry[stage*2+channel];
                expected[channel]=add(expected[channel],value>>1,value&1);
            }
        constexpr unsigned destinations[]{3,3,2,3,2,3};
        for(unsigned i=0;i<6;++i) expected[destinations[i]]=add(expected[destinations[i]],sends[i]>>1,sends[i]&1);
        const auto actual=sc55::MixVoiceAndEffectBuses(input,dry,sends);
        if(actual.left!=expected[0] || actual.right!=expected[1]
            || actual.reverb!=expected[2] || actual.chorus!=expected[3]) return 4;
    }
    for(unsigned trial=0;trial<4096;++trial) {
        notifications.voice_count=32;
        notifications.boundaryEnabled=next(); notifications.boundaryLatched=next();
        const bool updates=bool(trial&1),canPublish=bool(trial&2);
        auto expectedLatch=notifications.boundaryLatched;
        int expected=-1;
        for(unsigned slot=0;slot<32;++slot) {
            notifications.address[slot]=int32_t(next()&0xfffff);
            notifications.address_loop[slot]=int32_t(next()&0xfffff);
            notifications.direction[slot]=(next()&0x10000)?-1:1;
            notifications.gate[slot]=(next()&0x20000)?1.0f:0.0f;
            const auto bit=uint32_t(1)<<slot;
            if(notifications.gate[slot]==0) { expectedLatch&=~bit; continue; }
            // Frozen predicate from the former PCM adapter loop.
            const int32_t loop=notifications.address_loop[slot];
            bool flag=((notifications.address[slot]+((-loop)&0xfffff))&0x100000)!=0;
            flag ^= notifications.direction[slot]<0;
            if((notifications.boundaryEnabled&bit) && !(expectedLatch&bit)
                && canPublish && expected<0 && flag) {
                if(updates) expectedLatch|=bit;
                expected=int(slot);
            }
        }
        if(PCMSim_CollectBoundary(notifications,updates,canPublish)!=expected
            || notifications.boundaryLatched!=expectedLatch) return 3;
    }
    std::array<uint8_t,512> samples{};
    std::array<uint8_t,32> exponents{};
    for(unsigned i=0;i<samples.size();++i) samples[i]=uint8_t(int(i%9)-4);
    exponents.fill(0x55);
    PCMSimVoices vector{},scalar{};
    for(unsigned slot=0;slot<PCM_SIM_MAX_VOICES;++slot) {
        vector.address[slot]=32;
        vector.address_loop[slot]=32;
        vector.address_end[slot]=256;
        vector.phase_step[slot]=0x2800+slot*137;
        vector.direction[slot]=1;
        vector.rom_base[slot]=samples.data(); vector.rom_mask[slot]=511;
        vector.block_base[slot]=exponents.data(); vector.block_mask[slot]=31;
        vector.svf_q[slot]=0.75f;
        vector.envelopes[slot].ramps={sc55::EnvelopeRamp{0x1030,0},
            sc55::EnvelopeRamp{0x1030,0},sc55::EnvelopeRamp{0x4030,0}};
        vector.pan_l[slot]=0.25f; vector.pan_r[slot]=0.5f;
        vector.send_reverb[slot]=0.125f; vector.send_chorus[slot]=0.0625f;
        vector.gate[slot]=1;
    }
    scalar=vector;
    double energy=0,error=0;
    for(unsigned frame=0;frame<8192;++frame) {
        if(frame==2048) {
            // Publishing waveform controls must not reset a running oscillator
            // or filter. The scalar side keeps its existing setup/history.
            for(unsigned slot=0;slot<24;++slot)
                PCMSim_SetWaveform(vector,slot,{samples.data(),exponents.data(),511,31,32,256,0,false,false});
        }
        if(frame==5120) {
            for(unsigned slot=6;slot<24;++slot) {
                PCMSim_RestartVoice(vector,slot,48,123,false);
                scalar.address[slot]=48; scalar.sub_phase[slot]=123; scalar.reverse_mask[slot]=0;
                scalar.reference[slot]=scalar.svf_low[slot]=scalar.svf_band[slot]=0;
            }
        }
        if(frame==4096) {
            // A whole silent SIMD group and a partially active group.
            for(unsigned slot=0;slot<6;++slot) vector.gate[slot]=scalar.gate[slot]=0;
        }
        if(frame%128==0) {
            for(unsigned slot=6;slot<24;++slot) {
                const auto pitch=uint32_t(0x2800+slot*137+(frame/128)%7);
                const sc55::VoiceRenderUpdate update{uint16_t(pitch),
                    {vector.envelopes[slot].ramps[0].command,vector.envelopes[slot].ramps[1].command,
                     vector.envelopes[slot].ramps[2].command},16,32,8,4,48,0};
                PCMSim_ApplyVoiceUpdate(vector,slot,update);
                scalar.phase_step[slot]=pitch;
            }
        }
        if(frame==6144)
            for(unsigned slot=6;slot<24;++slot)
                vector.envelopes[slot].ramps[1].command=scalar.envelopes[slot].ramps[1].command=0x0030;
        // One common sample clock, including voices keyed off above.
        const sc55::EnvelopeClock clock{uint16_t((0x3fff-frame)&0x3fff),true};
        PCMSim_AdvanceEnvelopes(scalar,clock);
        float a[4],b[4];
        PCMSim_RenderFrame(vector,clock,a);
        PCMSim_RenderFrameScalar(scalar,b);
        for(unsigned bus=0;bus<4;++bus) {
            if(!std::isfinite(a[bus]) || !std::isfinite(b[bus])) return 1;
            energy+=double(b[bus])*b[bus];
            const double delta=double(a[bus])-b[bus]; error+=delta*delta;
        }
        for(unsigned slot=0;slot<24;++slot)
            if(vector.address[slot]!=scalar.address[slot]
                || vector.sub_phase[slot]!=scalar.sub_phase[slot]) return 2;
    }
    if(energy==0 || error/energy>1.0e-8) return 3;
    std::printf("Independent voice renderer: 8192 frames, four buses, relative error %.9f\n",
        std::sqrt(error/energy));
    return 0;
}
