#pragma once
#include "sc55_note_dispatch.h"

namespace sc55
{
// Audio-owner part configuration, indexed in firmware/GS order (0 is the
// default rhythm part, 1 is MIDI channel1). No emulated RAM/CPU is required.
// Bootstrap values preserve the native player's existing supported subset;
// construction is not a full ROM-derived boot or GS reset implementation.
struct PartSettings
{
    struct Part
    {
        ChannelControls::Channel controls;
        uint8_t bank = 0, keyShift = 64, fineTune = 128;
        uint8_t bankSelect = 0; // CC0 latch; bank changes on Program Change.
        NoteKeyRange keyRange;
        PartVelocityAdjustment velocity;
        std::array<uint8_t,12> scale = [] { std::array<uint8_t,12> a; a.fill(64); return a; }();
    };
    std::array<Part,16> parts{};
    std::array<PartMidiReceive,16> routing{};

    PartSettings() noexcept
    {
        for (unsigned part = 0; part < 16; ++part)
            routing[part] = {uint8_t(part == 0 ? 9 : part <= 9 ? part-1 : part),
                0x7fff,uint8_t(part == 0 ? 0xb0 : 0x81)};
    }

    enum class WriteResult { applied, unsupported, invalidLength };
    WriteResult write(std::span<const uint8_t> payload) noexcept
    { return write(payload,[](unsigned) { return false; }); }

    // Channel assignment has note/controller side effects even for the SAME
    // value. An owner must supply this operation; never silently update a byte.
    template<class ResetPart>
    WriteResult write(std::span<const uint8_t> payload,ResetPart&& resetPart) noexcept
    { return write(payload,resetPart,[](unsigned,uint8_t,uint8_t) { return false; }); }

    template<class ResetPart,class SelectTone>
    WriteResult write(std::span<const uint8_t> payload,ResetPart&& resetPart,SelectTone&& selectTone,
                      std::array<uint8_t,2>* assignedControllers=nullptr) noexcept
    { return write(payload,resetPart,selectTone,assignedControllers,
        [](unsigned,uint8_t,uint8_t) { return false; }); }

