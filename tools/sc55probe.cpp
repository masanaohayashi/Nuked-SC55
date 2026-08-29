// probe: SC-55 ファームウェアの「制御レート」ループを特定する。
//  - どの割り込みベクタが周期的に何 Hz で入るか
//  - PCM チップへのレジスタ書き込みを、書いた命令の PC 別に集計
//  - 書き込みバーストの間隔ヒストグラム（= 制御周期）
#include "NukedSC55Emulator.h"
#include "mcu.h"
#include "pcm.h"
#include "emu.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

extern void PCM_Write_Impl(pcm_t& pcm, uint32_t address, uint8_t data);
extern void MCU_Interrupt_Handle_Impl(mcu_t& mcu);

namespace {
bool tracing = false;
uint64_t writes = 0;
std::map<uint32_t, uint64_t> writesByPc;        // (cp<<16)|pc
std::map<uint32_t, std::map<uint32_t, uint64_t>> regsByPc;   // pc -> reg -> n
std::map<uint32_t, std::map<uint32_t, uint64_t>> pcByHandler; // handler -> pc -> n
uint32_t currentHandler = 0;
std::map<uint32_t, uint64_t> vectorCount;       // vector -> count
std::map<uint32_t, uint64_t> vectorLastCycle;
std::map<uint32_t, std::map<uint64_t, uint64_t>> vectorGapHist;  // vector -> gap(us bucket) -> n
uint64_t firstCycle = 0, lastCycle = 0;
uint64_t lastWriteCycle = 0;
std::map<uint64_t, uint64_t> burstGapHist;      // gap between write bursts, in us
// H8 のクロック。mcu.cycles は 1 命令 12 サイクル固定で進む前提の値。
// 実測の cycles/秒 から後で換算する。
}

void PCM_Write(pcm_t& pcm, uint32_t address, uint8_t data)
{
    if (tracing && pcm.mcu != nullptr)
    {
        const mcu_t& mcu = *pcm.mcu;
        ++writes;
        const uint32_t wpc = ((uint32_t)mcu.cp << 16) | mcu.pc;
        writesByPc[wpc]++;
        regsByPc[wpc][address & 0x3f]++;
        pcByHandler[currentHandler][wpc]++;
        if (lastWriteCycle != 0 && mcu.cycles > lastWriteCycle)
        {
            const uint64_t gap = mcu.cycles - lastWriteCycle;
            if (gap > 2000)                       // バースト内の連続書き込みは無視
                burstGapHist[gap / 1000 * 1000]++; // 1000 サイクル刻み
        }
        lastWriteCycle = mcu.cycles;
        lastCycle = mcu.cycles;
        if (firstCycle == 0) firstCycle = mcu.cycles;
    }
    PCM_Write_Impl(pcm, address, data);
}

void MCU_Interrupt_Handle(mcu_t& mcu)
{
    const uint16_t pcBefore = mcu.pc;
    const uint8_t  cpBefore = mcu.cp;
    MCU_Interrupt_Handle_Impl(mcu);
    if (tracing && (mcu.pc != pcBefore || mcu.cp != cpBefore))
    {
        // 割り込みが受理されると PC がハンドラ先頭に飛ぶ。飛び先をベクタの識別子として使う。
        const uint32_t handler = ((uint32_t)mcu.cp << 16) | mcu.pc;
        vectorCount[handler]++;
        auto& last = vectorLastCycle[handler];
        if (last != 0 && mcu.cycles > last)
            vectorGapHist[handler][(mcu.cycles - last) / 500 * 500]++;
        last = mcu.cycles;
        currentHandler = handler;
    }
}

