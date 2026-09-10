#pragma once
#include "MidiFilePlayer.h"
#include <stdexcept>
#include <utility>

// Local 55KTIZKE.RCP fixture: preserve SysEx and channel12 configuration in
// original order, then replay the four non-legato notes at155..156 seconds.
// This is a minimized regression, not a replacement for full-song validation.
inline void selectMonoReplenishmentExcerpt(MidiFileData& song)
{
    std::vector<MidiFileEvent> selected;
    double start=0;
    unsigned notes=0;
    for(auto event:song.events) {
        if(event.seconds>=156) break;
        if(event.bytes.empty()) continue;
        const auto status=event.bytes[0];
        if(status!=0xf0 && (status&15)!=11) continue;
        if(event.seconds<155) {
            if((status&0xe0)==0x80) continue;
            event.seconds=start;start+=0.01;
        } else {
            event.seconds=start+event.seconds-155;
            if(status==0x9b && event.bytes.size()==3 && event.bytes[2]) ++notes;
        }
        selected.push_back(std::move(event));
    }
    if(notes!=4) throw std::runtime_error("Expected 55KTIZKE.RCP mono replenishment fixture");
    song.events=std::move(selected);
    song.songEndSeconds=start+1;
    song.lastBarSeconds=0;
}