    template<class ResetPart,class SelectTone,class ChangeMode>
    WriteResult write(std::span<const uint8_t> payload,ResetPart&& resetPart,SelectTone&& selectTone,
                      std::array<uint8_t,2>* assignedControllers,ChangeMode&& changeMode) noexcept
    {
        if (payload.size() < 3) return WriteResult::invalidLength;
        if (payload[0] != 0x40 || (payload[1]&0xf0) != 0x10) return WriteResult::unsupported;
        const auto index = payload[1]&15;
        auto& part = parts[index];
        auto& receive = routing[index];
        auto address = payload[2];
        auto data = payload.subspan(3);
        if (data.empty()) return WriteResult::invalidLength;
        while (!data.empty())
        {
            if (address >= 3 && address <= 0x12)
            {
                constexpr std::array<uint16_t,16> masks{0x4000,0x2000,0x1000,0x0800,
                    0x0400,0x0200,0x0001,0x8000,0x0002,0x0004,0x0008,0x0010,
                    0x0020,0x0040,0x0080,0x0100};
                const auto mask = masks[address-3];
                receive.flags = uint16_t((receive.flags&~mask) | (data[0] ? mask : 0));
                ++address;
            }
            else if(address>=0x30 && address<=0x37) {
                const unsigned maximum=address==0x32 ? 80 : 114;
                part.controls.tone.values[address-0x30]=uint8_t(std::clamp(unsigned(data[0]),14u,maximum));
                address=address==0x37 ? 0x40 : uint8_t(address+1);
            }
            else switch (address)
            {
                case 0x00:
                    // 108c arg6: exactly bank+program, not a scalar prefix.
                    if (payload.size() != 5 || data.size() != 2) return WriteResult::invalidLength;
                    return selectTone(unsigned(index),data[0],data[1])
                        ? WriteResult::applied : WriteResult::unsupported;
                case 0x02:
                    if (!resetPart(unsigned(index))) return WriteResult::unsupported;
                    receive.channel = data[0] > 16 ? 16 : data[0];
                    address = 3; break;
                case 0x16:
                    part.keyShift = data[0] < 40 ? 40 : data[0] > 88 ? 88 : data[0];
                    address = 0x17; break;
                case 0x13: case 0x15:
                    if(!changeMode(unsigned(index),address,uint8_t(std::min(unsigned(data[0]),address==0x13 ? 1u : 2u))))
                        return WriteResult::unsupported;
                    ++address; break;
                case 0x14:
                    receive.noteFlags=uint8_t((receive.noteFlags&0xfc)|std::min(unsigned(data[0]),2u));
                    ++address; break;
                case 0x17:
                    if(payload.size()!=5 || data.size()!=2) return WriteResult::invalidLength;
                    part.fineTune=uint8_t(std::clamp(unsigned(uint8_t((data[0]<<4)|data[1])),8u,248u));
                    return WriteResult::applied;
                case 0x19: part.controls.volume = data[0]; address = 0x1a; break;
                case 0x1a: part.velocity.depth = data[0]; address = 0x1b; break;
                case 0x1b: part.velocity.offset = data[0]; address = 0x1c; break;
                case 0x1c: part.controls.pan = data[0]; address = 0x1d; break;
                case 0x1d: part.keyRange.low = data[0]; address = 0x1e; break;
                case 0x1e: part.keyRange.high = data[0]; address = 0x1f; break;
                case 0x1f: case 0x20:
                    if(!assignedControllers) return WriteResult::unsupported;
                    (*assignedControllers)[address-0x1f]=data[0]; ++address; break;
                case 0x21: part.controls.chorus = data[0]; address = 0x22; break;
                case 0x22: part.controls.reverb = data[0]; address = 0x30; break;
                case 0x40:
                    if (payload.size() != 15 || data.size() != 12) return WriteResult::invalidLength;
                    std::copy(data.begin(),data.end(),part.scale.begin());
                    return WriteResult::applied;
                default: return WriteResult::unsupported;
            }
            data = data.subspan(1);
        }
        return WriteResult::applied;
    }

    // Scalar MIDI values are applied after resolving channel->part. A disabled
    // receiver is ignored successfully, not confused with unsupported MIDI.
    bool applyScalar(const MidiDecoder::Event& event) noexcept
    {
        if (event.kind != MidiDecoder::Kind::message) return false;
        const unsigned kind = event.status&0xf0;
        uint16_t mask = 0;
        if (kind == 0xc0 && event.dataSize == 1) mask = 0x1000;
        else if (kind == 0xb0 && event.dataSize == 2)
        {
            mask = 0x0800;
            switch (event.first)
            {
                case 0: break;
                case 7: mask |= 4; break;
                case 10: mask |= 8; break;
                case 11: mask |= 16; break;
                case 67: mask |= 256; break;
                case 91: case 93: break;
                case 100: case 101: case 6: case 38: mask |= 1; break;
                case 98: case 99: mask |= 0x8000; break;
                default: return false;
            }
        }
        else return false;
        bool supported = true;
        for (unsigned part = 16; part-- > 0;)
        {
            if (routing[part].channel!=(event.status&15)) continue;
            // Mode changes precede the RPN/NRPN-specific receiver gate.
            if (kind==0xb0 && (routing[part].flags&0x0800)) {
                if (event.first==98 || event.first==99) parts[part].controls.nrpnSelected=true;
                if (event.first==100 || event.first==101) parts[part].controls.nrpnSelected=false;
            }
            if ((routing[part].flags&mask) == mask)
            {
                if (kind == 0xb0 && event.first == 0) parts[part].bankSelect = event.second;
                else supported &= ChannelControls::applyTo(parts[part].controls,event);
            }
        }
        return supported;
    }
};
}
