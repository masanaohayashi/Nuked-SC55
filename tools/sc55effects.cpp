// sc55effects - エフェクト（ERAM のリバーブ／コーラス）の状態を調べる。
//
// 浮動小数で書き直すときの下調べと、その後の突き合わせに使う。曲ファイルに頼らず、
// 合成した MIDI で送り量を変えながら遅延メモリの様子を見る。
//
//   sc55effects [リバーブ送り 0-127]
//
// 注意: pcm.ram1[28..29] は 1 パス内の作業レジスタで、パスの合間に覗くと常に 0 に
// 見える。持続する状態は eram（16,384 語の循環バッファ）のほうにある。ここで一度
// 引っかかって「エフェクトが動いていない」と誤読しかけた。
#include "NukedSC55Emulator.h"
#include "mcu.h"
#include "pcm.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
extern uint8_t MCU_Read_Impl(mcu_t&,uint32_t);
extern void MCU_Write_Impl(mcu_t&,uint32_t,uint8_t);
extern void MCU_Write16_Impl(mcu_t&,uint32_t,uint16_t);
extern void MCU_Step_Impl(mcu_t&);
uint8_t MCU_Read(mcu_t& m,uint32_t a){ return MCU_Read_Impl(m,a); }
void MCU_Write(mcu_t& m,uint32_t a,uint8_t v){ MCU_Write_Impl(m,a,v); }
void MCU_Write16(mcu_t& m,uint32_t a,uint16_t v){ MCU_Write16_Impl(m,a,v); }
static mcu_t* g = nullptr;
void MCU_Step(mcu_t& m){ g = &m; MCU_Step_Impl(m); }

int main(int argc, char** argv)
{
    const int send = argc > 1 ? std::atoi(argv[1]) : 127;

    NukedSC55Emulator e;
    if (!e.initialise("/Users/ring2/Documents/Roland SC-55 v1.21", 44100.0)) return 1;
    std::vector<float> l(64), r(64);
    for (int i = 0; i < 8000; ++i) e.render(l.data(), r.data(), 64);

    uint8_t cc[3] = { 0xb0, 91, (uint8_t) send };  e.sendMidi(cc, 3);
    uint8_t pc[2] = { 0xc0, 48 };                  e.sendMidi(pc, 2);
    for (int i = 0; i < 200; ++i) e.render(l.data(), r.data(), 64);
    uint8_t on[3] = { 0x90, 60, 100 };             e.sendMidi(on, 3);
    for (int i = 0; i < 200; ++i) e.render(l.data(), r.data(), 64);
    uint8_t off[3] = { 0x80, 60, 0 };              e.sendMidi(off, 3);

    std::printf("リバーブ送り %d\n消音後の RMS:\n", send);
    for (int blk = 0; blk < 60; ++blk)
    {
        double s = 0; int n = 0;
        for (int i = 0; i < 7; ++i) { e.render(l.data(), r.data(), 64);
            for (int k = 0; k < 64; ++k) { s += (double) l[k] * l[k]; ++n; } }
        if (blk % 10 == 0)
        {
            const double rms = std::sqrt(s / n);
            std::printf("  %4d ms : %7.1f dB\n", blk * 10, rms > 0 ? 20 * std::log10(rms) : -999.0);
        }
    }

    if (!g || !g->pcm) return 1;
    pcm_t& p = *g->pcm;

    long used = 0, peak = 0;
    for (int i = 0; i < 0x4000; ++i)
        if (p.eram[i]) { ++used; if (p.eram[i] > peak) peak = p.eram[i]; }
    std::printf("\neram: 16384 語中 %ld 語が非ゼロ、最大 0x%04lx\n", used, peak);

    std::printf("タップ位置（32 kHz、全長 512 ms の循環バッファへの読み書き位置）:\n");
    for (int bank = 28; bank <= 29; ++bank)
        for (int i = 0; i < 12; ++i)
        {
            const uint16_t v = p.ram2[bank][i];
            if (v) std::printf("  ram2[%d][%2d] = %5u  (%6.2f ms 位置)\n", bank, i, v, v / 32000.0 * 1000.0);
        }
    return 0;
}
