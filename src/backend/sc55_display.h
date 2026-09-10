#pragma once
#include "sc55_sysex.h"
#include "sc55_display_events.h"
#include "sc55_control_clock.h"
#include <algorithm>
#include <optional>

namespace sc55
{
// Unformatted display state. Timing and presentation are separate: service()
// advances state, while frame().compose() is a read-only presentation operation.
struct DisplayFrame
{
    using Line=std::array<uint8_t,16>;
    std::array<uint8_t,32> text{};
    unsigned length=0,offset=0;
    bool centered=false,visible=false;

    // Display-thread only in the product; never called by audio rendering or
    // snapshot publication. The selected part's normal line is supplied by UI.
    Line compose(const Line& normal) const noexcept
    {
        if(!visible) return normal;
        if(centered) {
            Line line; line.fill(' ');
            std::copy_n(text.begin(),length,line.begin()+(17-length)/2);
            return line;
        }
        std::array<uint8_t,66> sequence{};
        std::copy(normal.begin(),normal.end(),sequence.begin());
        sequence[16]='<';
        std::copy_n(text.begin(),length,sequence.begin()+17);
        sequence[17+length]='<';
        std::copy(normal.begin(),normal.end(),sequence.begin()+18+length);
        Line line;
        std::copy_n(sequence.begin()+offset,16,line.begin());
        return line;
    }
};

// Display-owner timing state. NativeMelodicPlayer does not own or call this.
class DisplayControl
{
public:
    using Line=std::array<uint8_t,16>;
    void receive(const DisplayData& data) noexcept
    {
        if(data.textRevision!=textRevision_) {
            textRevision_=data.textRevision;
            length_=data.textLength;
            text_=data.text;
            scrolling_=length_>16;
            offset_=0;
            textTicks_=scrolling_ ? 0 : 300;
            // 37dd does not reset the shared scroll-step countdown.
        }
        if(data.bitmapRevision!=bitmapRevision_) {
            bitmapRevision_=data.bitmapRevision;
            bitmap_=data.bitmap;
            bitmapTicks_=300;
        }
    }
    void timerTicks(unsigned ticks) noexcept
    {
        const auto subtract=[&](unsigned value) {return value>ticks ? value-ticks : 0u;};
        textTicks_=subtract(textTicks_);
        bitmapTicks_=subtract(bitmapTicks_);
        scrollTicks_=subtract(scrollTicks_);
    }
    void service(bool fastScroll=false) noexcept
    {
        frame_={text_,length_,offset_,textTicks_!=0,textActive()};
        if(textTicks_ || !scrolling_) return;
        if(!scrollTicks_) {
            scrollTicks_=fastScroll ? 20 : 30;
            if(++offset_==length_+19) scrolling_=false;
        }
    }
    const DisplayFrame& frame() const noexcept {return frame_;}
    void cancelText() noexcept {textTicks_=0; scrolling_=false; frame_.visible=false;}
    void cancelBitmap() noexcept {bitmapTicks_=0;}
    bool textActive() const noexcept {return textTicks_ || scrolling_;}
    bool bitmapActive() const noexcept {return bitmapTicks_!=0;}
    const auto& bitmap() const noexcept {return bitmap_;}
    unsigned textTicks() const noexcept {return textTicks_;}
    unsigned bitmapTicks() const noexcept {return bitmapTicks_;}
    unsigned scrollTicks() const noexcept {return scrollTicks_;}
    unsigned scrollOffset() const noexcept {return offset_;}
private:
    DisplayFrame frame_;
    std::array<uint8_t,32> text_{};
    std::array<uint8_t,64> bitmap_{};
    uint64_t textRevision_=0,bitmapRevision_=0;
    unsigned length_=0,offset_=0,textTicks_=0,bitmapTicks_=0,scrollTicks_=0;
    bool scrolling_=false;
};

struct DisplayView
{
    DisplayFrame text;
    std::array<uint8_t,64> bitmap{};
    bool bitmapVisible=false;
};

// Message-thread owner. Replay event time, not paint count; opening an editor
// does not restart a message's lifetime. Only immutable published data enters.
class DisplayPresentation
{
public:
    // Product preference is owned by the UI. Without an override diagnostic
    // consumers retain the firmware event history's scroll setting.
    DisplayView update(const DisplayEvents& input,std::optional<bool> fastScrollOverride={}) noexcept
    {
        fastScrollOverride_=fastScrollOverride;
        if(input.instance!=instance_ || input.sequence<sequence_ || input.cycles<cycles_) {
            reset(); instance_=input.instance;
        }
        if(input.sequence-sequence_>DisplayEvents::capacity) {
            // A stalled/absent reader never backpressures audio. On history
            // overrun resume latest text/bitmap at their original timestamps.
            // Discarded intermediate scroll-phase changes cannot be recovered;
            // this is a visible resync, not a claim of exact history replay.
            ++resyncs_;
            reset(); instance_=input.instance; fastScroll_=input.fastScroll;
            const auto text=input.latestText.kind==DisplayEvent::Kind::text;
            const auto bitmap=input.latestBitmap.kind==DisplayEvent::Kind::bitmap;
            if(text && bitmap && input.latestBitmap.cycles<input.latestText.cycles) {
                replay(input.latestBitmap); replay(input.latestText);
            } else {
                if(text) replay(input.latestText);
                if(bitmap) replay(input.latestBitmap);
            }
        } else {
            for(auto sequence=sequence_+1;sequence<=input.sequence;++sequence)
                replay(input.events[sequence%DisplayEvents::capacity]);
        }
        sequence_=input.sequence;
        advance(input.cycles);
        return {control_.frame(),control_.bitmap(),control_.bitmapActive()};
    }
    uint64_t resyncs() const noexcept {return resyncs_;}
private:
    // Device display timer and task7's normal service period. These clocks
    // schedule UI work only; they never split NativeSynth audio spans.
    static constexpr uint64_t timerCycles=200000;
    static constexpr uint64_t serviceCycles=20*ControlTaskClock::kernelTickCycles;
    void reset() noexcept
    {control_={}; data_={}; cycles_=sequence_=0; fastScroll_=false;}
    void advance(uint64_t end) noexcept
    {
        while(cycles_<end) {
            if(!control_.textActive() && !control_.bitmapActive()
                && !control_.frame().visible && !control_.scrollTicks()) {cycles_=end; break;}
            const auto nextService=(cycles_/serviceCycles+1)*serviceCycles;
            const auto next=std::min(end,nextService);
            control_.timerTicks(unsigned(next/timerCycles-cycles_/timerCycles));
            cycles_=next;
            if(next==nextService) control_.service(fastScrollOverride_.value_or(fastScroll_));
        }
    }
    void replay(const DisplayEvent& event) noexcept
    {
        advance(event.cycles);
        switch(event.kind) {
            case DisplayEvent::Kind::text:
                std::copy_n(event.bytes.begin(),data_.text.size(),data_.text.begin());
                data_.textLength=event.length; ++data_.textRevision; control_.receive(data_); break;
            case DisplayEvent::Kind::bitmap:
                data_.bitmap=event.bytes; ++data_.bitmapRevision; control_.receive(data_); break;
            case DisplayEvent::Kind::cancelText: control_.cancelText(); break;
            case DisplayEvent::Kind::cancelAll: control_.cancelText(); control_.cancelBitmap(); break;
            case DisplayEvent::Kind::fastScroll: fastScroll_=event.length!=0; break;
        }
    }
    DisplayControl control_;
    DisplayData data_;
    uint64_t instance_=0,sequence_=0,cycles_=0,resyncs_=0;
    bool fastScroll_=false;
    std::optional<bool> fastScrollOverride_;
};
}
