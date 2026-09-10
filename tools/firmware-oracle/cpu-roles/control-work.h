#pragma once
#include <map>
#include <tuple>
#include <limits>
#include <algorithm>
#include <vector>
#include "sc55_controller_scale.h"
#include "envelope-conversion-work.h"
#include "control-stage-work.h"
#include "filter-control-work.h"
#include "pitch-control.h"
#include "voice-output.h"
#include "amplitude-control.h"
#include "modulation-control.h"
#include "modulation-routing.h"
#include "first-modulation-routing.h"

// Read-only task8 routine accounting. Call-site/return-site pairs delimit real
// invocations, including linked voices; no CPU timing or state is changed.
struct ControlWorkProbe
{
    struct Stats {
        uint64_t count=0,own=0,elapsed=0,exclusive=0,interrupt=0;
        uint64_t minimum=std::numeric_limits<uint64_t>::max(),maximum=0;
        uint64_t exclusiveMinimum=std::numeric_limits<uint64_t>::max(),exclusiveMaximum=0;
    };
    using Key=std::tuple<unsigned,unsigned,unsigned,unsigned>; // routine,state,elapsed ticks,voices
    std::map<Key,Stats> groups;
    bool active=false,entered=false;
    unsigned returnPc=0;
    Key key{};
    uint64_t start=0,own=0;
    uint64_t exclusive=0,interrupt=0;
    // Sampled pre-Step PCs, not exact interrupt-aware instruction attribution.
    // Deliberately keep ISR pages visible instead of treating task ID as proof
    // that all elapsed instructions belong to the voice-control calculation.
    std::map<unsigned,uint64_t> envelopePages;
    std::map<unsigned,uint64_t> exclusiveEnvelopePages;
    unsigned instructionAddress=0;
    uint64_t predictedControllerCycles=0,controllerChecks=0;
    std::map<uint64_t,uint64_t> controllerBudgets;
    std::optional<sc55::SecondEnvelopePcmTables> conversionTables;
    std::optional<EnvelopeConversionWork> conversion;
    unsigned conversionBase=0,conversionInstructions=0;
    std::map<unsigned,uint64_t> conversionBudgets;
    std::optional<SecondEnvelopeOutputWork> envelopeOutput;
    unsigned outputBase=0,outputInstructions=0;
    std::map<unsigned,uint64_t> outputBudgets;
    // Observe the physical amplitude hold between readback and publication.
    // This is wall-time evidence, not a schedule to replay in the product.
    struct RampHold { uint64_t cycle; uint16_t level,clock; unsigned events=0; };
    std::array<std::optional<RampHold>,24> rampHolds{};
    uint64_t holdCount=0,holdCycles=0,holdMinimum=UINT64_MAX,holdMaximum=0;
    uint64_t holdClockSteps=0,holdLevelChanges=0,holdCommandChanges=0;
    uint64_t abandonedHolds=0;
    std::array<uint64_t,5> calculationGates{},stoppedAtGate{};
    ControlStageWork stageWork;
    PitchControlProbe pitchControl;
    VoiceOutputProbe voiceOutput;
    AmplitudeControlProbe amplitudeControl;
    ModulationControlProbe modulationControl;
    ModulationRoutingProbe modulationRouting;
    FirstModulationRoutingProbe firstModulationRouting;
    FirstModulationRoutingProbe firstModulationInitialization{true};
    sc55::EnvelopeTimes filterTimes{};
    std::optional<FilterControlWork> filterWork;
    unsigned filterBase=0,filterInstructions=0;
    unsigned filterEntryStage=0;
    std::array<uint64_t,12> filterStages{};
    std::map<unsigned,uint64_t> filterBudgets;

