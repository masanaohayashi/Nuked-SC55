#pragma once
#include "mcu.h"
#include "mcu_interrupt.h"
#include <cstdio>
#include <memory>
#include <initializer_list>

void MCU_UpdateUART_RX(mcu_t&);
void MCU_UpdateUART_TX(mcu_t&);

// Real peripheral functions, no ROM or firmware observation required.
inline bool verifyUartStatusRace()
{
    for (const bool receive : {true, false})
    {
        auto cpu = std::make_unique<mcu_t>();
        cpu->dev_register[DEV_SCR] = 0xf0;
        const auto source = receive ? INTERRUPT_SOURCE_UART_RX : INTERRUPT_SOURCE_UART_TX;
        const uint8_t flag = receive ? 0x40 : 0x80;
        const uint8_t stale = MCU_Read(*cpu, 0xffdc);
        if (receive) { MCU_PostUART(*cpu, 0x40); MCU_UpdateUART_RX(*cpu); }
        else MCU_UpdateUART_TX(*cpu);
        // Firmware read SSR before the peripheral event, then writes that
        // stale status. This must not acknowledge an event it did not read.
        MCU_Write(*cpu, 0xffdc, stale);
        if (!(cpu->dev_register[DEV_SSR] & flag) || !cpu->interrupt_pending.Contains(source))
        {
            std::fprintf(stderr, "UART %s: stale SSR write lost an unacknowledged event\n", receive ? "RX" : "TX");
            return false;
        }
        const uint8_t current = MCU_Read(*cpu, 0xffdc);
        MCU_Write(*cpu, 0xffdc, uint8_t(current & ~flag));
        if ((cpu->dev_register[DEV_SSR] & flag) || cpu->interrupt_pending.Contains(source))
        {
            std::fprintf(stderr, "UART acknowledgement did not clear flag and request together\n");
            return false;
        }
        MCU_Write(*cpu, 0xffdc, 0xc0);
        if (cpu->dev_register[DEV_SSR] & 0xc0)
        {
            std::fprintf(stderr, "Writing ones fabricated UART completion flags\n");
            return false;
        }
    }
    std::puts("UART stale-status races: RX and TX passed");
    return true;
}
