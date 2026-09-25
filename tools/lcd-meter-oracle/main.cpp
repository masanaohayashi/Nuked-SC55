#include "emu.h"
#include "rom_loader.h"
#include "MidiFilePlayer.h"
#include "sc55_sysex.h"
#include "sc55_lcd_meter_render.h"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
struct CaptureBackend final : LCD_Backend
{
    bool Start(const lcd_t&) override { return true; }
    void Stop() override {}
    void Render() override {}
};

void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
}

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        std::fprintf(stderr, "Usage: sc55-lcd-meter-oracle ROM_DIRECTORY [SONG.rcp]\n");
        return 2;
    }
    try
    {
        common::LoadRomsetResult roms;
        const auto error = common::LoadRomset(argv[1], "mk1-v1.21",
            common::RomLoader::Hashing, {}, roms);
        if (error != common::LoadRomsetError{})
            throw std::runtime_error(common::ToCString(error));
        require(roms.picked_name == "mk1-v1.21", "This oracle requires v1.21 ROMs");

        CaptureBackend capture;
        Emulator emu;
        require(emu.Init({.lcd_backend = &capture})
                && emu.LoadRoms(roms.romset, roms.romset_info), "Emulator init failed");
        emu.Reset();
        require(emu.StartLCD(), "LCD init failed");
        auto& cpu = emu.GetMCU();
        cpu.native_v121_enabled = false; // Actual H8 instructions, no native shortcuts.
        const auto run = [&](uint64_t cycles)
        {
            const auto end = cpu.cycles + cycles;
            while (cpu.cycles < end) emu.Step();
        };
        run(120000000);
        auto& lcd = emu.GetLCD();
        require(lcd.width == 741 && lcd.height == 268 && lcd.color1 != lcd.color2,
                "Reference LCD geometry/palette was not initialized");
        constexpr std::array<uint8_t, 8> romGlyphs {0,2,4,6,1,3,5,7};
        for (unsigned i = 0; i < romGlyphs.size(); ++i)
            require(lcd.LCD_Data[20 + (i / 4) * 40 + i % 4] == romGlyphs[i],
                    "Unexpected ROM meter DDRAM layout");
        std::puts("ROM DDRAM: upper=0,2,4,6 lower=1,3,5,7");

        unsigned frames = 0;
        std::vector<uint8_t> pixels(741 * 268);
        const auto check = [&](const std::array<uint8_t, 64>& bytes)
        {
            // Canonical envelope isolates the LCD layout from file-specific
            // manufacturer-ID encodings. Every payload still passes real H8 RX.
            std::vector<uint8_t> packet {0xf0,0x41,0x10,0x45,0x12,0x10,1,0};
            unsigned sum = 0x11;
            for (auto b : bytes) { packet.push_back(b); sum += b; }
            packet.push_back(uint8_t(-sum) & 127);
            packet.push_back(0xf7);
            emu.PostMIDI(packet);
            run(4000000); // Allow UART reception and complete LCD transfer.
            require(MCU_Read16(cpu, 0xcf34) != 0 && lcd.enable.load(),
                    "ROM did not activate the meter bitmap");
            for (unsigned i = 0; i < bytes.size(); ++i)
                require(MCU_Read(cpu, 0xff00 + i) == bytes[i]
                        && lcd.LCD_CG[i] == (bytes[i] & 31),
                        "ROM RX or LCD transfer did not finish");
            LCD_Render(lcd); // Existing firmware-backed renderer is the reference.
            std::fill(pixels.begin(), pixels.end(), 0);
            require(sc55::renderSysExLevelMeterPixels(pixels, 741, bytes),
                    "Native renderer rejected the frame");
            unsigned mismatches = 0;
            for (unsigned row = 0; row < 16; ++row)
                for (unsigned part = 0; part < 16; ++part)
                    for (unsigned dy = 0; dy < 9; ++dy)
                        for (unsigned dx = 0; dx < 24; ++dx)
                        {
                            const auto x = 293 + part * 26 + dx;
                            const auto y = 71 + row * 11 + dy;
                            const auto reference = lcd.buffer[y][x];
                            require(reference == lcd.color1 || reference == lcd.color2,
                                    "Reference cell was not rendered");
                            const auto expected = reference == lcd.color1 ? 1 : 2;
                            mismatches += pixels[y * 741 + x] != expected;
                        }
            if (mismatches != 0)
            {
                std::fprintf(stderr, "Frame %u: %u/55296 cell pixels differ from ROM\n",
                             frames, mismatches);
                throw std::runtime_error("SysEx meter layout differs from original ROM");
            }
            ++frames;
        };

        std::array<uint8_t, 64> bytes {};
        // This nonuniform fixture is also frozen in the ROM-free regression test.
        for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = (i * 37 + (i / 8) * 3) & 127;
        check(bytes);
        // Walk every MIDI data bit, including unused glyph columns and bits 5/6.
        for (unsigned byte = 0; byte < 64; ++byte)
            for (unsigned bit = 0; bit < 7; ++bit)
            {
                bytes.fill(0);
                bytes[byte] = uint8_t(1u << bit);
                check(bytes);
            }
        bytes.fill(0); check(bytes);
        bytes.fill(127); check(bytes);
        std::printf("ROM differential: %u synthetic frames match all 55,296 cell pixels\n", frames);

        if (argc == 3)
        {
            MidiFileData song;
            std::string loadError;
            if (!song.load(argv[2], loadError, false)) throw std::runtime_error(loadError);
            sc55::MidiDecoder decoder;
            sc55::SysExReceiver receiver;
            unsigned songFrames = 0;
            for (const auto& event : song.events)
                decoder.push(event.bytes, [&](const sc55::MidiDecoder::Event& midi)
                {
                    const auto result = receiver.receive(midi, 0x10, true, false);
                    if (result.status != sc55::SysExReceiver::Status::roland
                        || result.model != 0x45 || result.command != 0x12
                        || result.payload.size() != 67 || result.payload[0] != 0x10
                        || result.payload[1] != 1 || result.payload[2] != 0)
                        return;
                    std::copy_n(result.payload.begin() + 3, 64, bytes.begin());
                    check(bytes);
                    ++songFrames;
                });
            require(songFrames != 0, "Song contained no meter bitmap messages");
            std::printf("Song: %u Model45 meter frames match ROM pixels (payload replay)\n", songFrames);
        }
        emu.StopLCD();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "LCD meter oracle: %s\n", e.what());
        return 1;
    }
}