    // These entry points take the physical slot in r1. Observe all tasks,
    // before task8-only work accounting, to identify who interrupted a hold.
    void observeHoldEvent(mcu_t& cpu)
    {
        if(cpu.cp!=0) return;
        const unsigned event=cpu.pc==0x2928 ? 1u : cpu.pc==0x53e6 ? 2u : cpu.pc==0x113a ? 4u : 0u;
        if(!event) return;
        const auto slot=cpu.r[1];
        if(slot>=24) throw std::runtime_error("Invalid held-voice event slot");
        if(auto& hold=rampHolds[slot]) {
            hold->events|=event;
            std::printf("[DEBUG-held-event] slot=%u event=%u task=%u pc=%04x age=%llu stage=%u\n",
                slot,event,MCU_Read16(cpu,0xfdca),cpu.pc,(unsigned long long)(cpu.cycles-hold->cycle),
                MCU_Read16(cpu,MCU_Read16(cpu,0x676a+2*slot)));
        }
    }

    void observeRampHold(mcu_t& cpu)
    {
        if(cpu.cp!=0 || (cpu.pc!=0x318c && cpu.pc!=0x3212 && cpu.pc!=0x586f)) return;
        const auto base=cpu.r[0];
        const auto slot=MCU_Read16(cpu,uint16_t(base-2));
        if(slot>=24) throw std::runtime_error("Invalid amplitude-hold owner");
        auto& pending=rampHolds[slot];
        if(cpu.pc==0x318c) {
            // Termination can bypass normal publication; do not join two
            // separate updates (or a reused physical slot) into one interval.
            abandonedHolds+=pending.has_value(); pending.reset(); return;
        }
        auto& pcm=*cpu.pcm;
        if(cpu.pc==0x3212) {
            const auto stage=MCU_Read16(cpu,base);
            if(stage>0 && stage<14)
                pending=RampHold{cpu.cycles,uint16_t(pcm.ram2[slot][10]),pcm.tv_counter};
        } else if(pending) {
            const auto duration=cpu.cycles-pending->cycle;
            ++holdCount; holdCycles+=duration;
            holdMinimum=std::min(holdMinimum,duration); holdMaximum=std::max(holdMaximum,duration);
            // The PCM frame counter counts down, modulo 16384.
            holdClockSteps+=unsigned((pending->clock-pcm.tv_counter)&0x3fff);
            holdLevelChanges+=pcm.ram2[slot][10]!=pending->level;
            holdCommandChanges+=pcm.ram2[slot][4]!=0xff00;
            pending.reset();
        }
    }

    // Existing pure arithmetic includes its verified instruction counts. Add
    // setup, part-pointer reloads and the caller's JSR/final RTS, not IRQ time.
    static uint64_t controllerCycles(mcu_t& cpu)
    {
        const auto slot=cpu.r[1];
        if(slot>=24) throw std::runtime_error("Invalid controller probe slot");
        const unsigned part=MCU_Read(cpu,0xc8e4+slot),key=MCU_Read(cpu,0xc8fc+slot);
        if(part>=16 || key>=128) throw std::runtime_error("Invalid controller probe owner");
        const auto base=MCU_Read16(cpu,0x74a4+part*2);
        const auto pressure=MCU_Read(cpu,0x9740+part*128+key);
        unsigned instructions=17;
        for(unsigned i=0;i<11;++i) {
            std::array<uint16_t,5> contributions;
            for(unsigned source=0;source<5;++source)
                contributions[source]=MCU_Read16(cpu,0x9060+source*0x160+i*0x20+part*2);
            instructions+=sc55::ScaleVoiceController(i,MCU_Read(cpu,base+0x4c+i+(i>=3)),
                pressure,contributions).instructions+unsigned(i!=0);
        }
        return instructions*12u;
    }

