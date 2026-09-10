#pragma once
#include "mcu.h"
#include <array>
#include <map>
#include <tuple>
#include <cstdio>
#include <stdexcept>
#include <span>

// Raw-byte inventory, not a control-flow reachability proof. A branch could
// enter after an argument setup; direct event writes are a separate audit.
inline void ReportKernelNotificationSites(std::span<const uint8_t> rom1,std::span<const uint8_t> rom2)
{
    std::array<unsigned,9> destinations{};
    unsigned unknown=0,dataCandidates=0;
    std::array<unsigned,9> interruptDestinations{};
    unsigned unknownInterrupts=0;
    const auto scan=[&](std::span<const uint8_t> bytes,unsigned base,unsigned codeSize) {
        for(unsigned i=0;i+1<bytes.size();++i) {
            if(bytes[i]!=8 || bytes[i+1]!=0x12) continue;
            if(i>=codeSize) {++dataCandidates;continue;}
            if(i>=4 && bytes[i-4]==0x50 && bytes[i-2]==0x51 && bytes[i-3]<destinations.size()) {
                ++destinations[bytes[i-3]];
                std::printf("NOTIFY_SITE address=%06x destination=%u event=%u immediate-arguments\n",
                    base+i,bytes[i-3],bytes[i-1]);
            } else {
                ++unknown;
                std::printf("NOTIFY_SITE address=%06x unresolved-arguments\n",base+i);
            }
        }
    };
    scan(rom1,0,unsigned(rom1.size()));
    // Audit the known code bank at CP4 separately. Other-bank byte matches
    // are counted, not silently treated as proven unreachable instructions.
    scan(rom2,0x40000,0x10000);
    // Interrupt handlers already saved the context and jump into TRAPA2's
    // body. Counting only the trap opcode omits MIDI/PCM/LCD notifications.
    // This remains a byte inventory, not proof against indirect branches.
    for(unsigned i=0;i+2<rom1.size();++i) {
        if(rom1[i]!=0x10 || rom1[i+1]!=4 || rom1[i+2]!=0x19) continue;
        if(i>=7 && rom1[i-7]==0x50 && rom1[i-5]==0x51
            && rom1[i-3]==4 && rom1[i-2]==0 && rom1[i-1]==0x8d
            && rom1[i-6]<interruptDestinations.size()) {
            ++interruptDestinations[rom1[i-6]];
            std::printf("INTERRUPT_NOTIFY_SITE address=%06x destination=%u event=%u immediate-arguments\n",
                i,rom1[i-6],rom1[i-4]);
        } else {
            ++unknownInterrupts;
            std::printf("INTERRUPT_NOTIFY_SITE address=%06x unresolved-arguments\n",i);
        }
    }
    for(unsigned target=0;target<destinations.size();++target)
        std::printf("NOTIFY_SITES destination=%u count=%u\n",target,destinations[target]);
    std::printf("NOTIFY_SITES unresolved=%u other-bank-byte-matches=%u\n",unknown,dataCandidates);
    for(unsigned target=0;target<interruptDestinations.size();++target)
        std::printf("INTERRUPT_NOTIFY_SITES destination=%u count=%u\n",target,interruptDestinations[target]);
    std::printf("INTERRUPT_NOTIFY_SITES unresolved=%u\n",unknownInterrupts);
}

// Observe actual instruction entry, after interrupt dispatch. No RAM writes,
// synthetic wakeups, or recorded times are supplied to the native engine.
struct KernelEventProbe
{
    // Current task is context, not proof of the sender: an ISR may notify
    // while retaining the interrupted task's fdca value.
    using Notification=std::tuple<unsigned,unsigned,unsigned>; // context task, destination, event bit
    std::map<Notification,uint64_t> notifications;
    std::map<std::pair<unsigned,unsigned>,uint64_t> timers,waits,consumed;
    std::array<uint64_t,9> timerExpirations{},dormantEntries{},dormantReturns{};
    uint64_t pcmAcknowledgements=0,pcmDuringReuseWait=0,pcmCoalesced=0;

