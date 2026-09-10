#include "sc55_signal_renderer.h"
#include <array>
#include <memory>
#include <cstdio>

int main()
{
    PCMSim_Init();
    auto engine=std::make_unique<sc55::SignalRenderer>();
    engine->initializeClock(0x1273,0x497b,99);
    std::array<uint8_t,512> samples{};
    std::array<uint8_t,32> exponents{};
    for(unsigned i=0;i<samples.size();++i) samples[i]=uint8_t(int(i%9)-4);
    exponents.fill(0x55);
    for(unsigned slot=0;slot<24;++slot) {
        PCMSim_SetWaveform(engine->voices,slot,{samples.data(),exponents.data(),511,31,32,256,0,false,false});
        PCMSim_RestartVoice(engine->voices,slot,32,0,false);
        PCMSim_ApplyVoiceUpdate(engine->voices,slot,{uint16_t(0x2800+slot*137),{0xff00,0xff00,0xff00},16,32,32,32,48,1});
        for(auto& ramp:engine->voices.envelopes[slot].ramps) ramp.level=0x4000;
        engine->voices.gate[slot]=slot<8 ? 1.0f : 0.0f;
        engine->adoptVoiceGate(slot,slot<8,true,32,0,false);
    }
    auto& fx=engine->effects;
    const PCMEffectsSettings::Coefficient coefficient{0.5f,0.25f,0.5f,true};
    fx.settings.reverbInput=fx.settings.chorusInput=fx.settings.comb=coefficient;
    fx.settings.diffusion.fill(coefficient); fx.settings.damping.fill(coefficient);
    fx.settings.reverbReturn.fill(coefficient); fx.settings.chorusReturn.fill(coefficient);
    for(unsigned i=0;i<12;++i) {
        fx.settings.diffusionTaps[i]=uint16_t(i*11); fx.settings.tailTaps[i]=uint16_t(i*17);
    }
    fx.chorus={128,128,256,0,137,false,true,false}; fx.spread={0xff00,0x4000};
    auto referenceEffects=std::make_unique<PCMEffects>(fx);
    auto referenceVoices=engine->voices;
    sc55::AudioBuses previous{};
    double energy=0,tail=0;
    int expectedPending=-1;
    unsigned notifications=0;
    uint16_t random=0x497b;
    for(unsigned frame=0;frame<32768;++frame) {
        if(frame==8192) {
            engine->setVoiceKeys(0);
            for(unsigned slot=0;slot<24;++slot) referenceVoices.gate[slot]=0;
        }
        const sc55::EnvelopeClock clock{uint16_t((0x1272-frame)&0x3fff),frame%17!=0};
        float dry[6]{},sends[6]{},voices[4]{};
        referenceEffects->process(clock,true,float(previous.reverb),float(previous.chorus),dry,sends);
        PCMSim_AdvanceEnvelopes(referenceVoices,clock);
        PCMSim_RenderSignals(referenceVoices,voices);
        const int event=PCMSim_CollectBoundary(referenceVoices,clock.update,expectedPending<0);
        if(event>=0) expectedPending=event;
        int a[6],b[6];
        for(unsigned i=0;i<6;++i) { a[i]=int(std::lrint(dry[i])); b[i]=int(std::lrint(sends[i])); }
        const auto output=engine->render(true,clock.update);
        const auto bit=((random>>0)^(random>>1)^(random>>7)^(random>>12))&1;
        random=uint16_t((random>>1)|(bit<<15));
        if(engine->renderedFrames()!=frame+100 || engine->envelopePhase()!=clock.phase
            || engine->randomWord()!=random || engine->randomWord()!=random) return 5;
        if(output.boundaryVoice!=event || engine->hasVoiceBoundary()!=(expectedPending>=0)) return 3;
        if(frame%19==0) {
            if(engine->takeVoiceBoundary()!=expectedPending || engine->hasVoiceBoundary()) return 4;
            notifications+=expectedPending>=0;
            expectedPending=-1;
        }
        if(output.output.left!=previous.left || output.output.right!=previous.right
            || output.output.reverb!=previous.reverb || output.output.chorus!=previous.chorus
            || std::memcmp(engine->effects.delay,referenceEffects->delay,sizeof(engine->effects.delay))) return 1;
        previous=sc55::MixVoiceAndEffectBuses(voices,a,b);
        const auto e=double(output.output.left)*output.output.left+double(output.output.right)*output.output.right;
        energy+=e; if(frame>8192) tail+=e;
    }
    if(energy==0 || tail==0 || notifications==0) return 2;
    // Direct owner contract for newly started and stolen/reused voices. The
    // initial commands must survive a non-updating pass and key latch before
    // the first active EG pass. This needs no PCM registers or emulated clock.
    auto startup=std::make_unique<sc55::SignalRenderer>();
    const PCMSimWaveform waveform{samples.data(),exponents.data(),511,31,32,256,0,false,false};
    for(unsigned reuse=0;reuse<2;++reuse) {
        startup->setVoiceKeys(0);
        startup->render(false);
        startup->updateVoice(0,{0x4000,{0x7fba,0x21ba,0},16,32,0,0,0,0});
        startup->installVoice(0,waveform,32,0);
        startup->setVoiceKeys(1);
        startup->render(false,false);
        if(startup->voiceReady(0)) return 6;
        startup->render(false);
        if(startup->voiceReady(0)) return 7;
        startup->render(false);
        if(!startup->voiceReady(0) || startup->voices.envelopes[0].ramps[0].level!=0x3f80
            || startup->voices.envelopes[0].ramps[1].level!=0x1080) return 8;
    }
    // Shared pitch routing belongs to the renderer, including sources that
    // are not sounding voices. No compatibility register object is present.
    startup->setVoicePitchSource(0,7);
    startup->setVoicePitchSource(1,7);
    startup->setChorusPitchSource(7);
    startup->setPitchSource(7,1234);
    if(startup->voices.phase_step[0]!=1234 || startup->voices.phase_step[1]!=1234
        || startup->effects.chorus.increment!=1234) return 9;
    sc55::VoiceRenderUpdate control{};
    control.phaseIncrement=5678;
    startup->updateVoice(0,control);
    if(startup->voices.phase_step[0]!=1234 || startup->voices.phase_step[1]!=1234) return 10;
    startup->setVoicePitchSource(1,0);
    if(startup->voices.phase_step[1]!=5678) return 11;
    startup->setChorusPitchSource(31);
    startup->setPitchSource(31,3456);
    startup->setPitchSource(7,2222);
    if(startup->voices.phase_step[0]!=2222 || startup->voices.phase_step[1]!=5678
        || startup->effects.chorus.increment!=3456) return 12;
    auto ownedOutput=std::make_unique<sc55::SignalRenderer>();
    auto explicitOutput=std::make_unique<sc55::SignalRenderer>();
    for(auto* renderer:{ownedOutput.get(),explicitOutput.get()}) {
        renderer->updateVoice(0,{0x4000,{0x7fba,0x21ba,0},16,32,0,0,0,0});
        renderer->installVoice(0,waveform,32,0);
        renderer->setVoiceKeys(1);
    }
    ownedOutput->adoptFrameState(false,false);
    for(unsigned frame=0;frame<16;++frame) {
        const auto expected=explicitOutput->render(frame>=2,frame!=0).output;
        const auto actual=ownedOutput->nextFrame();
        if(actual.left!=int32_t(uint32_t(expected.left)<<12)
            || actual.right!=int32_t(uint32_t(expected.right)<<12)
            || ownedOutput->renderedFrames()!=frame+1
            || ownedOutput->randomWord()!=explicitOutput->randomWord()
            || !ownedOutput->updatesEnvelopes()) return 13;
    }
    std::puts("Independent signal engine: output delay, effect tail, startup and owned pitch routing match");
}
