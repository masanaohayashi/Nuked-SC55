#pragma once
#include "sc55_note_dispatch.h"

namespace sc55
{
// Common Note On receive traversal,20f2/20fe..2126. Part-specific rhythm,
// key-range and note-preparation gates remain the visitor's responsibility.
// Routing is frozen at admission; serialize configuration changes until done.
class NoteOnFanout
{
public:
    enum class Status { idle, ready, deferred, complete, failed };
    enum class Visit { accepted, deferred, failed };
    bool pending() const noexcept { return remaining_ != 0 && status_ != Status::failed; }
    Status status() const noexcept { return status_; }
    uint16_t remainingParts() const noexcept { return remaining_; }

    bool begin(const MidiDecoder::Event& event,std::span<const PartMidiReceive,16> routing,NoteReceiveMode mode) noexcept
    {
        if (pending() || status_ == Status::failed || event.kind != MidiDecoder::Kind::message
            || (event.status&0xf0) != 0x90 || event.dataSize != 2 || event.first > 127
            || event.second == 0 || event.second > 127) return false;
        remaining_ = 0; event_ = event;
        for (unsigned part = 0; part < 16; ++part)
            if (AcceptNotePart(event.status&15,part,routing[part],mode)) remaining_ |= uint16_t(1u<<part);
        status_ = remaining_ ? Status::ready : Status::complete;
        return true;
    }
    Status fail() noexcept { return status_ = Status::failed; }

    // One visitor per call, descending15..0. Accepted may reserve asynchronous
    // activation; do not replay that part on resume. Deferred must not mutate
    // note/PCM state. Earlier accepted parts are not rolled back on failure.
    template<class Visitor>
    Status advance(Visitor&& visit)
    {
        if (!pending()) return status_;
        for (unsigned part = 16; part-- > 0;)
            if (remaining_&(1u<<part))
            {
                const auto result = visit(uint8_t(part),event_);
                if (result == Visit::failed) return fail();
                if (result == Visit::deferred) return status_ = Status::deferred;
                remaining_ &= uint16_t(~(1u<<part));
                return status_ = remaining_ ? Status::ready : Status::complete;
            }
        return status_;
    }
private:
    MidiDecoder::Event event_{};
    uint16_t remaining_ = 0;
    Status status_ = Status::idle;
};
}
