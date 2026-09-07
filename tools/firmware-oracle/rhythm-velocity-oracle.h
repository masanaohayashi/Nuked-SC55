#pragma once
#include "sc55_note_dispatch.h"
#include "sc55_sample_install.h"

// Offline comparison only. Both patch banks and curves were imported before
// this call; native preparation never reads MCU memory or executes firmware.
inline void verifyRhythmVelocity(mcu_t& cpu,const sc55::SoundData& data)
{
    std::array<uint8_t,128> accumulators;
    for (unsigned key = 0; key < 128; ++key) accumulators[key] = MCU_Read(cpu,0x3d168+key);
    unsigned cases = 0, accepted = 0, sampleInputChecks = 0;
    for (unsigned tone = 0; tone < data.patchCount(); ++tone)
        for (unsigned program : {0u,127u})
            for (unsigned soft = 0; soft < 2; ++soft)
                for (unsigned velocity = 1; velocity < 128; ++velocity)
                {
                    const unsigned key = tone%128, mapIndex = tone%2;
                    sc55::RhythmKeyMap map;
                    map.tones[key] = uint16_t(tone); map.groups[key] = 7; map.flags[key] = 0x90;
                    const sc55::MidiDecoder::Event event{sc55::MidiDecoder::Kind::message,0x99,uint8_t(key),uint8_t(velocity),2};
                    const auto native = sc55::PrepareRhythmNoteVelocity(event,program,map,accumulators,17,soft != 0,data);
                    if (!native || !native->note || native->note->tone != tone || native->note->note != key)
                        throw std::runtime_error("Rhythm velocity preparation lost mapped tone/key");
                    const unsigned base = 0x8748+mapIndex*0x48c;
                    MCU_Write16(cpu,base+2*key,uint16_t(tone));
                    MCU_Write(cpu,base+0x200+key,7); MCU_Write(cpu,base+0x400+key,0x90);
                    MCU_Write(cpu,0x8049,uint8_t(program)); MCU_Write(cpu,0xa1b7,17);
                    MCU_Write(cpu,0xa3d0,0); MCU_Write(cpu,0xa3d3,uint8_t(velocity));
                    MCU_Write16(cpu,0xab02,uint16_t(soft));
                    cpu.cp = cpu.dp = cpu.ep = 0; cpu.sr = 0; cpu.pc = 0x0c3c;
                    cpu.r[0] = mapIndex; cpu.r[1] = key; cpu.r[2] = 0x8048; cpu.r[7] = 0x9900;
                    unsigned steps = 0;
                    while (cpu.pc != 0x0ca2)
                    {
                        if (++steps > 500) throw std::runtime_error("Rhythm velocity H8 path escaped");
                        const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                    }
                    const auto& plan = native->note->partials;
                    if (MCU_Read(cpu,0xa1b6) != key || MCU_Read(cpu,0xa1bf) != native->mapping.flags
                        || MCU_Read(cpu,0xa3d1) != native->mapping.group
                        || MCU_Read(cpu,0xa1b0) != plan.candidates.flags || MCU_Read(cpu,0xa3d4) != plan.candidates.count
                        || MCU_Read(cpu,0xa1b7) != plan.accumulator || cpu.r[7] != 0x9900)
                        throw std::runtime_error("Rhythm velocity plan differs from H8");
                    for (unsigned partial = 0; partial < 2; ++partial)
                        if (plan.partials[partial])
                        {
                            if (MCU_Read(cpu,0xa1b9+partial) != plan.partials[partial]->amplitude
                                || MCU_Read(cpu,0xa1bc+partial) != plan.partials[partial]->secondary)
                                throw std::runtime_error("Rhythm partial velocity differs from H8");
                            ++accepted;
                        }
                    if (velocity == 100 && program == 0 && soft == 0 && plan.candidates.count)
                    {
                        map.pitches[key] = uint8_t((key+17)%128);
                        std::array<uint8_t,12> scale; scale.fill(64); scale[key%12] = 65;
                        const auto dispatch = sc55::PlanPartialVoiceDispatch(*data.patch(tone),plan.candidates.flags,{255,255});
                        if (!dispatch) throw std::runtime_error("Rhythm sample dispatch invalid");
                        sc55::MelodicAllocationResult allocation{sc55::MelodicAllocationResult::Status::allocated,
                            native->note,sc55::VoiceAllocator::GroupAllocation{0},*dispatch};
                        const auto inputs = sc55::PrepareRhythmSampleInputs(allocation,map,64,0,scale,{0,0},{128,128});
                        if (!inputs) throw std::runtime_error("Rhythm sample inputs invalid");
                        MCU_Write16(cpu,0xa1c0,0x8048); MCU_Write(cpu,0x804e,64); MCU_Write(cpu,0xab46,0);
                        MCU_Write(cpu,base+0x180+key,map.pitches[key]); MCU_Write(cpu,0xa1b2,uint8_t(key));
                        for (unsigned i = 0; i < 12; ++i) MCU_Write(cpu,0x8048+0x1a+i,scale[i]);
                        MCU_Write16(cpu,0xa1cc,0xffff); MCU_Write(cpu,0xa3d6,255); MCU_Write(cpu,0xa3d7,255);
                        cpu.r[3] = 0; cpu.pc = 0x11fe; cpu.r[7] = 0x9900;
                        unsigned partial = 0, steps = 0;
                        while (cpu.pc != 0x1326)
                        {
                            if (++steps > 2000) throw std::runtime_error("Rhythm sample input path escaped");
                            if (cpu.pc == 0x132b)
                            {
                                while (partial < 2 && !(*dispatch)[partial].prepare) ++partial;
                                if (partial >= 2) throw std::runtime_error("Unexpected rhythm partial");
                                const auto& input = (*inputs)[partial++];
                                if (MCU_Read(cpu,0xa1b2) != input.originalNote || MCU_Read(cpu,0xa1b4) != input.sourceKey
                                    || uint8_t(cpu.r[0]) != input.initialKey)
                                    throw std::runtime_error("Ordered rhythm sample input differs");
                                ++sampleInputChecks;
                            }
                            const auto opcode = MCU_ReadCodeAdvance(cpu); MCU_Operand_Table[opcode](cpu,opcode);
                        }
                        while (partial < 2 && !(*dispatch)[partial].prepare) ++partial;
                        if (partial != 2) throw std::runtime_error("Missing rhythm partial preparation");
                    }
                    ++cases;
                }
    std::printf("Mapped rhythm velocity: %u H8 cases / %u accepted partials matched across both banks\n",cases,accepted);
    std::printf("Ordered rhythm sample inputs: %u H8 partial entries matched\n",sampleInputChecks);
}
