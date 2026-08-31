// sc55revtest - 試験用 .mid（tools/sc55mkmidi.py で作る）を鳴らして、
// エフェクトのタップ位置・係数・残響の尾を並べる。
//
// GS の SysEx で設定した値が、チップ側の何を動かすのかを直接見るための道具。
//
//   sc55revtest <test.mid>
#include "NukedSC55Emulator.h"
#include "MidiFilePlayer.h"
#include "mcu.h"
#include "pcm.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
extern uint8_t MCU_Read_Impl(mcu_t&,uint32_t);
extern void MCU_Write_Impl(mcu_t&,uint32_t,uint8_t);
extern void MCU_Write16_Impl(mcu_t&,uint32_t,uint16_t);
extern void MCU_Step_Impl(mcu_t&);
uint8_t MCU_Read(mcu_t& m,uint32_t a){ return MCU_Read_Impl(m,a); }
void MCU_Write(mcu_t& m,uint32_t a,uint8_t v){ MCU_Write_Impl(m,a,v); }
void MCU_Write16(mcu_t& m,uint32_t a,uint16_t v){ MCU_Write16_Impl(m,a,v); }
static mcu_t* g=nullptr;
void MCU_Step(mcu_t& m){ g=&m; MCU_Step_Impl(m); }

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("使い方: sc55revtest <test.mid>\n"); return 1; }

    NukedSC55Emulator e;
    if (!e.initialise("/Users/ring2/Documents/Roland SC-55 v1.21", 44100.0)) return 1;
    MidiFileData m; std::string err;
    if (!m.load(argv[1], err)) { std::printf("読めない: %s\n", err.c_str()); return 1; }

    std::vector<float> l(64), r(64);
    for (int i = 0; i < 8000; ++i) e.render(l.data(), r.data(), 64);

    // 全イベントを時刻どおりに送りつつ、消音後の尾を測る
    double t = 0; size_t n = 0; const double dt = 64.0 / 44100.0;
    double last_note_off = 0;
    for (const auto& ev : m.events)
        if (!ev.bytes.empty() && ((ev.bytes[0] & 0xf0) == 0x80 ||
            ((ev.bytes[0] & 0xf0) == 0x90 && ev.bytes.size() > 2 && ev.bytes[2] == 0)))
            last_note_off = ev.seconds;

    std::vector<double> rms;
    while (t < last_note_off + 3.0)
    {
        while (n < m.events.size() && m.events[n].seconds <= t)
        {
            if (!m.events[n].bytes.empty())
                e.sendMidi(m.events[n].bytes.data(), (int) m.events[n].bytes.size());
            ++n;
        }
        e.render(l.data(), r.data(), 64);
        if (t >= last_note_off)
        {
            double s = 0;
            for (int k = 0; k < 64; ++k) s += (double) l[k] * l[k] + (double) r[k] * r[k];
            rms.push_back(std::sqrt(s / 128));
        }
        t += dt;
    }

    if (!g || !g->pcm) return 1;
    pcm_t& p = *g->pcm;

    std::printf("%s\n", argv[1]);

    // 尾: 消音直後を基準に、何 ms で -20 / -40 dB まで落ちるか
    double peak = 0;
    for (size_t i = 0; i < rms.size() && i < 40; ++i) peak = std::max(peak, rms[i]);
    auto reach = [&] (double db) {
        const double target = peak * std::pow(10.0, db / 20.0);
        for (size_t i = 0; i < rms.size(); ++i)
            if (rms[i] < target) return (int) (i * 64 * 1000 / 44100);
        return -1;
    };
    std::printf("  尾: -20 dB まで %d ms、-40 dB まで %d ms\n", reach(-20), reach(-40));

    long used = 0;
    for (int i = 0; i < 0x4000; ++i) if (p.eram[i]) ++used;
    std::printf("  eram 非ゼロ %ld / 16384 語\n", used);

    std::printf("  タップ ram2[28]:");
    for (int i = 0; i < 12; ++i) std::printf(" %5u", p.ram2[28][i]);
    std::printf("\n  タップ ram2[29]:");
    for (int i = 0; i < 12; ++i) std::printf(" %5u", p.ram2[29][i]);
    std::printf("\n  係数 ram2[30]:");
    for (int i = 0; i < 12; ++i) std::printf(" %04x", p.ram2[30][i]);
    std::printf("\n  係数 ram2[31]:");
    for (int i = 0; i < 12; ++i) std::printf(" %04x", p.ram2[31][i]);
    std::printf("\n");
    return 0;
}
