#include "pcm.h"
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

// The branch-free keyed-voice renderer must match the slot-by-slot transcription
// bit for bit: every voice/effect RAM word, buses, IRQ, clock and posted frame.
// Randomized registers reach loop/ping-pong/reverse/key-on combinations that no
// song plays. No ROM needed: the wave banks are filled with noise.
namespace
{
// xorshift64*: every bit is usable (an LCG's low bits cycle with period 2).
uint64_t random_state = 0x9e3779b97f4a7c15ull;
uint32_t next()
{
    random_state ^= random_state >> 12; random_state ^= random_state << 25; random_state ^= random_state >> 27;
    return uint32_t((random_state * 0x2545f4914f6cdd1dull) >> 32);
}

struct Captured { std::vector<AudioFrame<int32_t>> frames; };

void fill(pcm_t& pcm)
{
    for (auto* rom : {pcm.waverom1, pcm.waverom2, pcm.waverom_card})
        for (size_t i = 0; i < 0x200000; ++i) rom[i] = uint8_t(next() >> 13);
    for (size_t i = 0; i < 0x100000; ++i) pcm.waverom3[i] = uint8_t(next() >> 13);
}

bool same(const pcm_t& a, const pcm_t& b)
{
    return !std::memcmp(a.ram1, b.ram1, sizeof(a.ram1)) && !std::memcmp(a.ram2, b.ram2, sizeof(a.ram2))
        && !std::memcmp(a.native_ram1, b.native_ram1, sizeof(a.native_ram1))
        && !std::memcmp(a.native_ram2, b.native_ram2, sizeof(a.native_ram2))
        && !std::memcmp(a.eram, b.eram, sizeof(a.eram))
        && a.accum_l == b.accum_l && a.accum_r == b.accum_r
        && a.rcsum[0] == b.rcsum[0] && a.rcsum[1] == b.rcsum[1]
        && a.cycles == b.cycles && a.tv_counter == b.tv_counter && a.nfs == b.nfs
        && a.irq_assert == b.irq_assert && a.irq_channel == b.irq_channel;
}

void run(const char* name, unsigned nativeVoices, uint8_t reg_slots, uint8_t config3d, unsigned frames)
{
    auto fast = std::make_unique<pcm_t>();
    fast->is_mk1 = true;
    fill(*fast);
    auto reference = std::make_unique<pcm_t>(*fast);
    fast->reference_voice_renderer = false;
    reference->reference_voice_renderer = true;
    Captured outFast, outReference;
    for (auto [pcm, out] : {std::pair{fast.get(), &outFast}, std::pair{reference.get(), &outReference}}) {
        pcm->output_context = out;
        pcm->output_sample = [](void* context, const AudioFrame<int32_t>& frame) {
            static_cast<Captured*>(context)->frames.push_back(frame);
        };
        pcm->native_voice_count = nativeVoices;
        pcm->config.reg_slots = reg_slots;
        pcm->config_reg_3d = config3d;
        pcm->enable_oversampling = false;
    }
    unsigned keyed = 0, stops = 0, raised = 0, frame = 0;
    for (unsigned call = 0; frame < frames; ++call) {
        // Re-randomize all registers now and then; in between, voices evolve
        // through key-on, loops and releases on their own.
        if (call % 16 == 0 || (next() & 7) == 0) {
            const bool full = call % 16 == 0;
            const uint32_t mask = (next() & 7) == 0 ? 0 : next();
            sc55::VoiceSet keys;
            for (unsigned slot = 0; slot < 128; ++slot) keys.set(slot, (next() & 3) != 0);
            for (auto* pcm : {fast.get(), reference.get()}) {
                pcm->voice_mask = pcm->voice_mask_pending = mask;
                pcm->native_keys = keys;
            }
            const bool nfs = (next() & 15) != 0, irq = (next() & 3) == 0;
            fast->nfs = reference->nfs = nfs;
            fast->irq_assert = reference->irq_assert = irq;
            for (unsigned slot = 0; slot < 128; ++slot) {
                const uint8_t source = uint8_t(next() % (nativeVoices ? nativeVoices : 128));
                fast->native_pitch_source[slot] = reference->native_pitch_source[slot] = source;
                if (slot >= 32 && !nativeVoices) continue;
                uint32_t* ram1[2] = {fast->voiceRam1(slot), reference->voiceRam1(slot)};
                uint16_t* ram2[2] = {fast->voiceRam2(slot), reference->voiceRam2(slot)};
                for (unsigned i = 0; i < 8; ++i) {
                    const uint32_t value = next() & 0xfffff;
                    if (full || (next() & 1)) ram1[0][i] = ram1[1][i] = value;
                }
                for (unsigned i = 0; i < 16; ++i) {
                    // Full-range increments: up to four source samples per pass.
                    const uint16_t value = uint16_t(next() >> 8);
                    if (full || (next() & 1)) ram2[0][i] = ram2[1][i] = value;
                }
                // Most voices start a few samples from a loop end or start, so
                // ends, loops and ping-pong turns happen at every walk step.
                if (next() & 3) {
                    const int span = int(next() & 0xfff) + 1, offset = int(next() & 15) - 8;
                    const uint32_t end = ram1[0][0], loop = (end - uint32_t(span)) & 0xfffff;
                    const uint32_t near = (next() & 1) ? end : loop;
                    ram1[0][2] = ram1[1][2] = loop;
                    ram1[0][4] = ram1[1][4] = (near + uint32_t(offset)) & 0xfffff;
                }
            }
        }
        // Spans of any length, as the scheduler hands them over, with and
        // without stopping at a voice IRQ; the IRQ is taken now and then.
        const uint64_t span = 1 + next() % 300;
        const bool stop = (next() & 1) != 0;
        const uint64_t before = fast->cycles;
        const bool irqBefore = fast->irq_assert;
        PCM_Update(*fast, fast->cycles + span * 625, stop);
        PCM_Update(*reference, reference->cycles + span * 625, stop);
        stops += stop && fast->cycles < before + span * 625;
        raised += !irqBefore && fast->irq_assert;
        frame += unsigned((fast->cycles - before) / 625);
        if (next() & 1) fast->irq_assert = reference->irq_assert = false;
        if (!same(*fast, *reference) || outFast.frames.size() != outReference.frames.size()
            || std::memcmp(outFast.frames.data(), outReference.frames.data(),
                   outFast.frames.size() * sizeof(AudioFrame<int32_t>))) {
            std::fprintf(stderr, "%s: first difference in call %u (frame %u)\n", name, call, frame);
            for (unsigned slot = 0; slot < (nativeVoices ? nativeVoices : 32u); ++slot) {
                for (unsigned i = 0; i < 8; ++i)
                    if (fast->voiceRam1(slot)[i] != reference->voiceRam1(slot)[i])
                        std::fprintf(stderr, "  slot %u ram1[%u] %05x != %05x\n", slot, i,
                            fast->voiceRam1(slot)[i], reference->voiceRam1(slot)[i]);
                for (unsigned i = 0; i < 16; ++i)
                    if (fast->voiceRam2(slot)[i] != reference->voiceRam2(slot)[i])
                        std::fprintf(stderr, "  slot %u ram2[%u] %04x != %04x\n", slot, i,
                            fast->voiceRam2(slot)[i], reference->voiceRam2(slot)[i]);
            }
            throw std::runtime_error("Branch-free voice renderer differs from the transcription");
        }
        keyed += unsigned((fast->cycles - before) / 625) * (nativeVoices ? std::min(fast->native_keys.count(), nativeVoices)
            : unsigned(std::popcount(fast->voice_mask & fast->voice_mask_pending & ((1ull << reg_slots) - 1))));
    }
    std::printf("%s: %u frames, %u keyed voice-frames, %u IRQs (%u stopping a span) match\n",
        name, frame, keyed, raised, stops);
}
}

int main()
{
    try {
        run("hardware 24 slots, 21-bit banks", 0, 24, 0xb7, 20000);
        run("hardware 24 slots, 19-bit banks", 0, 24, 0x97, 20000);
        run("hardware 28 slots", 0, 28, 0xb7, 10000);
        run("hardware 32 slots", 0, 32, 0xb7, 10000);
        run("native 24 voices", 24, 24, 0xb7, 20000);
        run("native 64 voices", 64, 24, 0xb7, 10000);
        run("native 128 voices", 128, 24, 0xb7, 10000);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
