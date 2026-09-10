#pragma once
#include <array>
#include <cstdint>

namespace sc55
{
// All voices see the same envelope clock. It is supplied by the audio owner,
// not restarted on note-on and not advanced independently by individual voices.
struct EnvelopeClock
{
    uint16_t phase=0;
    bool update=true;
};

// Exact SC-55 ramp arithmetic, independent of PCM register storage. A command
// encodes an eight-bit target and rate/mode; level is the controller readback.
// The two gain stages differ when a linear ramp crosses its target.
struct EnvelopeRamp
{
    enum class Stage { firstGain, secondGain, cutoff };
    uint16_t command=0,level=0;

    uint16_t advance(Stage stage,EnvelopeClock clock,bool active) noexcept
    {
        level&=0x7fff;
        const unsigned speed=command&255, target=command>>8;
        const bool slow=(speed&0xf0)==0;
        const bool alternate=slow || (speed&0x10)!=0;
        const bool linear=clock.update && ((speed&0x80)==0
            || ((speed&0x40)==0 && (!alternate || (speed&0x20)==0)));
        const bool everyFrame=(speed&0x80)==0 || (speed&0x40)==0;
        unsigned phaseShift=0,writeMask=0;
        if(!everyFrame) {
            const unsigned mode=unsigned(alternate)|((speed&0x20)?2u:0u);
            constexpr unsigned shifts[]{2,4,6,8},masks[]{3,15,63,127};
            phaseShift=shifts[mode]; writeMask=masks[mode];
        }
        const unsigned nibble=(clock.phase>>phaseShift)&15;
        const int rounding=int(((nibble&1)<<3)|((nibble&2)<<1)
            |((nibble&4)>>1)|((nibble&8)>>3));
        const bool write=clock.update && (!active || (clock.phase&writeMask)==0);
        const int current=stage!=Stage::cutoff || active ? int(level)*16 : 0;
        const int difference=int(target)*2048-current;
        int result;
        bool crossed=false;
        if(!linear) {
            const unsigned shift=(10-(speed&15))&15;
            result=(int(target)*2048+rounding+(difference>>shift)-difference)>>4;
        } else {
            const unsigned shift=(10-(((speed>>4)&14)|unsigned(alternate)))&15;
            const bool negative=(difference&0x80000)!=0;
            int step=int(speed&15)*512;
            if(!slow) step|=0x2000;
            if(negative) step^=~0x3f;
            result=((step>>shift)+(stage!=Stage::cutoff || active ? current|rounding : 0))>>4;
            crossed=(((int(target)*2048-result*16)&0x80000)!=0)!=negative;
        }
        if(write) level=uint16_t(crossed ? target*128 : unsigned(result)&0x7fff);
        return uint16_t(stage==Stage::secondGain && crossed ? target*128 : unsigned(result)&0x7ffe);
    }
};

struct VoiceEnvelopes
{
    std::array<EnvelopeRamp,3> ramps{};
    // Controller handoff at an envelope segment transition. Held segments
    // restore their saved level; moving segments capture the live level and
    // hold it until the next control command. No audio time elapses here.
    std::array<uint16_t,3> synchronize(const std::array<uint16_t,3>& commands,
        const std::array<uint16_t,3>& savedLevels) noexcept
    {
        auto levels=savedLevels;
        for(unsigned i=0;i<3;++i) {
            if(commands[i]==0xff00) ramps[i].level=uint16_t(savedLevels[i]>>1);
            else {
                ramps[i].command=0xff00;
                levels[i]=uint16_t(ramps[i].level<<1);
            }
        }
        return levels;
    }
    struct Output { float firstGain,secondGain,cutoff; };
    Output advance(EnvelopeClock clock,bool active) noexcept
    {
        // Filter uses the previous cutoff, amplifiers use the new gains.
        Output result{0,0,float(ramps[2].level)};
        if(active) {
            result.firstGain=ramps[0].advance(EnvelopeRamp::Stage::firstGain,clock,true)/16384.0f;
            result.secondGain=ramps[1].advance(EnvelopeRamp::Stage::secondGain,clock,true)/16384.0f;
            ramps[2].advance(EnvelopeRamp::Stage::cutoff,clock,true);
        } else {
            ramps[0].level=ramps[1].level=0;
            if(ramps[2].command) ramps[2].advance(EnvelopeRamp::Stage::cutoff,clock,false);
            else if(clock.update) ramps[2].level=0;
        }
        return result;
    }
};
}