    void instruction(mcu_t& cpu)
    {
        if(cpu.cp!=0) return;
        const auto task=unsigned(MCU_Read16(cpu,0xfdca));
        if(cpu.pc==0x760) {
            ++pcmAcknowledgements;
            if(MCU_Read(cpu,0xfdeb+2)==0x80) ++pcmDuringReuseWait;
        }
        if(cpu.pc==0x768 && cpu.r[0]<24)
            pcmCoalesced+=MCU_Read(cpu,0xcb30+cpu.r[0])!=0;
        if(cpu.pc==0x422) {
            const auto destination=unsigned(cpu.r[2])-0xfde2u;
            const auto bit=unsigned(cpu.r[1]&7);
            if(task>9 || destination>=9) throw std::runtime_error("Invalid kernel notification owner");
            ++notifications[{task,destination,bit}];
        }
        if(cpu.pc==0x32d) {
            const auto offset=unsigned(cpu.r[2])-0xfe12u;
            if(offset>=18 || (offset&1) || offset/2!=task)
                throw std::runtime_error("Timer registration owner differs from current task");
            ++timers[{task,cpu.r[0]}];
        }
        if(cpu.pc==0x3d0) {
            const auto destination=unsigned(cpu.r[2])-0xfe12u;
            if(destination>=9) throw std::runtime_error("Invalid timer expiration owner");
            ++timerExpirations[destination];
        }
        if(cpu.pc==0x466) {
            if(task>=9 || cpu.r[2]!=0xfde2u+task)
                throw std::runtime_error("Invalid kernel wait owner");
            ++waits[{task,cpu.r[0]&255}];
        }
        // Immediate wait satisfaction and restoration of a sleeping waiter.
        if(cpu.pc==0x473 || cpu.pc==0x518) {
            const unsigned owner=cpu.pc==0x473 ? task : cpu.r[6];
            const unsigned bit=cpu.pc==0x473 ? cpu.r[0] : cpu.r[2];
            if(owner>=9 || bit>=8) throw std::runtime_error("Invalid consumed kernel event");
            ++consumed[{owner,bit}];
        }
        if(task==5 || task==6) {
            if(cpu.pc==0x542) ++dormantEntries[task];
            if(cpu.pc==0x546) ++dormantReturns[task];
        }
    }

    void report(mcu_t& cpu) const
    {
        std::printf("PCM_RECEPTION acknowledgements=%llu duringReuseWait=%llu coalesced=%llu\n",
            (unsigned long long)pcmAcknowledgements,(unsigned long long)pcmDuringReuseWait,
            (unsigned long long)pcmCoalesced);
        for(const auto& [key,count]:notifications) {
            const auto [source,destination,bit]=key;
            std::printf("NOTIFY contextTask=%u destination=%u bit=%u count=%llu\n",
                source,destination,bit,(unsigned long long)count);
        }
        for(const auto& [key,count]:timers)
            std::printf("TIMER task=%u period=%u registrations=%llu expirations=%llu\n",
                key.first,key.second,(unsigned long long)count,
                (unsigned long long)timerExpirations[key.first]);
        for(const auto& [key,count]:waits)
            std::printf("WAIT task=%u mask=%02x count=%llu\n",
                key.first,key.second,(unsigned long long)count);
        for(const auto& [key,count]:consumed)
            std::printf("CONSUME task=%u bit=%u count=%llu\n",
                key.first,key.second,(unsigned long long)count);
        for(unsigned task:{5u,6u})
            std::printf("DORMANT task=%u entries=%llu returned=%llu pending=%02x mask=%02x deadline=%04x period=%u\n",
                task,(unsigned long long)dormantEntries[task],(unsigned long long)dormantReturns[task],
                MCU_Read(cpu,0xfde2+task),MCU_Read(cpu,0xfdeb+task),
                MCU_Read16(cpu,0xfe12+task*2),MCU_Read16(cpu,0xfe24+task*2));
    }
};
