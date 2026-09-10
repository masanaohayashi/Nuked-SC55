#pragma once
#include "sc55_sysex.h"
#include <algorithm>

namespace sc55
{
// Audio-owner display control. A display-service call, not a UI paint, moves
// the scrolling window. The caller supplies the current normal 16-character
// line and the shared device-timer ticks (00:7f48..7f6c).
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
    Line service(const Line& normal,bool fastScroll=false) noexcept
    {
        if(textTicks_) {
            Line line; line.fill(' ');
            // 3812..3843 puts floor((16-length)/2) spaces on the right.
            std::copy_n(text_.begin(),length_,line.begin()+(17-length_)/2);
            return line;
        }
        if(!scrolling_) return normal;
        // 386d..3894,38ee: normal line is refreshed at both ends of the
        // scrolling message. No frozen copy of the selected part's text.
        std::array<uint8_t,66> sequence{};
        std::copy(normal.begin(),normal.end(),sequence.begin());
        sequence[16]='<';
        std::copy_n(text_.begin(),length_,sequence.begin()+17);
        sequence[17+length_]='<';
        std::copy(normal.begin(),normal.end(),sequence.begin()+18+length_);
        Line line;
        std::copy_n(sequence.begin()+offset_,16,line.begin());
        if(!scrollTicks_) {
            scrollTicks_=fastScroll ? 20 : 30;
            if(++offset_==length_+19) scrolling_=false;
        }
        return line;
    }
    void cancelText() noexcept {textTicks_=0; scrolling_=false;}
    void cancelBitmap() noexcept {bitmapTicks_=0;}
    bool textActive() const noexcept {return textTicks_ || scrolling_;}
    bool bitmapActive() const noexcept {return bitmapTicks_!=0;}
    const auto& bitmap() const noexcept {return bitmap_;}
    unsigned textTicks() const noexcept {return textTicks_;}
    unsigned bitmapTicks() const noexcept {return bitmapTicks_;}
    unsigned scrollTicks() const noexcept {return scrollTicks_;}
    unsigned scrollOffset() const noexcept {return offset_;}
private:
    std::array<uint8_t,32> text_{};
    std::array<uint8_t,64> bitmap_{};
    uint64_t textRevision_=0,bitmapRevision_=0;
    unsigned length_=0,offset_=0,textTicks_=0,bitmapTicks_=0,scrollTicks_=0;
    bool scrolling_=false;
};
}
