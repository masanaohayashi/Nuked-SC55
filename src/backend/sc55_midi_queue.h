#pragma once
#include "sc55_midi.h"
#include <array>
#include <cstddef>

namespace sc55
{
enum class MidiDispatchResult { idle, future, deferred, accepted, failed };

// Serialized engine-owned ingress, NOT a cross-thread queue. Timestamps are
// absolute PCM device cycles; host buffers must be split at MIDI timestamps.
// Own all queued data, never retain a host buffer or SysEx pointer. Capacity
// is a caller policy, not a claim about the firmware UART buffer size.
template<std::size_t Capacity>
class MidiEventQueue
{
    static_assert(Capacity >= 2); // One byte can abort SysEx and begin another.
public:
    enum class PushStatus { accepted, full, invalidTime, failed };
    struct PushResult { PushStatus status; std::size_t consumed = 0; };

    std::size_t size() const noexcept { return count_; }
    bool failed() const noexcept { return failed_; }

    bool pushReceiveRecovery(uint64_t cycle) noexcept
    {
        if (failed_ || cycle<lastCycle_ || count_==Capacity) return false;
        events_[(head_+count_)%Capacity]={{MidiDecoder::Kind::receiveRecovery},cycle};
        ++count_; lastCycle_=cycle;
        return true;
    }
    // Call after dispatchOne returns, never from its sink. Communication-error
    // recovery discards pending RX bytes and both partial/running messages.
    void discardReceived() noexcept
    { head_=count_=0; decoder_.reset(); }

    // Full returns the exact accepted byte prefix. The caller must retain and
    // retry the suffix at its original timestamp before supplying later bytes.
    // Neither decoder state nor any events from the rejected byte are committed.
    PushResult push(std::span<const uint8_t> bytes,uint64_t cycle)
    {
        if (failed_) return {PushStatus::failed};
        if (cycle < lastCycle_) return {PushStatus::invalidTime};
        std::size_t consumed = 0;
        for (const auto byte : bytes)
        {
            auto next = decoder_;
            std::array<MidiDecoder::Event,2> decoded{};
            unsigned produced = 0;
            next.push(std::span(&byte,1),[&](auto event) { decoded[produced++] = event; });
            if (produced > Capacity-count_) return {PushStatus::full,consumed};
            for (unsigned i = 0; i < produced; ++i)
            {
                events_[(head_+count_)%Capacity] = {decoded[i],cycle};
                ++count_;
            }
            decoder_ = next; lastCycle_ = cycle; ++consumed;
        }
        return {PushStatus::accepted,consumed};
    }

    // At most one callback. A deferred sink must not mutate engine state.
    // Accepted may start asynchronous voice activation; that event is consumed
    // exactly once. Failed may follow partial mutation and is never replayed.
    // Sink must not throw, reenter or mutate this queue.
    template<class Sink>
    MidiDispatchResult dispatchOne(uint64_t now,Sink&& sink)
    {
        if (failed_) return MidiDispatchResult::failed;
        if (count_ == 0) return MidiDispatchResult::idle;
        const auto& entry = events_[head_];
        if (entry.cycle > now) return MidiDispatchResult::future;
        const auto result = sink(entry.event);
        if (result == MidiDispatchResult::deferred) return result;
        if (result != MidiDispatchResult::accepted)
        { failed_ = true; return MidiDispatchResult::failed; }
        head_ = (head_+1)%Capacity; --count_;
        return result;
    }

private:
    struct Entry { MidiDecoder::Event event{}; uint64_t cycle = 0; };
    std::array<Entry,Capacity> events_{};
    MidiDecoder decoder_;
    std::size_t head_ = 0, count_ = 0;
    uint64_t lastCycle_ = 0;
    bool failed_ = false;
};
}
