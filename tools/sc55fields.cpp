// ボイス構造体の各オフセットを誰が書いているかを記録する。
// 298 バイトのどのフィールドがどのコードで計算されるかを一発で割り出す。
#include "NukedSC55Emulator.h"
#include "MidiFilePlayer.h"
#include "mcu.h"
#include "pcm.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
extern uint8_t MCU_Read_Impl(mcu_t&, uint32_t);
extern void MCU_Write_Impl(mcu_t&, uint32_t, uint8_t);
extern void MCU_Step_Impl(mcu_t&);
// ワードアクセスは mcu.cpp 内の別関数を通るので、これも包まないと取りこぼす。
extern uint16_t MCU_Read16_Impl(mcu_t&, uint32_t);
extern void MCU_Write16_Impl(mcu_t&, uint32_t, uint16_t);

static constexpr uint32_t VOICE0 = 0xacde;
static constexpr uint32_t STRIDE = 0x12a;
static constexpr int      VOICES = 24;

static bool tracing = false;
static uint32_t pc_now = 0;
// offset -> writer pc -> count
static std::map<uint32_t, std::map<uint32_t, uint64_t>> writes;
static std::map<uint32_t, uint64_t> reads_by_offset;

uint8_t MCU_Read(mcu_t& m, uint32_t a)
{
    if (tracing) { const uint32_t x = a & 0xffff;
        if (x >= VOICE0 && x < VOICE0 + STRIDE * VOICES) reads_by_offset[(x - VOICE0) % STRIDE]++; }
    return MCU_Read_Impl(m, a);
}
void MCU_Write(mcu_t& m, uint32_t a, uint8_t v)
{
    if (tracing) { const uint32_t x = a & 0xffff;
        if (x >= VOICE0 && x < VOICE0 + STRIDE * VOICES) writes[(x - VOICE0) % STRIDE][pc_now]++; }
    MCU_Write_Impl(m, a, v);
}
uint16_t MCU_Read16(mcu_t& m, uint32_t a)
{
    if (tracing) { const uint32_t x = a & 0xffff;
        if (x >= VOICE0 && x < VOICE0 + STRIDE * VOICES) { reads_by_offset[(x - VOICE0) % STRIDE]++; reads_by_offset[(x + 1 - VOICE0) % STRIDE]++; } }
    return MCU_Read16_Impl(m, a);
}
void MCU_Write16(mcu_t& m, uint32_t a, uint16_t v)
{
    if (tracing) { const uint32_t x = a & 0xffff;
        if (x >= VOICE0 && x < VOICE0 + STRIDE * VOICES) { writes[(x - VOICE0) % STRIDE][pc_now]++; writes[(x + 1 - VOICE0) % STRIDE][pc_now]++; } }
    MCU_Write16_Impl(m, a, v);
}
void MCU_Step(mcu_t& m) { pc_now = ((uint32_t) m.cp << 16) | m.pc; MCU_Step_Impl(m); }

int main(int argc, char** argv)
{
    NukedSC55Emulator e;
    if (!e.initialise("/Users/ring2/Documents/Roland SC-55 v1.21", 44100.0)) return 1;
    MidiFileData m; std::string err; std::vector<MidiFileEvent> ev;
    if (argc > 1 && m.load(argv[1], err)) ev = m.events;
    std::vector<float> l(256), r(256);
    for (int i = 0; i < 8000; ++i) e.render(l.data(), r.data(), 64);
    tracing = true;
    double t = 0; size_t n = 0; const double dt = 256.0 / 44100.0;
    while (t < 20.0) {
        while (n < ev.size() && ev[n].seconds <= t) {
            if (!ev[n].bytes.empty()) e.sendMidi(ev[n].bytes.data(), (int) ev[n].bytes.size());
            ++n; }
        e.render(l.data(), r.data(), 256); t += dt; }
    tracing = false;

    std::printf("ボイス構造体 (%d 本 × %u バイト) の書き込み元\n", VOICES, STRIDE);
    std::printf("既知: +1a=TVA +1e=TVF +26=カットオフ +34=パン +3a=送り +48=ピッチ +66/+68=フラグ\n\n");
    std::printf("  ofs | 書込回数 | 読出回数 | 主な書き込み元\n");
    for (uint32_t ofs = 0; ofs < STRIDE; ++ofs) {
        auto it = writes.find(ofs);
        if (it == writes.end()) continue;
        uint64_t tot = 0; for (auto& [pc, c] : it->second) tot += c;
        if (tot < 20) continue;
        std::vector<std::pair<uint32_t,uint64_t>> v(it->second.begin(), it->second.end());
        std::sort(v.begin(), v.end(), [](auto&a,auto&b){ return a.second > b.second; });
        std::printf("  %3x | %8llu | %8llu |", ofs, (unsigned long long) tot,
                    (unsigned long long) reads_by_offset[ofs]);
        for (size_t i = 0; i < std::min<size_t>(3, v.size()); ++i)
            std::printf(" %02x:%04x(%llu)", v[i].first >> 16, v[i].first & 0xffff,
                        (unsigned long long) v[i].second);
        std::printf("\n");
    }
    return 0;
}
