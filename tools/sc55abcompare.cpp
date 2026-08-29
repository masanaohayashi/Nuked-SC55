// sc55abcompare - runs pcm.cpp (the emulation) and pcm_sim.cpp (the simulation)
// side by side on one identical register stream and reports how far apart they
// are. The firmware, the H8 and the register writes are shared; only the thing
// that turns registers into audio differs.
//
// Build: see FIRMWARE_CONTROL_LOOP.md. pcm.cpp is compiled with
//   -DPCM_Write=PCM_Write_Impl -DMCU_PostSample=MCU_PostSample_Impl
// so this file can wrap both.
#include "MidiFilePlayer.h"
#include "NukedSC55Emulator.h"
#include "mcu.h"
#include "pcm.h"
#include "pcm_sim.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

uint8_t PCM_ReadROM(pcm_t& pcm, uint32_t address);
extern void PCM_Write_Impl(pcm_t& pcm, uint32_t address, uint8_t data);
extern void MCU_PostSample_Impl(mcu_t& mcu, const AudioFrame<int32_t>& frame);

namespace
{
pcm_t* g_pcm = nullptr;
PCMSimVoices g_sim;
bool g_comparing = false;

// Key-on edges: when the firmware starts a voice the simulation has to pick up
// the new sample's addresses and clear its own running state.
uint32_t g_previous_keys = 0;

// Per-voice state the chip keeps in its own registers, so the simulation can be
// checked field by field instead of only at the mix bus.
struct FieldError { double sum_abs = 0.0; double sum_ref = 0.0; uint64_t n = 0; };
FieldError g_err_address, g_err_reference, g_err_env0, g_err_env1, g_err_env2;
FieldError g_err_svf_low, g_err_svf_band;
uint64_t g_nibble_total = 0, g_nibble_match = 0;
uint64_t g_addr_total = 0, g_addr_exact = 0;
uint64_t g_phase_exact = 0;
// How many frames after a key-on the read position first parts company.
std::map<int, uint64_t> g_first_divergence;
int g_age[32] = {};
bool g_diverged[32] = {};
bool g_trace = false;
// Which kind of sample the voices that lose sync are playing.
uint64_t g_div_bidi = 0, g_div_oneshot = 0, g_live_bidi = 0, g_live_oneshot = 0;
uint64_t g_div_flip = 0, g_live_flip = 0;
std::map<int, uint64_t> g_addr_offset;   // signed distance, clamped
std::map<int, uint64_t> g_nibble_chip, g_nibble_mine;

void Track(FieldError& field, double simulated, double reference)
{
    field.sum_abs += std::fabs(simulated - reference);
    field.sum_ref += std::fabs(reference);
    ++field.n;
}

inline double sx20(uint32_t v)
{
    return (double)(int32_t)((v & 0xfffff) ^ 0x80000) - 524288.0;
}

uint64_t g_frames = 0;
double g_sum_reference = 0.0, g_sum_simulated = 0.0;
double g_sum_error = 0.0, g_sum_product = 0.0;
double g_peak_reference = 0.0, g_peak_simulated = 0.0, g_peak_error = 0.0;

// The output stage is a noise shaper: it emits only the high bits and feeds
// the low ones back, twice per pass over the voices. A single emitted sample
// is the modulator's output, not the signal, so both sides are averaged over
// the pass and then smoothed before they are compared.
double g_smooth_reference = 0.0, g_smooth_simulated = 0.0;
double SMOOTHING = 0.15;

void CompareFrame(double reference_l_raw)
{
    if (g_pcm == nullptr)
        return;

    const pcm_t& pcm = *g_pcm;
    const uint32_t keys = pcm.voice_mask & pcm.voice_mask_pending;

    for (int slot = 0; slot < g_sim.voice_count; ++slot)
    {
        const bool keyed_now = ((keys >> slot) & 1) != 0;
        const bool keyed_before = ((g_previous_keys >> slot) & 1) != 0;

        PCMSim_SyncVoice(g_sim, pcm, slot);

        if (keyed_now && !keyed_before)
        {
            PCMSim_KeyOn(g_sim, pcm, slot);
            g_age[slot] = 0;
            g_diverged[slot] = false;
        }
    }
    g_previous_keys = keys;

    for (int slot = 0; slot < g_sim.voice_count; ++slot)
    {
        if (!((keys >> slot) & 1)) continue;
        g_phase_exact += (g_sim.sub_phase[slot] == (uint32_t)(pcm.ram2[slot][8] & 0x3fff));
        {
            const int32_t mine_addr = g_sim.address[slot];
            const int32_t chip_addr = (int32_t)(pcm.ram1[slot][4] & 0xfffff);
            ++g_addr_total;
            g_addr_exact += (mine_addr == chip_addr);
            int32_t d = mine_addr - chip_addr;
            if (d < -4) d = -5; if (d > 4) d = 5;
            g_addr_offset[d]++;
            const bool bidi = (pcm.ram2[slot][7] & 0x40) != 0;
            const bool flip = (pcm.ram2[slot][7] & 0x80) != 0;
            if (bidi) ++g_live_bidi; else ++g_live_oneshot;
            if (flip) ++g_live_flip;
            if (!g_diverged[slot] && mine_addr != chip_addr)
            {
                g_diverged[slot] = true;
                g_first_divergence[g_age[slot] < 20 ? g_age[slot] : 20]++;
                if (bidi) ++g_div_bidi; else ++g_div_oneshot;
                if (flip) ++g_div_flip;
            }
            if (g_age[slot] < 12 && slot == 0 && g_trace)
                std::printf("age %2d  phase chip=%5u mine=%5u step=%5u | addr chip=%06x mine=%06x | ram2[7]=%04x\n",
                            g_age[slot], (unsigned)(pcm.ram2[slot][8] & 0x3fff),
                            (unsigned)g_sim.sub_phase[slot], (unsigned)g_sim.phase_step[slot],
                            (unsigned)chip_addr, (unsigned)mine_addr, (unsigned)pcm.ram2[slot][7]);
            ++g_age[slot];
        }
        Track(g_err_address,   (double)g_sim.address[slot],   (double)(pcm.ram1[slot][4] & 0xfffff));
        Track(g_err_reference, (double)g_sim.reference[slot], sx20(pcm.ram1[slot][5]));
        Track(g_err_svf_low,  (double)g_sim.svf_low[slot],  sx20(pcm.ram1[slot][3]));
        Track(g_err_svf_band, (double)g_sim.svf_band[slot], sx20(pcm.ram1[slot][1]));
        Track(g_err_env0, (double)g_sim.env_level[0][slot], (double)(pcm.ram2[slot][9]  & 0x7fff));
        Track(g_err_env1, (double)g_sim.env_level[1][slot], (double)(pcm.ram2[slot][10] & 0x7fff));
        Track(g_err_env2, (double)g_sim.env_level[2][slot], (double)(pcm.ram2[slot][11] & 0x7fff));

        // The chip caches the current block's exponent in ram2[7] bits 12..15.
        const uint32_t here = pcm.ram1[slot][4] & 0xfffff;
        const uint32_t bank = (uint32_t)(((pcm.ram2[slot][7] >> 8) & 15) << 20);
        const uint8_t packed = PCM_ReadROM(*g_pcm, bank | (here >> 5));
        const int mine = (here & 0x10) ? ((packed >> 4) & 15) : (packed & 15);
        const int chip = (pcm.ram2[slot][7] >> 12) & 15;
        ++g_nibble_total;
        g_nibble_match += (mine == chip);
        g_nibble_chip[chip]++;
        g_nibble_mine[mine]++;
    }


    float simulated[4] = {};
    PCMSim_RenderFrame(g_sim, pcm, simulated);

    g_smooth_reference += (reference_l_raw - g_smooth_reference) * SMOOTHING;
    g_smooth_simulated += (simulated[0] - g_smooth_simulated) * SMOOTHING;

    const double reference_l = g_smooth_reference;
    const double simulated_l = g_smooth_simulated;

    ++g_frames;
    g_sum_reference += reference_l * reference_l;
    g_sum_simulated += simulated_l * simulated_l;
    g_sum_error += (reference_l - simulated_l) * (reference_l - simulated_l);
    g_sum_product += reference_l * simulated_l;
    g_peak_reference = std::max(g_peak_reference, std::fabs(reference_l));
    g_peak_simulated = std::max(g_peak_simulated, std::fabs(simulated_l));
    g_peak_error = std::max(g_peak_error, std::fabs(reference_l - simulated_l));
}
} // namespace

