#pragma once
#include "sc55_system_defaults.h"
#include <map>
#include <cstdio>

// Diagnostic only. Observe reads of the settings not yet interpreted by the
// native controller, without mutating the H8 or interpreting absence as proof.
struct ConfigurationReaders
{
    std::array<bool,0x748> watched{};
    std::map<std::pair<unsigned,unsigned>,uint64_t> reads; // offset, instruction
    const mcu_t* executing=nullptr;
    unsigned instructionAddress=0;
    ConfigurationReaders()
    {
        for(auto offset:sc55::UninterpretedSystemSettings::globalOffsets) watched[offset]=true;
        for(unsigned part=0;part<16;++part)
            for(auto offset:sc55::UninterpretedSystemSettings::partOffsets)
                watched[0x48+part*0x70+offset]=true;
    }
    void instruction(const mcu_t& cpu) noexcept
    { executing=&cpu; instructionAddress=(unsigned(cpu.cp)<<16)|cpu.pc; }
    void read(const mcu_t& cpu,unsigned address)
    {
        if(&cpu!=executing || address<0x8000 || address>=0x8748) return;
        const auto offset=address-0x8000;
        if(watched[offset]) ++reads[{offset,instructionAddress}];
    }
    void report() const
    {
        std::array<bool,0x748> observed{};
        for(const auto& [key,count]:reads) {
            observed[key.first]=true;
            std::printf("CONFIG_READER offset=%03x address=%04x instruction=%06x reads=%llu\n",
                key.first,0x8000+key.first,key.second,(unsigned long long)count);
        }
        unsigned seen=0,total=0;
        for(unsigned i=0;i<watched.size();++i) if(watched[i]) {++total;seen+=observed[i];}
        std::printf("CONFIG_READERS observed=%u watched=%u (unobserved is not unreachable)\n",seen,total);
    }
};
