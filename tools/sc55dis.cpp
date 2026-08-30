// sc55dis - SC-55 のファームウェアを逆アセンブルする。
//
//   sc55dis 00:0379 200            そのアドレスから 200 バイト
//   sc55dis 00:0379 200 song.mid   実行回数を注釈する
//
// 命令デコードは binutils 2.16.1 の opcodes/h8500-dis.c をそのまま使う。H8/500
// を持つ最後のリリースで、以降の binutils からは削除されている。ここが書くのは
// メモリの読み口と、実行回数の注釈だけ。
//
// バイトはエミュレータの MCU_Read から取る。バンク切り替えの規則を書き写すより、
// CPU が実際に読むものをそのまま読むほうが間違えようがない。
#include "NukedSC55Emulator.h"
#include "MidiFilePlayer.h"
#include "mcu.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "h8500/binutils-shim.h"
}

extern uint8_t MCU_Read_Impl(mcu_t&, uint32_t);
extern void MCU_Write_Impl(mcu_t&, uint32_t, uint8_t);
extern void MCU_Interrupt_Handle_Impl(mcu_t&);

namespace {
mcu_t* g_mcu = nullptr;
uint32_t g_page = 0;

bool counting = false;
std::map<uint32_t, uint64_t> g_hits;      // (cp<<16)|pc -> 実行回数

int ReadMemory(bfd_vma address, bfd_byte* buffer, unsigned int length, disassemble_info*)
{
    for (unsigned int i = 0; i < length; ++i)
        buffer[i] = MCU_Read_Impl(*g_mcu, g_page | ((address + i) & 0xffff));
    return 0;
}

void MemoryError(int, bfd_vma, disassemble_info*) {}

int Emit(void* stream, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = std::vfprintf((FILE*) stream, format, args);
    va_end(args);
    return n;
}
}

uint8_t MCU_Read(mcu_t& mcu, uint32_t address)
{
    // The emulator owns its mcu_t; borrow the pointer the first time it reads.
    g_mcu = &mcu;
    return MCU_Read_Impl(mcu, address);
}

void MCU_Write(mcu_t& mcu, uint32_t address, uint8_t value)
{
    MCU_Write_Impl(mcu, address, value);
}

void MCU_Interrupt_Handle(mcu_t& mcu)
{
    if (counting)
        g_hits[((uint32_t) mcu.cp << 16) | mcu.pc]++;
    MCU_Interrupt_Handle_Impl(mcu);
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("使い方: sc55dis CP:PC 長さ [song.mid]\n"
                    "  例:   sc55dis 00:0379 256 /path/to/song.mid\n");
        return 1;
    }

    unsigned cp = 0, pc = 0;
    if (std::sscanf(argv[1], "%x:%x", &cp, &pc) != 2)
    {
        std::fprintf(stderr, "アドレスは CP:PC の形で（例 00:0379）\n");
        return 1;
    }
    const unsigned length = (unsigned) std::strtoul(argv[2], nullptr, 0);

    NukedSC55Emulator emu;
    if (! emu.initialise("/Users/ring2/Documents/Roland SC-55 v1.21", 44100.0))
    {
        std::fprintf(stderr, "エミュレータの初期化に失敗\n");
        return 1;
    }

    std::vector<float> l(256), r(256);
    for (int i = 0; i < 2000; ++i) emu.render(l.data(), r.data(), 64);

    // 実行回数の注釈がほしければ、指定の曲を流して数える。
    if (argc > 3)
    {
        MidiFileData midi;
        std::string error;
        std::vector<MidiFileEvent> events;
        if (midi.load(argv[3], error)) events = midi.events;
        else std::fprintf(stderr, "%s\n", error.c_str());

        counting = true;
        double t = 0.0; size_t next = 0; const double dt = 256.0 / 44100.0;
        while (t < 5.0)
        {
            while (next < events.size() && events[next].seconds <= t)
            {
                if (! events[next].bytes.empty())
                    emu.sendMidi(events[next].bytes.data(), (int) events[next].bytes.size());
                ++next;
            }
            emu.render(l.data(), r.data(), 256);
            t += dt;
        }
        counting = false;
    }

    if (g_mcu == nullptr)
    {
        std::fprintf(stderr, "MCU をつかめなかった\n");
        return 1;
    }
    g_page = cp << 16;

    disassemble_info info {};
    info.stream = stdout;
    info.fprintf_func = Emit;
    info.read_memory_func = ReadMemory;
    info.memory_error_func = MemoryError;

    uint64_t total = 0;
    for (auto& [where, count] : g_hits) total += count;

    unsigned address = pc;
    while (address < pc + length)
    {
        const uint32_t here = (cp << 16) | (address & 0xffff);
        const auto hit = g_hits.find(here);

        if (total != 0)
        {
            if (hit != g_hits.end())
                std::printf("%10llu %5.2f%%  ", (unsigned long long) hit->second,
                            100.0 * (double) hit->second / (double) total);
            else
                std::printf("%10s %5s   ", "", "");
        }

        std::printf("%02x:%04x  ", cp, address & 0xffff);

        // 生バイトも出す。テーブルの誤読に気づけるように。
        const int size = print_insn_h8500(address, &info);
        std::printf("\n");
        address += size > 0 ? size : 1;
    }
    return 0;
}
