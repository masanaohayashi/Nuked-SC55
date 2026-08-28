// sc55render - MIDI ファイルを SC-55 エミュレータでレンダリングして WAV に書き出す。
//
//   sc55render input.mid [output.wav] [--rom DIR] [--start SEC] [--len SEC]
//
// 既定では曲の最後 + 1小節ぶんまでレンダリングする。
// SMF の解析はプラグインと同じ MidiFilePlayer を使う（同一 tick の順序を保つ）。
#include "MidiFilePlayer.h"
#include "NukedSC55Emulator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

int main (int argc, char** argv)
{
    const char* input = nullptr;
    const char* output = nullptr;
    std::string romDirectory = "/Users/ring2/Documents/Roland SC-55 v1.21";
    double startSeconds = 0.0, lengthSeconds = -1.0;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if      (arg == "--rom"   && i + 1 < argc) romDirectory = argv[++i];
        else if (arg == "--start" && i + 1 < argc) startSeconds = std::atof (argv[++i]);
        else if (arg == "--len"   && i + 1 < argc) lengthSeconds = std::atof (argv[++i]);
        else if (input == nullptr)  input = argv[i];
        else if (output == nullptr) output = argv[i];
    }

    if (input == nullptr)
    {
        std::printf ("使い方: sc55render input.mid [output.wav] [--rom DIR] [--start SEC] [--len SEC]\n");
        return 1;
    }

    std::string outPath = output != nullptr ? output : std::string (input);
    if (output == nullptr)
    {
        const size_t dot = outPath.find_last_of ('.');
        if (dot != std::string::npos) outPath.erase (dot);
        outPath += ".wav";
    }

    MidiFileData midi;
    std::string parseError;
    if (! midi.load (input, parseError)) { std::fprintf (stderr, "%s\n", parseError.c_str()); return 1; }
    const auto& events = midi.events;

    const double total = lengthSeconds > 0 ? lengthSeconds : (midi.totalSeconds() - startSeconds);
    std::printf ("%s: %zu events, 曲の長さ %.1f 秒 + 1小節 %.2f 秒\n",
                 input, events.size(), midi.songEndSeconds, midi.lastBarSeconds);
    std::printf ("レンダリング %.1f 秒 -> %s\n", total, outPath.c_str());

    NukedSC55Emulator emu;
    if (! emu.initialise (romDirectory, 44100.0))
    {
        std::fprintf (stderr, "エミュレータの初期化に失敗: %s\n", emu.getError().c_str());
        return 1;
    }

    std::vector<float> left (64), right (64);
    std::vector<int16_t> pcm;
    pcm.reserve (static_cast<size_t> (total * 44100.0 * 2.0) + 65536);

    auto renderBlock = [&] (bool record)
    {
        // FIFO が満ちるまで待つ。待たないとアンダーランで直前フレームが繰り返される。
        while (emu.availableFrames() < 96)
            std::this_thread::sleep_for (std::chrono::microseconds (200));

        emu.render (left.data(), right.data(), 64);

        if (record)
            for (int i = 0; i < 64; ++i)
            {
                pcm.push_back (static_cast<int16_t> (std::lround (std::clamp (left[i],  -1.0f, 1.0f) * 32767.0f)));
                pcm.push_back (static_cast<int16_t> (std::lround (std::clamp (right[i], -1.0f, 1.0f) * 32767.0f)));
            }
    };

    for (int i = 0; i < 3000; ++i) renderBlock (false);          // 起動シーケンス

    const double dt = 64.0 / 44100.0;
    double t = 0.0;
    size_t next = 0;
    int lastPercent = -1;

    while (t < startSeconds + total)
    {
        while (next < events.size() && events[next].seconds <= t)
        {
            if (! events[next].bytes.empty())
                emu.sendMidi (events[next].bytes.data(), static_cast<int> (events[next].bytes.size()));
            ++next;
        }

        renderBlock (t >= startSeconds);
        t += dt;

        const int percent = static_cast<int> (100.0 * t / (startSeconds + total));
        if (percent != lastPercent) { std::printf ("\r  %d%%", percent); std::fflush (stdout); lastPercent = percent; }
    }

    FILE* wav = std::fopen (outPath.c_str(), "wb");
    if (wav == nullptr) { std::fprintf (stderr, "\n書き込めません: %s\n", outPath.c_str()); return 1; }

    const uint32_t dataBytes = static_cast<uint32_t> (pcm.size() * 2), rate = 44100;
    const uint32_t header[] = { 0x46464952, dataBytes + 36, 0x45564157, 0x20746d66, 16, 0x00020001,
                                rate, rate * 4, 0x00100004, 0x61746164, dataBytes };
    std::fwrite (header, 4, 11, wav);
    std::fwrite (pcm.data(), 2, pcm.size(), wav);
    std::fclose (wav);

    std::printf ("\r  完了: %s (%.1f 秒)\n", outPath.c_str(), pcm.size() / 2.0 / 44100.0);
    return 0;
}
