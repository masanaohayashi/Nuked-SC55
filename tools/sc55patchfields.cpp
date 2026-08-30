// 音色ブロック 216 バイトのどのオフセットを、どのコードが読んでいるかを記録する。
// パーシャル・パラメータのフィールドを名前ではなく「使っている側」から特定する。
#include "NukedSC55Emulator.h"
#include "MidiFilePlayer.h"
#include "mcu.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

extern uint8_t MCU_Read_Impl(mcu_t&, uint32_t);
extern uint16_t MCU_Read16_Impl(mcu_t&, uint32_t);
extern void MCU_Write_Impl(mcu_t&, uint32_t, uint8_t);
extern void MCU_Write16_Impl(mcu_t&, uint32_t, uint16_t);
extern void MCU_Step_Impl(mcu_t&);

namespace {
// tools/sc55patches.py が求めた値。ROM のバージョンが変われば変わる。
constexpr uint32_t TABLE = 0x0ff48;
constexpr uint32_t STRIDE = 0xd8;
constexpr int      COUNT = 225;

bool tracing = false;
uint32_t pc_now = 0;
std::map<uint32_t, std::map<uint32_t, uint64_t>> readers;   // offset -> pc -> count

// MCU_Read と同じ規則で ROM2 上のオフセットに直す。
void note(uint32_t address)
{
    // ページ 0 は ROM1 と RAM と IO。音色テーブルはページ 1 以降から読まれるので、
    // ここで弾かないと 0xffa1 のような IO アドレスがテーブル内に化けて紛れ込む。
    if ((address >> 16) == 0) return;

    uint32_t rom = address & 0x3ffff;
    if (address & 0x80000) rom |= 0x40000;
    if (rom < TABLE || rom >= TABLE + STRIDE * COUNT) return;
    readers[(rom - TABLE) % STRIDE][pc_now]++;
}
}

uint8_t MCU_Read(mcu_t& m, uint32_t a) { if (tracing) note(a); return MCU_Read_Impl(m, a); }
uint16_t MCU_Read16(mcu_t& m, uint32_t a) { if (tracing) { note(a); note(a + 1); } return MCU_Read16_Impl(m, a); }
void MCU_Write(mcu_t& m, uint32_t a, uint8_t v) { MCU_Write_Impl(m, a, v); }
void MCU_Write16(mcu_t& m, uint32_t a, uint16_t v) { MCU_Write16_Impl(m, a, v); }
void MCU_Step(mcu_t& m) { pc_now = ((uint32_t) m.cp << 16) | m.pc; MCU_Step_Impl(m); }

int main(int argc, char** argv)
{
    NukedSC55Emulator emu;
    if (! emu.initialise("/Users/ring2/Documents/Roland SC-55 v1.21", 44100.0)) return 1;

    MidiFileData midi; std::string error; std::vector<MidiFileEvent> events;
    if (argc > 1 && midi.load(argv[1], error)) events = midi.events;

    std::vector<float> l(256), r(256);
    for (int i = 0; i < 8000; ++i) emu.render(l.data(), r.data(), 64);

    tracing = true;
    double t = 0; size_t n = 0; const double dt = 256.0 / 44100.0;
    const double duration = argc > 2 ? std::atof(argv[2]) : 60.0;
    while (t < duration) {
        while (n < events.size() && events[n].seconds <= t) {
            if (! events[n].bytes.empty()) emu.sendMidi(events[n].bytes.data(), (int) events[n].bytes.size());
            ++n; }
        emu.render(l.data(), r.data(), 256); t += dt; }
    tracing = false;

    std::printf("音色ブロック (216 バイト) のオフセット別 読み出し元\n");
    std::printf("  +00-5b パーシャル1  +5c-b7 パーシャル2  +b8-c3 名前  +c4-d7 共通\n\n");
    std::printf("  ofs | 読出回数 | 主な読み出し元\n");
    for (uint32_t ofs = 0; ofs < STRIDE; ++ofs) {
        auto it = readers.find(ofs);
        if (it == readers.end()) continue;
        uint64_t total = 0; for (auto& [pc, c] : it->second) total += c;
        std::vector<std::pair<uint32_t,uint64_t>> v(it->second.begin(), it->second.end());
        std::sort(v.begin(), v.end(), [](auto&a, auto&b){ return a.second > b.second; });
        std::printf("  %3x | %8llu |", ofs, (unsigned long long) total);
        for (size_t i = 0; i < std::min<size_t>(3, v.size()); ++i)
            std::printf(" %02x:%04x(%llu)", v[i].first >> 16, v[i].first & 0xffff,
                        (unsigned long long) v[i].second);
        std::printf("\n");
    }
    return 0;
}