int main(int argc, char** argv)
{
    std::string rom = "/Users/ring2/Documents/Roland SC-55 v1.21";
    const int notes = argc > 1 ? std::atoi(argv[1]) : 0;
    double seconds = argc > 2 ? std::atof(argv[2]) : 10.0;

    NukedSC55Emulator emu;
    if (!emu.initialise(rom, 44100.0)) { std::fprintf(stderr, "init failed\n"); return 1; }

    std::vector<float> l(256), r(256);
    for (int i = 0; i < 2000; ++i) emu.render(l.data(), r.data(), 64);   // 起動

    // 指定した数のノートを鳴らして保持する
    for (int i = 0; i < notes; ++i)
    {
        const uint8_t on[3] = { (uint8_t)(0x90 + (i % 8)), (uint8_t)(40 + i * 2), 100 };
        emu.sendMidi(on, 3);
        for (int k = 0; k < 40; ++k) emu.render(l.data(), r.data(), 64);
    }
    for (int k = 0; k < 400; ++k) emu.render(l.data(), r.data(), 64);   // 落ち着かせる

    tracing = true;
    const uint64_t cyc0 = emu.getDebugState().cycles;

    const double dt = 256.0 / 44100.0;
    double t = 0.0;
    while (t < seconds)
    {
        emu.render(l.data(), r.data(), 256);
        t += dt;
    }
    tracing = false;

    const uint64_t cycles = emu.getDebugState().cycles - cyc0;
    const double cyclesPerSecond = cycles / seconds;

    std::printf("ノート %d 音保持, 計測 %.1f 秒, mcu.cycles %llu (= %.2f Mcycle/s)\n\n",
                notes, seconds, (unsigned long long)cycles, cyclesPerSecond / 1e6);

    std::printf("== 割り込みハンドラ (飛び先 PC 別) ==\n");
    for (auto& [vec, n] : vectorCount)
    {
        std::printf("  handler cp=%02x pc=%04x : %8llu 回 = %8.1f Hz", vec >> 16, vec & 0xffff, (unsigned long long)n, n / seconds);
        auto& gaps = vectorGapHist[vec];
        if (!gaps.empty())
        {
            auto best = std::max_element(gaps.begin(), gaps.end(),
                [](auto& a, auto& b){ return a.second < b.second; });
            std::printf("   最頻間隔 %llu cycles = %.3f ms (%llu/%llu 回)",
                        (unsigned long long)best->first,
                        1000.0 * best->first / cyclesPerSecond,
                        (unsigned long long)best->second, (unsigned long long)n);
        }
        std::printf("\n");
    }

    std::printf("\n== PCM 書き込み: 合計 %llu (%.0f/秒) ==\n",
                (unsigned long long)writes, writes / seconds);
    std::vector<std::pair<uint32_t, uint64_t>> v(writesByPc.begin(), writesByPc.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.second > b.second; });
    std::printf("  書き込みを行った命令の PC (上位 25 / 全 %zu 箇所):\n", v.size());
    for (size_t i = 0; i < std::min<size_t>(25, v.size()); ++i)
        std::printf("    cp=%02x pc=%04x : %8llu  (%5.2f%%)\n",
                    v[i].first >> 16, v[i].first & 0xffff,
                    (unsigned long long)v[i].second, 100.0 * v[i].second / writes);

    std::printf("\n== 書き込み PC → PCM レジスタ ==\n");
    for (size_t i = 0; i < std::min<size_t>(20, v.size()); ++i)
    {
        std::printf("    cp=%02x pc=%04x -> ", v[i].first >> 16, v[i].first & 0xffff);
        for (auto& [reg, n] : regsByPc[v[i].first]) std::printf("%02x ", reg);
        std::printf("\n");
    }

    std::printf("\n== どの割り込みハンドラの中で書いているか ==\n");
    for (auto& [h, pcs] : pcByHandler)
    {
        uint64_t total = 0; for (auto& [pc, n] : pcs) total += n;
        std::printf("    handler cp=%02x pc=%04x : %llu 書き込み (%.1f%%)\n",
                    h >> 16, h & 0xffff, (unsigned long long)total, 100.0 * total / writes);
    }

    std::printf("\n== 書き込みバーストの間隔 (2000 cycle 以上空いたところ) ==\n");
    std::vector<std::pair<uint64_t, uint64_t>> g(burstGapHist.begin(), burstGapHist.end());
    std::sort(g.begin(), g.end(), [](auto& a, auto& b){ return a.second > b.second; });
    for (size_t i = 0; i < std::min<size_t>(12, g.size()); ++i)
        std::printf("    %6llu cycles = %6.3f ms : %llu 回\n",
                    (unsigned long long)g[i].first,
                    1000.0 * g[i].first / cyclesPerSecond,
                    (unsigned long long)g[i].second);
    return 0;
}
