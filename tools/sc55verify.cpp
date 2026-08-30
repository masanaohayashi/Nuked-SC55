// 解析結果をコードで検証する。
//
// パンについては経路が端から端まで分かっている:
//   voice+0x36 に入る 0-127 の値を p として
//   voice+0x34 = (PAN_CURVE[128-p] << 8) | PAN_CURVE[p]
// この式が実機の出力と一致するかを、曲を流しながら全ボイス・全フレームで照合する。
#include "NukedSC55Emulator.h"
#include "MidiFilePlayer.h"
#include "mcu.h"
#include "sc55_tables.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern uint8_t MCU_Read_Impl(mcu_t&, uint32_t);
extern void MCU_Write_Impl(mcu_t&, uint32_t, uint8_t);

namespace {
constexpr uint32_t VOICE0 = 0xacde;
constexpr uint32_t STRIDE = 0x12a;
constexpr int      VOICES = 24;
mcu_t* g_mcu = nullptr;
}

uint8_t MCU_Read(mcu_t& m, uint32_t a) { g_mcu = &m; return MCU_Read_Impl(m, a); }

extern void MCU_Step_Impl(mcu_t&);
extern void MCU_Write16_Impl(mcu_t&, uint32_t, uint16_t);
void MCU_Write16(mcu_t& m, uint32_t a, uint16_t v) { MCU_Write16_Impl(m, a, v); }

// ピッチは voice+0x48 に書かれるが、その材料の [0xc8b0] は呼び出し側がボイスごとに
// 直前に置く一時変数で、レンダリングの合間に外から読むと最後のボイスの値しか見えない。
// 書き込み命令 00:5364 の瞬間に照合する。
static bool g_tracing = false;
static uint64_t g_pitch_checked = 0, g_pitch_matched = 0;

void MCU_Step(mcu_t& m)
{
    if (g_tracing && m.cp == 0 && m.pc == 0x5364)
    {
        const uint32_t base = m.r[0];
        const auto rw = [&] (uint32_t at) {
            return (uint16_t) ((MCU_Read_Impl(m, at) << 8) | MCU_Read_Impl(m, at + 1));
        };
        const uint16_t source = rw(base + 0xa6), global = rw(0xc8b0);
        const uint32_t sum = (uint32_t) source + global;
        const uint16_t want = (source & 0x8000) ? ((sum & 0x10000) ? (uint16_t) sum : 0)
                                                : ((sum & 0x10000) ? 0xffff : (uint16_t) sum);
        ++g_pitch_checked;
        if ((uint16_t) m.r[4] == want) ++g_pitch_matched;
    }
    MCU_Step_Impl(m);
}
void MCU_Write(mcu_t& m, uint32_t a, uint8_t v) { MCU_Write_Impl(m, a, v); }

int main(int argc, char** argv)
{
    NukedSC55Emulator emu;
    if (! emu.initialise("/Users/ring2/Documents/Roland SC-55 v1.21", 44100.0)) return 1;

    MidiFileData midi; std::string error; std::vector<MidiFileEvent> events;
    if (argc > 1 && midi.load(argv[1], error)) events = midi.events;

    std::vector<float> l(256), r(256);
    for (int i = 0; i < 8000; ++i) emu.render(l.data(), r.data(), 64);
    if (g_mcu == nullptr) return 1;

    uint64_t checked = 0, matched = 0;
    int shown = 0;
    g_tracing = true;

    const auto word = [] (uint32_t at) {
        return (uint16_t) ((MCU_Read_Impl(*g_mcu, at) << 8) | MCU_Read_Impl(*g_mcu, at + 1));
    };

    double t = 0; size_t n = 0; const double dt = 256.0 / 44100.0;
    const double duration = argc > 2 ? std::atof(argv[2]) : 20.0;
    while (t < duration)
    {
        while (n < events.size() && events[n].seconds <= t)
        {
            if (! events[n].bytes.empty()) emu.sendMidi(events[n].bytes.data(), (int) events[n].bytes.size());
            ++n;
        }
        emu.render(l.data(), r.data(), 256);

        for (int v = 0; v < VOICES; ++v)
        {
            const uint32_t base = VOICE0 + STRIDE * v;
            const uint8_t index = MCU_Read_Impl(*g_mcu, base + 0x37);   // +0x36 のワードの下位
            const uint8_t left = MCU_Read_Impl(*g_mcu, base + 0x34);
            const uint8_t right = MCU_Read_Impl(*g_mcu, base + 0x35);
            // 一度も使われていないボイスは全部 0 のまま。照合の対象外。
            if (index > 128 || (left == 0 && right == 0 && index == 0)) continue;

            const uint8_t want_left = sc55::PAN_CURVE[128 - index];
            const uint8_t want_right = sc55::PAN_CURVE[index];

            ++checked;
            if (left == want_left && right == want_right) ++matched;
            else if (shown < 5)
            {
                std::printf("  不一致 voice %2d: index=%3d  実機 L=%3d R=%3d  計算 L=%3d R=%3d\n",
                            v, index, left, right, want_left, want_right);
                ++shown;
            }
        }
        t += dt;
    }

    g_tracing = false;
    std::printf("ピッチの照合: %llu 件中 %llu 件一致 (%.2f%%)\n",
                (unsigned long long) g_pitch_checked, (unsigned long long) g_pitch_matched,
                g_pitch_checked ? 100.0 * (double) g_pitch_matched / (double) g_pitch_checked : 0.0);
    std::printf("パンの照合: %llu 件中 %llu 件一致 (%.2f%%)\n",
                (unsigned long long) checked, (unsigned long long) matched,
                checked ? 100.0 * (double) matched / (double) checked : 0.0);
    return matched == checked ? 0 : 1;
}
