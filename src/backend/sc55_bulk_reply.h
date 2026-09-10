#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace sc55
{
// Audio-owner bulk transfer, independent of CPU, PCM and transport. The
// transport must acknowledge actual completion, not just enqueueing a packet.
// No setting snapshot: each chunk reads its owning configuration when emitted.
class BulkReplyTransfer
{
public:
    struct Packet { std::array<uint8_t,138> bytes{}; unsigned size=0; };
    enum class BeginResult { accepted, invalid, busy };
    enum class PanelScope { allSettings, systemAndParts, parts };
    BeginResult begin(std::span<const uint8_t> request) noexcept
    {
        if(active()) return BeginResult::busy;
        return beginRequest(request);
    }
    BeginResult beginAllSettings() noexcept
    { return beginPanel(PanelScope::allSettings,0xffff,3); }
    BeginResult beginPanel(PanelScope scope,uint16_t parts,uint8_t drumMaps) noexcept
    {
        if(active()) return BeginResult::busy;
        if((scope!=PanelScope::allSettings && scope!=PanelScope::systemAndParts && scope!=PanelScope::parts)
            || drumMaps>3) return BeginResult::invalid;
        segmentCount_=segmentIndex_=0;
        const auto add=[&](uint8_t region,unsigned map,unsigned offset,unsigned length) {
            segments_[segmentCount_++]={region,uint8_t(map),uint16_t(offset),uint16_t(length)};
        };
        if(scope==PanelScope::allSettings) {
            add(0x48,0,0,0x748);drumMaps=3;
        } else {
            // 715f sends the eight master bytes and remaining64 system bytes
            // before7191's ascending selected parts. 717d starts at7191.
            if(scope==PanelScope::systemAndParts) {add(0x48,0,0,8);add(0x48,0,8,64);}
            for(unsigned part=0;part<16;++part)
                if(parts&(1u<<part)) add(0x48,0,0x48+0x70*part,0x70);
            // v1.21 7191 accumulates both maps, but70f2's synchronous parser
            // call clobbers that selection. After map0,71ef observes R5=0:
            // partial dumps omit map1 when both were selected. The explicit
            // all-settings path above sends both maps and does not use7191.
            if(drumMaps&1) drumMaps=1;
        }
        for(unsigned map=0;map<2;++map) if(drumMaps&(1u<<map))
            for(unsigned wireBlock=0;wireBlock<16;wireBlock+=2) {
                const auto block=wireBlock<4 ? wireBlock^2 : wireBlock;
                add(0x49,map,block*64,block==14 ? 12 : 128);
            }
        if(!segmentCount_) return BeginResult::accepted;
        selectPanelSegment();
        // Panel producer 70e8/712a waits before each internal request.
        // 7318 directly calls the parser in the panel task; it returns only
        // after that request completes, before the next producer wait starts.
        delayTicks_=40; phase_=Phase::requestSpacing;
        return BeginResult::accepted;
    }
private:
    void selectPanelSegment() noexcept
    {
        const auto& segment=segments_[segmentIndex_];
        region_=segment.region;map_=segment.map;offset_=segment.offset;end_=offset_+segment.length;
    }
    BeginResult beginRequest(std::span<const uint8_t> request) noexcept
    {
        if(request.size()!=6 || request[3]!=0) return BeginResult::invalid;
        const unsigned length=unsigned(request[4])*64+(request[5]>>1);
        unsigned offset=0,map=0;
        if(request[0]==0x48) {
            offset=unsigned(request[1])*64+(request[2]>>1);
            if(!length || offset>=0x748 || length>0x748-offset) return BeginResult::invalid;
        } else if(request[0]==0x49) {
            if(request[1]>=0x20 || (request[1]&1) || request[2]!=0) return BeginResult::invalid;
            map=request[1]>>4;
            unsigned block=request[1]&15;
            if(block<4) block^=2;
            offset=block*64;
            if(length!=(offset==0x380 ? 12u : 128u)) return BeginResult::invalid;
        } else return BeginResult::invalid;
        region_=request[0]; map_=map; offset_=offset; end_=offset+length;
        phase_=Phase::ready;
        return BeginResult::accepted;
    }
public:
    // load(region, map, configuration offset) is a bounded read from the
    // native owner. Drum offset excludes tone IDs on the wire, so add100h.
    template<class Load>
    bool next(Packet& packet,Load&& load) noexcept
    {
        if(phase_!=Phase::ready) return false;
        const unsigned count=std::min(64u,end_-offset_);
        unsigned block=offset_/64;
        if(region_==0x49 && block<4) block^=2;
        packet.bytes[0]=0xf0; packet.bytes[1]=0x41; packet.bytes[2]=0x10;
        packet.bytes[3]=0x42; packet.bytes[4]=0x12; packet.bytes[5]=region_;
        packet.bytes[6]=uint8_t(block|(map_<<4)); packet.bytes[7]=uint8_t((offset_%64)*2);
        unsigned sum=packet.bytes[5]+packet.bytes[6]+packet.bytes[7];
        for(unsigned i=0;i<count;++i) {
            const uint8_t value=load(region_,map_,offset_+i+(region_==0x49 ? 0x100 : 0));
            packet.bytes[8+i*2]=value>>4; packet.bytes[9+i*2]=value&15;
            sum+=(value>>4)+(value&15);
        }
        packet.bytes[8+count*2]=uint8_t(-sum)&127;
        packet.bytes[9+count*2]=0xf7; packet.size=10+count*2;
        phase_=Phase::transmitting;
        return true;
    }

    bool transmissionComplete() noexcept
    {
        if(phase_!=Phase::transmitting) return false;
        // 04:1c78 / 04:1d2b: wait40 kernel ticks AFTER TX ring drains,
        // including the last chunk. Never turn this into audio-thread sleep.
        delayTicks_=40; phase_=Phase::spacing; return true;
    }
    void advanceKernelTicks(unsigned ticks) noexcept
    {
        if(phase_!=Phase::spacing && phase_!=Phase::requestSpacing) return;
        if(ticks<delayTicks_) { delayTicks_-=ticks; return; }
        delayTicks_=0;
        if(phase_==Phase::requestSpacing) {phase_=Phase::ready;return;}
        offset_+=64;
        if(offset_<end_) {phase_=Phase::ready;return;}
        if(segmentCount_ && ++segmentIndex_<segmentCount_) {
            selectPanelSegment();
            delayTicks_=40;phase_=Phase::requestSpacing;
            return;
        }
        segmentCount_=0;phase_=Phase::idle;
    }
    bool active() const noexcept { return phase_!=Phase::idle; }
    bool panelTransferActive() const noexcept { return segmentCount_!=0; }
    bool transmitting() const noexcept { return phase_==Phase::transmitting; }
    unsigned spacingTicksRemaining() const noexcept
    { return phase_==Phase::spacing || phase_==Phase::requestSpacing ? delayTicks_ : 0; }

private:
    enum class Phase { idle, ready, transmitting, spacing, requestSpacing };
    Phase phase_=Phase::idle;
    uint8_t region_=0;
    unsigned map_=0,offset_=0,end_=0,delayTicks_=0;
    struct Segment { uint8_t region,map;uint16_t offset,length; };
    // Two system regions, sixteen parts, eight regions for each drum map.
    std::array<Segment,34> segments_{};
    unsigned segmentCount_=0,segmentIndex_=0;
};
}