    void before(mcu_t& cpu)
    {
        instructionAddress=(unsigned(cpu.cp)<<16)|cpu.pc;
        if(cpu.cp!=0 || MCU_Read16(cpu,0xfdca)!=8) return;
        if(active && cpu.pc==std::get<0>(key)) entered=true;
        // 3188 can discard its return address and resume the voice scan.
        if(active && (cpu.pc==returnPc || (std::get<0>(key)==0x3188 && cpu.pc==0x5b5c))) {
            if(std::get<0>(key)==0x5c20) {
                if(own!=predictedControllerCycles || exclusive!=predictedControllerCycles) {
                    std::fprintf(stderr,"[DEBUG-controller-budget] actual=%llu predicted=%llu\n",
                        (unsigned long long)own,(unsigned long long)predictedControllerCycles);
                    throw std::runtime_error("Semantic controller work budget differs from H8");
                }
                ++controllerChecks;
                ++controllerBudgets[predictedControllerCycles];
            }
            auto& s=groups[key]; ++s.count; s.own+=own; s.elapsed+=cpu.cycles-start;
            s.exclusive+=exclusive;s.interrupt+=interrupt;
            s.minimum=std::min(s.minimum,own); s.maximum=std::max(s.maximum,own);
            s.exclusiveMinimum=std::min(s.exclusiveMinimum,exclusive);
            s.exclusiveMaximum=std::max(s.exclusiveMaximum,exclusive);
            active=false;
        }
        unsigned routine=0,next=0;
        switch(cpu.pc) {
        case 0x5b86: routine=0x5c20; next=0x5b89; break;
        case 0x5b99: routine=0x3985; next=0x5b9c; break;
        case 0x5ba4: routine=0x3188; next=0x5ba7; break;
        case 0x5bab: routine=0x5855; next=0x5bae; break;
        case 0x5bce: routine=0x5c20; next=0x5bd0; break;
        case 0x5bd8: routine=0x5ff5; next=0x5bdb; break;
        case 0x5beb: routine=0x3985; next=0x5bee; break;
        case 0x5bf6: routine=0x3d44; next=0x5bf9; break;
        case 0x5c01: routine=0x3188; next=0x5c04; break;
        case 0x5c08: routine=0x3188; next=0x5c0b; break;
        case 0x5c0f: routine=0x5855; next=0x5c12; break;
        case 0x5c16: routine=0x5855; next=0x5c19; break;
        default: return;
        }
        // Step may take an interrupt before executing the call instruction.
        // Returning from that interrupt revisits the same, not-yet-called site.
        if(active && !entered && returnPc==next) return;
        if(active) {
            std::fprintf(stderr,"[DEBUG-control-work] missed return=%04x nextCall=%04x routine=%04x\n",
                returnPc,cpu.pc,std::get<0>(key));
            throw std::runtime_error("Control-work probe missed a routine return");
        }
        key={routine,MCU_Read16(cpu,cpu.r[0]),MCU_Read16(cpu,0xac5a),24-MCU_Read(cpu,0xa3c1)};
        if(routine==0x5c20) predictedControllerCycles=controllerCycles(cpu);
        returnPc=next; start=cpu.cycles; own=exclusive=interrupt=0; active=true; entered=false;
    }
    // Called at the actual interpreter entry, AFTER interrupt dispatch. This
    // does not attribute an interrupt's first instruction to the old task PC.
    void instruction(mcu_t& cpu,bool inHardwareInterrupt)
    {
        stageWork.instruction(cpu,inHardwareInterrupt);
        pitchControl.instruction(cpu,inHardwareInterrupt);
        voiceOutput.instruction(cpu,inHardwareInterrupt);
        amplitudeControl.instruction(cpu,inHardwareInterrupt);
        modulationControl.instruction(cpu,inHardwareInterrupt);
        modulationRouting.instruction(cpu,inHardwareInterrupt);
        firstModulationRouting.instruction(cpu,inHardwareInterrupt);
        firstModulationInitialization.instruction(cpu,inHardwareInterrupt);
        observeHoldEvent(cpu);
        if(!active || MCU_Read16(cpu,0xfdca)!=8) return;
        if(inHardwareInterrupt) {interrupt+=12;return;}
        if(cpu.cp==0 && cpu.pc<0x752) return;
        observeRampHold(cpu);
        if(cpu.cp==0) {
            constexpr std::array<unsigned,5> gates{0x32fc,0x3312,0x3328,0x333e,0x3354};
            for(unsigned i=0;i<gates.size();++i) if(cpu.pc==gates[i]) {
                ++calculationGates[i];
                const auto stage=MCU_Read16(cpu,cpu.r[0]);
                stoppedAtGate[i]+=stage>=14;
                if(stage>=14) {
                    const auto slot=MCU_Read16(cpu,uint16_t(cpu.r[0]-2));
                    if(slot>=24) throw std::runtime_error("Invalid stopped calculation slot");
                    std::printf("[DEBUG-held-stop] phase=%u slot=%u stage=%u events=%u\n",
                        i,slot,stage,rampHolds[slot] ? rampHolds[slot]->events : 0);
                }
            }
        }
        exclusive+=12;
        if(cpu.cp==0 && (cpu.pc==0x4443 || cpu.pc==0x4662 || cpu.pc==0x473c) && !conversionTables) {
            conversionTables.emplace();
            for(unsigned i=0;i<129;++i) conversionTables->levelCurve[i]=MCU_Read16(cpu,0x7612+2*i);
            for(unsigned i=0;i<128;++i) conversionTables->outputCeiling[i]=MCU_Read(cpu,0x7816+i);
            for(unsigned i=0;i<256;++i) conversionTables->smoothingCeiling[i]=MCU_Read(cpu,0x7714+i);
            for(unsigned i=0;i<128;++i) filterTimes[i]=MCU_Read16(cpu,0x6f12+2*i);
        }
        if(cpu.cp==0 && cpu.pc==0x4443) {
            if(filterWork) throw std::runtime_error("Overlapping complete filter calculation");
            filterBase=cpu.r[0]; filterInstructions=0;
            const auto word=[&](int offset) { return MCU_Read16(cpu,uint16_t(filterBase+offset)); };
            const auto byte=[&](int offset) { return MCU_Read(cpu,uint16_t(filterBase+offset)); };
            const auto part=word(0x2e);
            sc55::SecondEnvelopeReleaseState state;
            state.stage=word(2); state.progress={word(0x0a),word(0x14)};
            filterEntryStage=state.stage;
            state.parameter=byte(0x4a); state.releaseParameter=byte(0x4e);
            state.level=word(0x20); state.start=word(0x54); state.target=word(0x56);
            state.releaseTarget=word(0x5e); state.stepTime=word(-0x30);
            for(unsigned i=0;i<3;++i) { state.nextParameters[i]=byte(0x4b+i); state.nextTargets[i]=word(0x58+2*i); }
            sc55::SecondEnvelopeTiming timing{word(-0x2c),word(-0x2a),word(-0x28),word(-0x26),
                MCU_Read(cpu,part+0x14),MCU_Read(cpu,part+0x15),MCU_Read(cpu,part+0x16),bool(byte(0xa2)&16)};
            sc55::SecondEnvelopeOutputInputs input;
            input.base=byte(0xa3); input.control=MCU_Read(cpu,part+0x12);
            input.suppressPositiveControl=bool(byte(0xa2)&4); input.offset=word(0x88);
            input.sources[0]={word(-0x78),word(0x8c),word(-0x60)};
            input.sources[1]={word(-0x56),word(0x94),word(-0x3e)};
            filterWork=FilterControlWork::evaluate(state,{byte(0x68),word(0x22),word(0x24),word(0x26)},
                byte(0x65)!=0,MCU_Read16(cpu,0xac5a),timing,filterTimes,input,byte(0x67),
                MCU_Read(cpu,part+0x13),byte(0x69),*conversionTables);
            if(!filterWork) throw std::runtime_error("Invalid complete filter calculation input");
        }
        if(filterWork && cpu.cp==0 && cpu.pc==0x3332) {
            const auto word=[&](unsigned offset) { return MCU_Read16(cpu,uint16_t(filterBase+offset)); };
            const auto& expected=*filterWork;
            if(filterInstructions!=expected.instructions || word(2)!=expected.segment.stage
                || word(0x0a)!=expected.segment.progress.position || word(0x14)!=expected.segment.progress.deferredTicks
                || MCU_Read(cpu,filterBase+0x4a)!=expected.segment.parameter
                || word(0x54)!=expected.segment.start || word(0x56)!=expected.segment.target
                || MCU_Read16(cpu,uint16_t(filterBase-0x30))!=expected.segment.stepTime
                || word(0x20)!=expected.segment.level || word(0x22)!=expected.pcm.output
                || word(0x24)!=expected.pcm.level || word(0x26)!=expected.pcm.command
                || MCU_Read(cpu,filterBase+0x68)!=expected.pcm.control) {
                std::fprintf(stderr,"[DEBUG-filter-work] instructions=%u/%u stage=%u/%u level=%04x/%04x output=%04x/%04x command=%04x/%04x\n",
                    filterInstructions,expected.instructions,word(2),expected.segment.stage,word(0x20),expected.segment.level,
                    word(0x22),expected.pcm.output,word(0x26),expected.pcm.command);
                throw std::runtime_error("Complete native filter value/work differs from H8");
            }
            ++filterBudgets[filterInstructions];
            if(filterEntryStage<=22 && !(filterEntryStage&1)) ++filterStages[filterEntryStage/2];
            filterWork.reset();
        }
        if(filterWork) ++filterInstructions;
        if(cpu.cp==0 && cpu.pc==0x4662) {
            if(envelopeOutput) throw std::runtime_error("Second-envelope output overlapped an unfinished call");
            outputBase=cpu.r[0];outputInstructions=0;
            const auto part=MCU_Read16(cpu,outputBase+0x2e);
            sc55::SecondEnvelopeOutputInputs input;
            input.base=MCU_Read(cpu,outputBase+0xa3);input.control=MCU_Read(cpu,part+0x12);
            input.suppressPositiveControl=bool(MCU_Read(cpu,outputBase+0xa2)&4);
            input.offset=MCU_Read16(cpu,outputBase+0x88);
            input.sources[0]={MCU_Read16(cpu,outputBase-0x78),MCU_Read16(cpu,outputBase+0x8c),MCU_Read16(cpu,outputBase-0x60)};
            input.sources[1]={MCU_Read16(cpu,outputBase-0x56),MCU_Read16(cpu,outputBase+0x94),MCU_Read16(cpu,outputBase-0x3e)};
            envelopeOutput=SecondEnvelopeOutputWork::evaluate(cpu.r[5],input,MCU_Read(cpu,outputBase+0x68),
                MCU_Read(cpu,outputBase+0x67),MCU_Read(cpu,part+0x13),MCU_Read(cpu,outputBase+0x69),
                MCU_Read16(cpu,outputBase+0x24),MCU_Read16(cpu,outputBase-0x30),*conversionTables);
            if(!envelopeOutput) throw std::runtime_error("Invalid second-envelope output work input");
        }
        if(envelopeOutput) {
            ++outputInstructions;
            if(cpu.cp==0 && (cpu.pc==0x47ee || cpu.pc==0x47f4 || cpu.pc==0x47fa)) {
                if(outputInstructions!=envelopeOutput->instructions
                    || MCU_Read16(cpu,outputBase+0x22)!=envelopeOutput->output
                    || MCU_Read16(cpu,outputBase+0x24)!=envelopeOutput->conversion.level
                    || MCU_Read16(cpu,outputBase+0x26)!=envelopeOutput->conversion.command
                    || MCU_Read(cpu,outputBase+0x68)!=envelopeOutput->conversion.control) {
                    std::fprintf(stderr,"[DEBUG-output-work] actual=%u predicted=%u output=%04x/%04x level=%04x/%04x command=%04x/%04x\n",
                        outputInstructions,envelopeOutput->instructions,MCU_Read16(cpu,outputBase+0x22),envelopeOutput->output,
                        MCU_Read16(cpu,outputBase+0x24),envelopeOutput->conversion.level,
                        MCU_Read16(cpu,outputBase+0x26),envelopeOutput->conversion.command);
                    throw std::runtime_error("Second-envelope output value/work differs from H8");
                }
                ++outputBudgets[outputInstructions];envelopeOutput.reset();
            }
        }
        if(cpu.cp==0 && cpu.pc==0x473c) {
            if(conversion) throw std::runtime_error("Envelope conversion entry overlapped an unfinished call");
            conversionBase=cpu.r[0];conversionInstructions=0;
            conversion=EnvelopeConversionWork::evaluate(MCU_Read16(cpu,conversionBase+0x22),
                MCU_Read16(cpu,conversionBase+0x24),MCU_Read(cpu,conversionBase+0x68),
                MCU_Read16(cpu,conversionBase-0x30),*conversionTables);
            if(!conversion) throw std::runtime_error("Invalid input to semantic envelope conversion work");
        }
        if(conversion) {
            ++conversionInstructions;
            if(cpu.cp==0 && (cpu.pc==0x47ee || cpu.pc==0x47f4 || cpu.pc==0x47fa)) {
                if(conversionInstructions!=conversion->instructions
                    || MCU_Read16(cpu,conversionBase+0x24)!=conversion->level
                    || MCU_Read16(cpu,conversionBase+0x26)!=conversion->command
                    || MCU_Read(cpu,conversionBase+0x68)!=conversion->control) {
                    std::fprintf(stderr,"[DEBUG-envelope-work] actual=%u predicted=%u level=%04x/%04x command=%04x/%04x\n",
                        conversionInstructions,conversion->instructions,MCU_Read16(cpu,conversionBase+0x24),conversion->level,
                        MCU_Read16(cpu,conversionBase+0x26),conversion->command);
                    throw std::runtime_error("Envelope conversion value/work differs from H8");
                }
                ++conversionBudgets[conversionInstructions];conversion.reset();
            }
        }
        if(std::get<0>(key)==0x3188)
            exclusiveEnvelopePages[((unsigned(cpu.cp)<<16)|cpu.pc)&~255u]+=12;
    }
    void after(uint64_t cycles,unsigned task,bool sleeping,bool kernelPc)
    {
        if(active && task==8 && !sleeping && !kernelPc) {
            own+=cycles;
            if(std::get<0>(key)==0x3188) envelopePages[instructionAddress&~255u]+=cycles;
        }
    }
    void report() const
    {
        stageWork.report();
        pitchControl.report();
        voiceOutput.report();
        amplitudeControl.report();
        modulationControl.report();
        modulationRouting.report();
        firstModulationRouting.report();
        firstModulationInitialization.report();
        uint64_t filterChecks=0;
        for(const auto& [cost,count]:filterBudgets) filterChecks+=count;
        std::printf("[DEBUG-filter-work] %llu complete value/work comparisons, %zu budgets PASS\n",
            (unsigned long long)filterChecks,filterBudgets.size());
        for(unsigned i=0;i<filterStages.size();++i) if(filterStages[i])
            std::printf("[DEBUG-filter-stage] stage=%u count=%llu\n",2*i,(unsigned long long)filterStages[i]);
        for(unsigned i=0;i<calculationGates.size();++i)
            std::printf("[DEBUG-control-gate] phase=%u entered=%llu stopped=%llu\n",i,
                (unsigned long long)calculationGates[i],(unsigned long long)stoppedAtGate[i]);
        std::printf("[DEBUG-ramp-hold] n=%llu cycles mean=%llu min=%llu max=%llu PCMsteps=%llu levelChanges=%llu commandChanges=%llu unpublished=%llu\n",
            (unsigned long long)holdCount,(unsigned long long)(holdCount ? holdCycles/holdCount : 0),
            (unsigned long long)(holdCount ? holdMinimum : 0),(unsigned long long)holdMaximum,
            (unsigned long long)holdClockSteps,(unsigned long long)holdLevelChanges,
            (unsigned long long)holdCommandChanges,(unsigned long long)abandonedHolds);
        uint64_t conversionChecks=0;
        for(const auto& [cost,count]:conversionBudgets) conversionChecks+=count;
        std::printf("[DEBUG-envelope-work] %llu value/work comparisons, %zu budgets PASS\n",
            (unsigned long long)conversionChecks,conversionBudgets.size());
        for(const auto& [cost,count]:conversionBudgets)
            std::printf("[DEBUG-envelope-budget] instructions=%u count=%llu\n",cost,(unsigned long long)count);
        uint64_t outputChecks=0;
        for(const auto& [cost,count]:outputBudgets) outputChecks+=count;
        std::printf("[DEBUG-output-work] %llu value/work comparisons, %zu budgets PASS\n",
            (unsigned long long)outputChecks,outputBudgets.size());
        if(!outputBudgets.empty())
            std::printf("[DEBUG-output-budget] instructions min=%u max=%u\n",outputBudgets.begin()->first,outputBudgets.rbegin()->first);
        std::printf("[DEBUG-controller-budget] %llu input-derived work comparisons PASS\n",
            (unsigned long long)controllerChecks);
        for(const auto& [cycles,count]:controllerBudgets)
            std::printf("[DEBUG-controller-budget] cycles=%llu count=%llu\n",
                (unsigned long long)cycles,(unsigned long long)count);
        std::map<unsigned,Stats> totals;
        for(const auto& [key,s]:groups) {
            const auto [routine,state,ticks,voices]=key;
            auto& total=totals[routine];
            total.count+=s.count; total.own+=s.own; total.elapsed+=s.elapsed;
            total.exclusive+=s.exclusive;total.interrupt+=s.interrupt;
            if(voices==24)
                std::printf("[DEBUG-control-work] routine=%04x state=%u ticks=%u voices=%u n=%llu ownMean=%llu ownMin=%llu ownMax=%llu exclusiveMean=%llu exclusiveMin=%llu exclusiveMax=%llu elapsedMean=%llu\n",
                    routine,state,ticks,voices,(unsigned long long)s.count,(unsigned long long)(s.own/s.count),
                    (unsigned long long)s.minimum,(unsigned long long)s.maximum,
                    (unsigned long long)(s.exclusive/s.count),(unsigned long long)s.exclusiveMinimum,
                    (unsigned long long)s.exclusiveMaximum,(unsigned long long)(s.elapsed/s.count));
        }
        for(const auto& [routine,s]:totals)
            std::printf("[DEBUG-control-total] routine=%04x n=%llu sampled=%llu exclusive=%llu irq=%llu elapsed=%llu\n",routine,
                (unsigned long long)s.count,(unsigned long long)s.own,(unsigned long long)s.exclusive,
                (unsigned long long)s.interrupt,(unsigned long long)s.elapsed);
        std::vector<std::pair<uint64_t,unsigned>> pages;
        for(const auto& [address,cycles]:envelopePages) pages.emplace_back(cycles,address);
        std::sort(pages.rbegin(),pages.rend());
        for(unsigned i=0;i<std::min<std::size_t>(16,pages.size());++i)
            std::printf("[DEBUG-control-envelope-page] address=%02x:%04x cycles=%llu\n",
                pages[i].second>>16,pages[i].second&65535,(unsigned long long)pages[i].first);
        pages.clear();
        for(const auto& [address,cycles]:exclusiveEnvelopePages) pages.emplace_back(cycles,address);
        std::sort(pages.rbegin(),pages.rend());
        for(unsigned i=0;i<std::min<std::size_t>(16,pages.size());++i)
            std::printf("[DEBUG-control-exclusive-page] address=%02x:%04x cycles=%llu\n",
                pages[i].second>>16,pages[i].second&65535,(unsigned long long)pages[i].first);
    }
};
