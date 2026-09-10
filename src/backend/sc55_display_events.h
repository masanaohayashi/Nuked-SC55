#pragma once
#include "sc55_sysex.h"
#include <atomic>
#include <algorithm>

namespace sc55
{
// Audio-owned receipt log, published through the existing SynthState exchange.
// No timers, UI callbacks, formatting, dynamic allocation or reader handshake.
struct DisplayEvent
{
    enum class Kind : uint8_t { text, bitmap, cancelText, cancelAll, fastScroll };
    uint64_t cycles=0;
    Kind kind=Kind::cancelAll;
    uint8_t length=0;
    std::array<uint8_t,64> bytes{};
};

struct DisplayEvents
{
    static constexpr unsigned capacity=64;
    uint64_t instance=0,sequence=0,cycles=0;
    std::array<DisplayEvent,capacity> events{};
    // Latest accepted values survive history overwrite and editor absence.
    DisplayEvent latestText,latestBitmap;
    bool fastScroll=false;
};

class DisplayEventLog
{
public:
    // Construct with the synth, off the audio thread. Identity also prevents a
    // reloaded ROM/synth from inheriting an old editor's display clock.
    DisplayEventLog() noexcept {state_.instance=nextInstance_.fetch_add(1,std::memory_order_relaxed);}
    void receive(const DisplayData& data,bool bitmap,uint64_t cycles) noexcept
    {
        DisplayEvent event; event.cycles=cycles;
        event.kind=bitmap ? DisplayEvent::Kind::bitmap : DisplayEvent::Kind::text;
        if(bitmap) {event.bytes=data.bitmap; state_.latestBitmap=event;}
        else {
            event.length=data.textLength;
            std::copy(data.text.begin(),data.text.end(),event.bytes.begin());
            state_.latestText=event;
        }
        append(event);
    }
    void cancel(bool bitmap,uint64_t cycles) noexcept
    {
        DisplayEvent event; event.cycles=cycles;
        event.kind=bitmap ? DisplayEvent::Kind::cancelAll : DisplayEvent::Kind::cancelText;
        state_.latestText=event;
        if(bitmap) state_.latestBitmap=event;
        append(event);
    }
    void fastScroll(bool enabled,uint64_t cycles) noexcept
    {
        DisplayEvent event; event.cycles=cycles; event.kind=DisplayEvent::Kind::fastScroll;
        event.length=enabled; state_.fastScroll=enabled; append(event);
    }
    DisplayEvents snapshot(uint64_t cycles) const noexcept
    {auto result=state_; result.cycles=cycles; return result;}
private:
    void append(const DisplayEvent& event) noexcept
    {state_.events[++state_.sequence%DisplayEvents::capacity]=event;}
    inline static std::atomic<uint64_t> nextInstance_{1};
    DisplayEvents state_;
};
}