void PCM_Write(pcm_t& pcm, uint32_t address, uint8_t data)
{
    g_pcm = &pcm;
    PCM_Write_Impl(pcm, address, data);
}

void MCU_PostSample(mcu_t& mcu, const AudioFrame<int32_t>& frame)
{
    // Two posts per pass over the voice slots: the mixer runs at twice the
    // rate the voices are updated at. Sum both, then step the simulation once.
    static double pending = 0.0;
    static int posts = 0;

    if (g_comparing && g_pcm != nullptr)
    {
        const double bias = ((g_pcm->config_reg_3c & 0x30) == 0x30) ? 4096.0 : 0.0;
        pending += (double)(frame.left >> 12) - bias;

        if (++posts == 2)
        {
            CompareFrame(pending * 0.5);
            pending = 0.0;
            posts = 0;
        }
    }

    MCU_PostSample_Impl(mcu, frame);
}

int main(int argc, char** argv)
{
    std::string rom = "/Users/ring2/Documents/Roland SC-55 v1.21";
    const char* input = nullptr;
    double seconds = 10.0;
    int notes = 0;   // instead of a file: hold this many notes

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--rom" && i + 1 < argc) rom = argv[++i];
        else if (arg == "--notes" && i + 1 < argc) notes = std::atoi(argv[++i]);
        else if (input == nullptr)          input = argv[i];
        else                                seconds = std::atof(argv[i]);
    }

    PCMSim_Init();

    NukedSC55Emulator emu;
    if (!emu.initialise(rom, 44100.0)) { std::fprintf(stderr, "init failed\n"); return 1; }

    std::vector<MidiFileEvent> events;
    if (input != nullptr && *input != '\0')
    {
        MidiFileData midi;
        std::string error;
        if (midi.load(input, error)) events = midi.events;
        else { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    }

    std::vector<float> l(256), r(256);
    for (int i = 0; i < 2000; ++i) emu.render(l.data(), r.data(), 64);

    // Effects are not simulated yet, so take them out of the reference: their
    // return would otherwise land in the same mix bus we are comparing.
    const uint8_t reverb_off[] = {0xf0,0x41,0x10,0x42,0x12,0x40,0x01,0x33,0x00,0x0c,0xf7};
    const uint8_t chorus_off[] = {0xf0,0x41,0x10,0x42,0x12,0x40,0x01,0x3a,0x00,0x05,0xf7};
    emu.sendMidi(reverb_off, sizeof reverb_off);
    emu.sendMidi(chorus_off, sizeof chorus_off);
    for (int i = 0; i < 400; ++i) emu.render(l.data(), r.data(), 64);

    for (int i = 0; i < notes; ++i)
    {
        const uint8_t on[3] = { (uint8_t)(0x90 + (i % 8)), (uint8_t)(48 + i * 3), 100 };
        emu.sendMidi(on, 3);
        for (int k = 0; k < 40; ++k) emu.render(l.data(), r.data(), 64);
    }
    for (int k = 0; k < 200; ++k) emu.render(l.data(), r.data(), 64);

    if (const char* sm = getenv("SC55_SMOOTH")) SMOOTHING = atof(sm);
    g_trace = (getenv("SC55_TRACE") != nullptr);
    g_comparing = true;
    double t = 0.0;
    size_t next = 0;
    const double dt = 256.0 / 44100.0;
    while (t < seconds)
    {
        while (next < events.size() && events[next].seconds <= t)
        {
            if (!events[next].bytes.empty())
                emu.sendMidi(events[next].bytes.data(), (int)events[next].bytes.size());
            ++next;
        }
        emu.render(l.data(), r.data(), 256);
        t += dt;
    }
    g_comparing = false;

    if (g_frames == 0) { std::fprintf(stderr, "no frames compared\n"); return 1; }

    const double rms_reference = std::sqrt(g_sum_reference / (double)g_frames);
    const double rms_simulated = std::sqrt(g_sum_simulated / (double)g_frames);
    const double rms_error = std::sqrt(g_sum_error / (double)g_frames);
    const double correlation = g_sum_product
        / (std::sqrt(g_sum_reference) * std::sqrt(g_sum_simulated) + 1e-12);

    std::printf("frames                 %llu\n", (unsigned long long)g_frames);
    std::printf("RMS  emulation         %.1f\n", rms_reference);
    std::printf("RMS  simulation        %.1f\n", rms_simulated);
    std::printf("RMS  error             %.1f  (%.1f%% of emulation)\n",
                rms_error, 100.0 * rms_error / (rms_reference + 1e-12));
    std::printf("peak emulation         %.1f\n", g_peak_reference);
    std::printf("peak simulation        %.1f\n", g_peak_simulated);
    std::printf("peak error             %.1f\n", g_peak_error);
    std::printf("correlation            %.4f\n", correlation);

    std::printf("\nper-voice state vs the chip's own registers (mean |error| / mean |value|):\n");
    auto report = [](const char* name, const FieldError& f)
    {
        if (f.n == 0) { std::printf("  %-22s no samples\n", name); return; }
        std::printf("  %-22s %10.1f / %10.1f = %6.1f%%\n", name,
                    f.sum_abs / (double)f.n, f.sum_ref / (double)f.n,
                    100.0 * f.sum_abs / (f.sum_ref + 1e-12));
    };
    report("address  ram1[4]", g_err_address);
    report("dpcm sum ram1[5]", g_err_reference);
    report("svf low  ram1[3]", g_err_svf_low);
    report("svf band ram1[1]", g_err_svf_band);
    report("env0     ram2[9]", g_err_env0);
    report("env1     ram2[10]", g_err_env1);
    report("cutoff   ram2[11]", g_err_env2);

    std::printf("\naddress exact match: %llu / %llu (%.2f%%)\n",
                (unsigned long long)g_addr_exact, (unsigned long long)g_addr_total,
                100.0 * (double)g_addr_exact / (double)(g_addr_total + 1));
    std::printf("  offset (mine - chip):");
    for (auto& [d, n] : g_addr_offset)
        std::printf(" %s%d:%.1f%%", d <= -5 ? "<=" : (d >= 5 ? ">=" : ""), d, 100.0*n/g_addr_total);
    std::printf("\n");

    std::printf("voices that lost sync, by sample type:\n");
    std::printf("  ping-pong (b6): %llu lost, %.1f%% of active-voice frames are ping-pong\n",
                (unsigned long long)g_div_bidi, 100.0*g_live_bidi/(g_addr_total+1));
    std::printf("  one-shot      : %llu lost, %.1f%% of frames\n",
                (unsigned long long)g_div_oneshot, 100.0*g_live_oneshot/(g_addr_total+1));
    std::printf("  reversed (b7) : %llu lost, %.1f%% of frames\n",
                (unsigned long long)g_div_flip, 100.0*g_live_flip/(g_addr_total+1));
    std::printf("sub-phase exact match: %.2f%%\n",
                100.0 * (double)g_phase_exact / (double)(g_addr_total + 1));
    std::printf("frames after key-on before the address first differs:\n  ");
    for (auto& [age, n] : g_first_divergence) std::printf("%s%d:%llu ", age >= 20 ? ">=" : "", age, (unsigned long long)n);
    std::printf("\n");

    std::printf("\nblock exponent: %llu / %llu agree (%.1f%%)\n",
                (unsigned long long)g_nibble_match, (unsigned long long)g_nibble_total,
                100.0 * (double)g_nibble_match / (double)(g_nibble_total + 1));
    std::printf("  chip:"); for (auto& [v, n] : g_nibble_chip) std::printf(" %d:%.0f%%", v, 100.0*n/g_nibble_total);
    std::printf("\n  mine:"); for (auto& [v, n] : g_nibble_mine) std::printf(" %d:%.0f%%", v, 100.0*n/g_nibble_total);
    std::printf("\n");
    return 0;
}
