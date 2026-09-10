#pragma once
#include "sc55_effects_control.h"
#include "sc55_sound_data_import.h"

// Observe real task-8 requests, predict their result from the entry state, and
// compare at the firmware's return to its event loop. Never replace a CPU step.
inline void VerifyEffectsRequests(Emulator& emu)
{
    auto& cpu=emu.GetMCU();
    const auto tables=sc55::ImportEffectsTables(
        std::vector<uint8_t>(cpu.rom1,cpu.rom1+32768),
        std::vector<uint8_t>(cpu.rom2,cpu.rom2+262144));
    // Force each PCM readback to lag once. No polling loop is allowed inside
    // one native service call, and no output coefficients may publish early.
    {
        using Chorus=sc55::EffectsControl::Chorus;
        Chorus c; c.phase=6; c.parameters={3,64,17,27,80,9,31};
        unsigned readyThrough=0, reads=0, outputWrites=0;
        std::map<unsigned,unsigned> addresses;
        const auto read=[&](unsigned,unsigned reg) {
            ++reads;
            if (reg==0x1e) return readyThrough>=1 ? 0x7fu : 0u;
            if (reg==0x04) return readyThrough>=2 ? 0x3802u+6*27 : 0u;
            return readyThrough>=3 ? 0u : 1u;
        };
        const auto write=[&](unsigned,unsigned reg,unsigned) {
            if (reg==0x14 || reg==0x16 || reg==0x1a) ++outputWrites;
        };
        const auto address=[&](unsigned bank,unsigned reg,unsigned value) {
            if (bank!=31) throw std::runtime_error("Wrong chorus address bank");
            addresses[reg]=value;
        };
        for (readyThrough=0;readyThrough<4;++readyThrough) {
            reads=0;
            if (!c.advanceSetup(tables,read,write,address) || reads>3)
                throw std::runtime_error("Unbounded/unhandled chorus setup");
            if (readyThrough<3 && (c.phase!=6 || outputWrites))
                throw std::runtime_error("Chorus setup published before PCM readiness");
        }
        if (c.phase!=8 || c.setup!=Chorus::Setup::idle || outputWrites!=3
            || addresses[0x08]!=0x3801+6*27 || addresses[0x04]!=0x3802+6*27
            || addresses[0x0c]!=0x3801+6*27+10*9+10)
            throw std::runtime_error("Chorus setup readback resume failed");
    }
    sc55::EffectsControl expected;
    unsigned pending=0, checked=0, reverbFull=0, chorusFull=0, reverbDiff=0, chorusDiff=0;
    sc55::EffectsControl drain;
    unsigned drainPending=0, drainChecked=0, coefficientChecked=0, setupChecked=0;
    unsigned reverbCoefficientChecked=0,reverbSetupChecked=0;
    bool coefficientPending=false;
    bool setupPending=false;
    std::vector<std::array<unsigned,3>> writes;
    const auto run=[&](unsigned duration) {
        const auto end=cpu.cycles+duration;
        while (cpu.cycles<end)
        {
            if (cpu.cp==0 && (cpu.pc==0x604a || cpu.pc==0x6412))
            {
                const bool reverb=cpu.pc==0x604a;
                writes.clear();
                const auto writer=[&](unsigned bank,unsigned reg,unsigned value) {
                    writes.push_back({bank,reg,value});
                };
                if (reverb) {
                    drain.reverb.phase=MCU_Read16(cpu,0xcb6c);
                    drain.reverb.output=MCU_Read16(cpu,0xcb5e);
                    drain.reverb.lpf=MCU_Read16(cpu,0xcb60);
                    drain.reverb.lpfTarget=MCU_Read16(cpu,0xcb62);
                    drain.reverb.dirty=MCU_Read(cpu,0xcb70);
                    for (unsigned i=0;i<6;++i) drain.reverb.parameters[i]=MCU_Read(cpu,0xcb51+i);
                    drain.reverb.drainTicks=MCU_Read(cpu,0xcb72);
                    if (drain.reverb.advanceDrain(writer)) drainPending=1;
                    else if (drain.reverb.advanceSetup(tables,writer))
                    { drainPending=1; setupPending=true; }
                    else if (drain.reverb.advanceCoefficients(tables,MCU_Read(cpu,0x802b),writer))
                    { drainPending=1; coefficientPending=true; }
                } else {
                    drain.chorus.phase=MCU_Read16(cpu,0xcb6e);
                    for (unsigned i=0;i<3;++i) drain.chorus.output[i]=MCU_Read(cpu,0xcb64+i);
                    drain.chorus.lpf=MCU_Read16(cpu,0xcb68);
                    drain.chorus.lpfTarget=MCU_Read16(cpu,0xcb6a);
                    drain.chorus.dirty=MCU_Read(cpu,0xcb71);
                    for (unsigned i=0;i<7;++i) drain.chorus.parameters[i]=MCU_Read(cpu,0xcb57+i);
                    drain.chorus.drainTicks=MCU_Read(cpu,0xcb73);
                    if (drain.chorus.advanceDrain(writer)) drainPending=2;
                    else if (drain.chorus.advanceCoefficients(tables,writer))
                    { drainPending=2; coefficientPending=true; }
                    else if (drain.chorus.phase==6) {
                        const auto ready=[&](unsigned,unsigned reg) {
                            return reg==0x1e ? 0x7fu : reg==0x04
                                ? 0x3802u+6u*drain.chorus.parameters[3] : 0u;
                        };
                        drain.chorus.setup=sc55::EffectsControl::Chorus::Setup::idle;
                        drain.chorus.advanceSetup(tables,ready,writer,
                            [](unsigned,unsigned,unsigned) {});
                        drainPending=2; setupPending=true;
                    }
                }
            }
            if (drainPending && cpu.cp==0 && cpu.pc==(drainPending==1 ? 0x5b02 : 0x5b0b))
            {
                const bool reverb=drainPending==1;
                bool match=reverb
                    ? drain.reverb.phase==MCU_Read16(cpu,0xcb6c)
                        && drain.reverb.output==MCU_Read16(cpu,0xcb5e)
                        && drain.reverb.lpf==MCU_Read16(cpu,0xcb60)
                        && drain.reverb.lpfTarget==MCU_Read16(cpu,0xcb62)
                        && drain.reverb.dirty==MCU_Read(cpu,0xcb70)
                        && drain.reverb.drainTicks==MCU_Read(cpu,0xcb72)
                    : drain.chorus.phase==MCU_Read16(cpu,0xcb6e)
                        && drain.chorus.lpf==MCU_Read16(cpu,0xcb68)
                        && drain.chorus.lpfTarget==MCU_Read16(cpu,0xcb6a)
                        && drain.chorus.dirty==MCU_Read(cpu,0xcb71)
                        && drain.chorus.drainTicks==MCU_Read(cpu,0xcb73);
                if (!reverb) for (unsigned i=0;i<3;++i)
                    match &= drain.chorus.output[i]==MCU_Read(cpu,0xcb64+i);
                // Compare taps as well as coefficients. Exclude only the
                // PCM-mutated chorus mask/phase, not all delay-address banks.
                for (unsigned i=0;i<writes.size();++i) {
                    const auto write=writes[i];
                    bool last=true;
                    for (unsigned j=i+1;j<writes.size();++j)
                        if (writes[j][0]==write[0] && writes[j][1]==write[1]) last=false;
                    const unsigned index=((write[1]>>1)&7)|((write[1]&32)?8:0);
                    if (last && !(write[0]==31 && (index==7 || index==8)))
                        match &= cpu.pcm->ram2[write[0]][index]==write[2];
                }
                if (!match) throw std::runtime_error("Native effects drain differs from H8/PCM");
                if (setupPending) ++(reverb ? reverbSetupChecked : setupChecked);
                else if (coefficientPending) ++(reverb ? reverbCoefficientChecked : coefficientChecked);
                else ++drainChecked;
                drainPending=0; coefficientPending=false; setupPending=false;
            }
            if (cpu.cp==0 && (cpu.pc==0x5954 || cpu.pc==0x5a2a))
            {
                if (pending) throw std::runtime_error("Nested effects request");
                pending=cpu.pc==0x5a2a ? 1 : 2;
                if (pending==1)
                {
                    std::array<uint8_t,6> raw{};
                    for (unsigned i=0;i<6;++i) {
                        raw[i]=MCU_Read(cpu,0x802b+i);
                        expected.reverb.parameters[i]=MCU_Read(cpu,0xcb51+i);
                    }
                    expected.reverb.phase=MCU_Read16(cpu,0xcb6c);
                    expected.reverb.dirty=MCU_Read(cpu,0xcb70);
                    expected.reverb.request(raw);
                }
                else
                {
                    std::array<uint8_t,7> raw{};
                    for (unsigned i=0;i<7;++i) {
                        raw[i]=MCU_Read(cpu,0x8033+i);
                        expected.chorus.parameters[i]=MCU_Read(cpu,0xcb57+i);
                    }
                    expected.chorus.phase=MCU_Read16(cpu,0xcb6e);
                    expected.chorus.dirty=MCU_Read(cpu,0xcb71);
                    expected.chorus.request(raw);
                }
            }
            if (pending && cpu.cp==0 && cpu.pc==0x5942)
            {
                const bool reverb=pending==1;
                const auto phase=reverb ? expected.reverb.phase : expected.chorus.phase;
                const auto dirty=reverb ? expected.reverb.dirty : expected.chorus.dirty;
                bool match=phase==MCU_Read16(cpu,reverb ? 0xcb6c : 0xcb6e)
                    && dirty==MCU_Read(cpu,reverb ? 0xcb70 : 0xcb71);
                for (unsigned i=0;i<(reverb?6u:7u);++i)
                    match &= (reverb?expected.reverb.parameters[i]:expected.chorus.parameters[i])
                        == MCU_Read(cpu,(reverb?0xcb51:0xcb57)+i);
                if (!match) throw std::runtime_error("Native effects request differs from H8");
                ++checked;
                if (reverb) ++(phase==2 ? reverbFull : reverbDiff);
                else ++(phase==2 ? chorusFull : chorusDiff);
                pending=0;
            }
            emu.Step();
        }
    };
    run(120000000);
    // Settle individual changes, then submit new requests during transitions.
    for (unsigned duration : {20000000u,100000u})
        for (auto item : {std::array<unsigned,2>{0x33,17},{0x31,6},{0x34,90},
            {0x32,3},{0x35,51},{0x36,127},{0x3a,17},{0x3c,27},
            {0x39,4},{0x3b,67},{0x3d,80},{0x3e,9},{0x3f,127},
            {0x33,64},{0x31,0},{0x3a,64},{0x3c,8}})
        {
            const uint8_t message[]{0xf0,0x41,0x10,0x42,0x12,0x40,1,
                uint8_t(item[0]),uint8_t(item[1]),uint8_t((128-((0x41+item[0]+item[1])&127))&127),0xf7};
            emu.PostMIDI(message); run(duration);
        }
    run(20000000);
    // Every reverb character, each LPF entry, both time/feedback extremes,
    // including differential requests after the full setup has settled.
    const auto set=[&](unsigned address,unsigned value) {
        const uint8_t message[]{0xf0,0x41,0x10,0x42,0x12,0x40,1,
            uint8_t(address),uint8_t(value),uint8_t((128-((0x41+address+value)&127))&127),0xf7};
        emu.PostMIDI(message); run(20000000);
    };
    for (unsigned character=0;character<8;++character) {
        set(0x31,character); set(0x32,character);
        set(0x34,0); set(0x34,127); set(0x35,0); set(0x35,127);
        set(0x33,0); set(0x33,127); set(0x36,0); set(0x36,127);
    }
    if (pending || !reverbFull || !chorusFull || !reverbDiff || !chorusDiff)
        throw std::runtime_error("Effects request coverage incomplete");
    std::printf("Native effects requests PASS: %u; reverb full/diff=%u/%u chorus=%u/%u\n",
        checked,reverbFull,reverbDiff,chorusFull,chorusDiff);
    if (!drainChecked || drainPending) throw std::runtime_error("No completed effects drain checks");
    std::printf("Native effects drain PASS: %u periodic calls, state and PCM coefficients\n",drainChecked);
    if (!coefficientChecked) throw std::runtime_error("No chorus coefficient checks");
    std::printf("Native chorus coefficients PASS: %u periodic calls\n",coefficientChecked);
    if (!setupChecked) throw std::runtime_error("No chorus setup checks");
    std::printf("Native chorus setup PASS: %u completed H8 calls (ready-readback prediction)\n",setupChecked);
    if (!reverbCoefficientChecked || !reverbSetupChecked) throw std::runtime_error("Missing reverb setup/coefficients");
    std::printf("Native reverb PASS: %u setup / %u coefficient calls\n",reverbSetupChecked,reverbCoefficientChecked);
}
