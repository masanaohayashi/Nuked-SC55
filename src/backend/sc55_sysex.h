#pragma once
#include "sc55_midi.h"
#include <array>
#include <cstddef>

namespace sc55
{
// Device-level reception, independent of GS part flags and GS-reset defaults.
// H8: ac24 identity, cdf8 exclusive reception, cdf7 GM/GS-reset reception,
// cdc8 bit9 checksum override. The audio owner applies configuration changes.
struct MidiInputSettings
{
    uint8_t deviceId=0x10;
    bool receiveExclusive=true,receiveReset=true,ignoreChecksum=false;
    bool receiveProgramChanges=true;
};

// Serialized audio-owner receiver. The wire packet is staged until EOX; no
// settings are changed by a partial, interrupted or bad-checksum transaction.
// This replaces reception, not the GS address dispatcher/reset state machine.
class SysExReceiver
{
public:
    enum class Status { pending, ignored, aborted, tooLong, badChecksum, roland, gmOn, gmOff };
    struct Result
    {
        Status status = Status::pending;
        uint8_t model = 0, command = 0;
        // Address followed by data/size. Valid until the next receive() call.
        std::span<const uint8_t> payload{};
    };

    Result receive(const MidiDecoder::Event& event,uint8_t deviceId = 0x10,
                   bool receiveEnabled = true,bool ignoreChecksum = false) noexcept
    {
        using Kind = MidiDecoder::Kind;
        if (event.kind == Kind::realtime) return {};
        if (event.kind == Kind::sysexBegin)
        { size_ = 0; active_ = true; overflow_ = false; return {}; }
        if (event.kind == Kind::sysexAbort)
        { active_ = false; size_ = 0; return {Status::aborted}; }
        if (!active_) return {Status::ignored};
        if (event.kind == Kind::sysexData)
        {
            if (size_ < bytes_.size()) bytes_[size_++] = event.first;
            else overflow_ = true;
            return {};
        }
        if (event.kind != Kind::sysexEnd) return {};
        active_ = false;
        if (!receiveEnabled) return {Status::ignored};
        if (overflow_) return {Status::tooLong};
        if (size_ == 4 && bytes_[0] == 0x7e && bytes_[1] == 0x7f && bytes_[2] == 9)
        {
            if (bytes_[3] == 1) return {Status::gmOn};
            if (bytes_[3] == 2) return {Status::gmOff};
        }
        if (size_ < 8 || bytes_[0] != 0x41 || bytes_[1] != deviceId
            || (bytes_[2] != 0x42 && bytes_[2] != 0x45)
            || (bytes_[3] != 0x11 && bytes_[3] != 0x12)) return {Status::ignored};
        unsigned checksum = 0;
        for (std::size_t i = 4; i < size_; ++i) checksum += bytes_[i];
        if (!ignoreChecksum && (checksum & 127)) return {Status::badChecksum};
        return {Status::roland,bytes_[2],bytes_[3],std::span(bytes_).subspan(4,size_-5)};
    }

private:
    // v1.21 final command+address+data limit88h, plus manufacturer/device/model
    // and checksum. Larger input is drained, not truncated into a valid packet.
    std::array<uint8_t,0x88+4> bytes_{};
    std::size_t size_ = 0;
    bool active_ = false, overflow_ = false;
};

// Model45 DT1 payload owner (04:7321..73a6). Audio-thread state; the UI must
// consume a published copy, never borrow this object across threads. Layout,
// scrolling and display expiry belong to the display controller, not MIDI.
struct DisplayData
{
    std::array<uint8_t,32> text{};
    std::array<uint8_t,64> bitmap{};
    uint8_t textLength=0;
    uint64_t textRevision=0, bitmapRevision=0;
    enum class WriteResult { applied, invalidAddress, invalidLength };

    WriteResult write(std::span<const uint8_t> payload) noexcept
    {
        if(payload.size()<3) return WriteResult::invalidLength;
        if(payload[0]!=0x10 || payload[2]!=0 || payload[1]>1)
            return WriteResult::invalidAddress;
        const auto data=payload.subspan(3);
        if(payload[1]==0) {
            if(data.empty() || data.size()>text.size()) return WriteResult::invalidLength;
            textLength=uint8_t(data.size());
            for(std::size_t i=0;i<data.size();++i) text[i]=data[i]<0x20 ? 0x20 : data[i];
            ++textRevision;
        } else {
            if(data.size()!=bitmap.size()) return WriteResult::invalidLength;
            for(std::size_t i=0;i<data.size();++i) bitmap[i]=data[i];
            ++bitmapRevision;
        }
        return WriteResult::applied;
    }
};

// Master controls use values, not H8 RAM or a CPU snapshot. Construct before
// rendering; mutation is serialized with notes/control ticks. Full GS reset
// still belongs to the engine because live voices/FX must be drained as well.
struct MasterControls
{
    uint16_t tune = 1024;
    uint8_t volume = 100, keyShift = 64, pan = 64;
    uint8_t portamentoController = 84;
    uint8_t resetCommand = 0x42; // Stored GS command; only zero requests reset.

    enum class WriteResult { applied, unsupported, invalidLength, resetRequested };
    WriteResult write(std::span<const uint8_t> payload) noexcept
    {
        if (payload.size() < 3) return WriteResult::invalidLength;
        if (payload[0] != 0x40 || payload[1] != 0) return WriteResult::unsupported;
        auto address = payload[2];
        // 1511 compares the complete command/address/data length with8.
        // Unlike scalar records, master tune cannot prefix a longer write.
        if (address == 0 && payload.size() != 7) return WriteResult::invalidLength;
        auto data = payload.subspan(3);
        if (data.empty()) return WriteResult::invalidLength;
        // 0f7c..0f8e continues through the next TABLE RECORD, not the next
        // numeric address. Earlier settings stay committed if a later one fails.
        while (!data.empty())
        {
            if (address != 0 && address != 4 && address != 5 && address != 6 && address != 0x7e && address != 0x7f)
                return WriteResult::unsupported;
            if (address == 0)
            {
                if (data.size() < 4) return WriteResult::invalidLength;
                // Shift bytes then OR, including firmware byte truncation.
                const auto high = uint8_t((data[0]<<4) | data[1]);
                const auto low = uint8_t((data[2]<<4) | data[3]);
                const auto value = uint16_t((unsigned(high)<<8)|low);
                tune = value < 24 ? 24 : value > 2024 ? 2024 : value;
                data = data.subspan(4); address = 4;
            }
            else
            {
                if (address == 4) volume = data[0];
                else if (address == 5) keyShift = data[0] < 40 ? 40 : data[0] > 88 ? 88 : data[0];
                else if(address==6) pan = data[0] == 0 ? 1 : data[0];
                else if(address==0x7e) portamentoController=data[0];
                else {
                    resetCommand=data[0];
                    if(resetCommand==0) return WriteResult::resetRequested;
                }
                data = data.subspan(1); address = address == 6 ? 0x7e : uint8_t(address+1);
            }
        }
        return WriteResult::applied;
    }
};
}
