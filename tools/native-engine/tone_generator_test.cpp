#include "sc55_tone_generator.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

// The four-voice (NEON) path must produce exactly what the scalar voice
// produces: same frames, envelope readbacks and loop events. Random waveforms,
// loops, pitches, envelopes and keys; no ROM needed.
namespace
{
uint64_t state = 0x2545f4914f6cdd1dull;
uint32_t next()
{
    state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
    return uint32_t((state * 0x2545f4914f6cdd1dull) >> 32);
}

sc55::VoiceRenderUpdate randomUpdate()
{
    sc55::VoiceRenderUpdate u;
    u.phaseIncrement = uint16_t(next());
    for (auto& command : u.rampCommands) command = uint16_t(next());
    u.panLeft = int8_t(next()); u.panRight = int8_t(next());
    u.reverbSend = int8_t(next()); u.chorusSend = int8_t(next());
    u.resonance = uint8_t(next() & 127);
    u.filterFlags = uint8_t((next() & 2) | ((next() & 7) == 0)); // loop events now and then
    return u;
}

void run(unsigned voices, unsigned blocks)
{
    std::vector<uint8_t> roms[3];
    for (auto& rom : roms) { rom.resize(0x100000); for (auto& b : rom) b = uint8_t(next()); }
    auto scalar = std::make_unique<sc55::ToneGenerator>(roms[0], roms[1], roms[2], voices);
    auto vector = std::make_unique<sc55::ToneGenerator>(roms[0], roms[1], roms[2], voices);
    scalar->setVectorized(false);
    // Effects on, so the reverb and chorus sends reach the output.
    sc55::ChorusSetup chorus;
    chorus.begin = 0x100; chorus.end = 0x900; chorus.position = 0x400; chorus.increment = 0x80;
    chorus.returns = {0x4040, 0x2020, 0x3030, 0x4000};
    sc55::ReverbSetup reverb;
    for (unsigned i = 0; i < reverb.diffusionTaps.size(); ++i) reverb.diffusionTaps[i] = uint16_t(0x100 + 0x177 * i);
    for (unsigned i = 0; i < reverb.tailTaps.size(); ++i) reverb.tailTaps[i] = uint16_t(0x2000 + 0x233 * i);
    reverb.diffusion = {0x3020, 0x3020}; reverb.damping = {0x2020, 0x2020};
    reverb.comb = 0x3030; reverb.output = 0x4040; reverb.spreadCommand = 0x40ff;
    for (auto* tg : {scalar.get(), vector.get()}) {
        tg->effects().configureReverb(reverb);
        tg->effects().configureChorus(chorus);
        tg->enableEffects();
        using P = sc55::EffectParameter;
        tg->effects().update(P::reverbInput, 0x3040);
        tg->effects().update(P::reverbOutput, 0x4040);
        tg->effects().update(P::chorusInput, 0x3040);
    }
    std::vector<AudioFrame<int32_t>> a(300), b(300);
    unsigned frames = 0, events = 0;
    int64_t peak = 0;
    for (unsigned block = 0; block < blocks; ++block) {
        // Control between spans, applied identically to both.
        const unsigned changes = next() % 8;
        for (unsigned c = 0; c < changes; ++c) {
            const unsigned voice = next() % voices;
            switch (next() % 5) {
            case 0: {
                sc55::VoiceRenderStart start;
                start.end = next() & 0xfffff;
                start.loop = (start.end - 1 - (next() & 0x7ff)) & 0xfffff;
                start.start = (start.loop - (next() & 0xff)) & 0xfffff;
                start.mode = uint16_t(next() & 0x0ee0);
                start.pitchSource = uint8_t(next() % voices);
                start.controls = randomUpdate();
                for (auto* tg : {scalar.get(), vector.get()}) tg->install(voice, start);
                break;
            }
            case 1: { const auto u = randomUpdate(); for (auto* tg : {scalar.get(), vector.get()}) tg->update(voice, u); break; }
            case 2: {
                sc55::VoiceSet keys;
                for (unsigned v = 0; v < voices; ++v) keys.set(v, (next() & 3) != 0);
                for (auto* tg : {scalar.get(), vector.get()}) tg->setKeys(keys);
                break;
            }
            case 3: {
                const auto stage = sc55::EnvelopeRamp::Stage(next() % 3);
                const auto command = uint16_t(next());
                for (auto* tg : {scalar.get(), vector.get()}) tg->setRamp(voice, stage, command);
                break;
            }
            case 4: {
                const auto level = uint16_t(next()), command = uint16_t(next());
                if (scalar->completeEnable(voice, level, command) != vector->completeEnable(voice, level, command))
                    throw std::runtime_error("Readiness differs");
                break;
            }
            }
        }
        const std::size_t span = 1 + next() % 300;
        const auto ra = scalar->render(std::span(a.data(), span));
        const auto rb = vector->render(std::span(b.data(), span));
        if (ra != rb || std::memcmp(a.data(), b.data(), ra * sizeof(a[0])))
            throw std::runtime_error("Vectorized frames differ from the scalar voice");
        for (unsigned v = 0; v < voices; ++v)
            for (unsigned s = 0; s < 3; ++s)
                if (scalar->rampLevel(v, sc55::EnvelopeRamp::Stage(s)) != vector->rampLevel(v, sc55::EnvelopeRamp::Stage(s)))
                    throw std::runtime_error("Vectorized envelope levels differ");
        if (scalar->hasLoopEvent() != vector->hasLoopEvent()) throw std::runtime_error("Loop events differ");
        if (scalar->hasLoopEvent() && (next() & 3)) {
            ++events;
            if (scalar->takeLoopEvent() != vector->takeLoopEvent()) throw std::runtime_error("Loop event voices differ");
        }
        frames += unsigned(ra);
        for (std::size_t i = 0; i < ra; ++i) peak = std::max(peak, std::abs(int64_t(a[i].left)));
    }
    std::printf("%u voices: %u frames, %u loop events identical (peak %.1f dBFS)\n", voices, frames, events,
        peak ? 20 * std::log10(double(peak) / 2147483648.0) : -999.0);
}
}

int main()
{
    try {
        run(24, 4000);
        run(128, 2000);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
