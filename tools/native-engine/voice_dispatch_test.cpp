#include "sc55_voice_prepare.h"
#include <cstdlib>
#include <iostream>

static void check(bool ok) {if(!ok) {std::cerr<<"Voice dispatch regression failed\n";std::exit(1);}}
struct Writer {
    unsigned writes=0;
    void setVoiceRamp(uint8_t,sc55::EnvelopeRamp::Stage,uint16_t) {++writes;}
};
int main()
{
    using namespace sc55;
    for(auto pair: {std::array<unsigned,2>{2,3},{0,14},{19,1},{126,127}}) {
        PreparedNoteVelocity selection{};
        InstalledPartialSamples samples{};
        std::array<NormalPartialDspInputs,2> dsp{};
        std::array<VoiceStopState,voiceCapacity> lifecycle{};
        std::array<uint8_t,voiceCapacity> activity{};
        VoiceLinks links;
        links.second[pair[0]]=uint8_t(pair[1]);links.first[pair[1]]=uint8_t(pair[0]);
        // Real envelope completion clears PCM pairing before task1 reclaims
        // the group; a queued mono restart may reuse both physical slots.
        Writer writer;uint16_t stage=14;
        check(FinishEnvelopeTermination(pair[0],stage,activity[pair[0]],links,writer));
        check(writer.writes==1);
        for(unsigned i=0;i<2;++i) {
            selection.partials.partials[i].emplace();samples[i].emplace();
            samples[i]->slot=uint8_t(pair[i]); samples[i]->installed.emplace();
            samples[i]->installed->input.partial=uint8_t(i);
            lifecycle[pair[i]].pendingOperation=VoiceOperation::prepare;
        }
        auto result=DispatchNormalVoiceInputs(selection,samples,dsp,lifecycle,links,activity);
        check(result && result->count==2);
        check(result->entries[0].slot==std::max(pair[0],pair[1]));
        check(result->entries[1].slot==std::min(pair[0],pair[1]));
        check(!result->entries[0].linkedToPrevious && !result->entries[1].linkedToPrevious);
        for(auto slot:pair)check(lifecycle[slot].pendingOperation==VoiceOperation::none);
        // Do not consume half a note if unrelated work intervenes.
        lifecycle[pair[0]].pendingOperation=VoiceOperation::prepare;
        lifecycle[pair[1]].pendingOperation=VoiceOperation::finishStop;
        activity[pair[1]]=1;
        lifecycle[pair[1]].stages.fill(0x12);
        check(!DispatchNormalVoiceInputs(selection,samples,dsp,lifecycle,links,activity));
        check(lifecycle[pair[0]].pendingOperation==VoiceOperation::prepare);
        check(lifecycle[pair[1]].pendingOperation==VoiceOperation::finishStop);
        check(activity[pair[1]]==1 && lifecycle[pair[1]].stages[0]==0x12);
        // A linked pair retains its firmware order even if that is not
        // descending slot order, and still shares its preparation context.
        lifecycle[pair[1]].pendingOperation=VoiceOperation::prepare;
        links.second[pair[0]]=uint8_t(pair[1]);links.first[pair[1]]=uint8_t(pair[0]);
        result=DispatchNormalVoiceInputs(selection,samples,dsp,lifecycle,links,activity);
        check(result && result->entries[0].slot==pair[0] && result->entries[1].slot==pair[1]);
        check(!result->entries[0].linkedToPrevious && result->entries[1].linkedToPrevious);
    }
    std::cout<<"Detached voice preparation and atomic mismatch handling PASS\n";
}
