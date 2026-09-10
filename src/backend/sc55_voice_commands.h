#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace sc55
{
// Receive-time decisions, not a MIDI packet or a physical voice. Tone/velocity
// belong to this request; EG, soft pedal and other live controls are read by
// the later voice-management phase. No host pointers or firmware addresses.
struct NoteRequest
{
    enum class Action { on, off };
    Action action=Action::on;
    uint8_t part=0,key=0,velocity=0;
    std::optional<uint16_t> tone; // Melodic tone; rhythm preparation reads the live shared map.
};

struct PedalRequest
{
    enum class Kind { hold, sostenuto, portamento };
    Kind kind=Kind::hold;
    uint8_t part=0;
    bool enabled=false;
};

struct PortamentoSourceRequest
{
    uint8_t part=0,key=255;
};

struct PartReleaseRequest
{
    enum class Kind { notes, sound };
    Kind kind=Kind::notes;
    uint8_t part=0;
};

struct ControllerResetRequest { uint8_t part=0; };
struct ProgramVoiceRequest { uint8_t part=0; uint16_t tone=0; };
struct PartModeRequest { uint8_t part=0; bool poly=true; };

// The variant prevents a source key or pedal state from being disguised as a
// note velocity. Every producer shares the same voice-management FIFO.
using VoiceCommand=std::variant<NoteRequest,PedalRequest,PortamentoSourceRequest,PartReleaseRequest,ControllerResetRequest,ProgramVoiceRequest,PartModeRequest>;

// Audio-owner only. Publish a complete descending-part fan-out atomically;
// backpressure must never publish half a MIDI event and replay those parts.
// Capacity is native ingress policy, not an emulation of UART throughput.
class VoiceCommands
{
public:
    std::size_t size() const noexcept { return count_; }
    bool publish(std::span<const VoiceCommand> requests) noexcept
    {
        if(requests.size()>entries_.size()-count_) return false;
        for(const auto& request:requests) {
            entries_[(head_+count_)%entries_.size()]=request;
            ++count_;
        }
        return true;
    }
    std::optional<VoiceCommand> take() noexcept
    {
        if(!count_) return std::nullopt;
        const auto result=entries_[head_];
        head_=(head_+1)%entries_.size(); --count_;
        return result;
    }
    void clear() noexcept { head_=count_=0; }
private:
    std::array<VoiceCommand,128> entries_{};
    std::size_t head_=0,count_=0;
};
}
