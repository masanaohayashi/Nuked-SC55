#include "NukedSC55Emulator.h"

#include "SC55Lcd.h"
#include "SC55Debug.h"
#include "NativeSoundDataCache.h"
#include "NativeMeterDecay.h"
#include "sc55_synth.h"
#include "sc55_display.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string_view>

#include "audio.h"
#include "diagnostics.h"
#include "emu.h"
#include "lcd.h"
#include "mcu.h"
#include "pcm.h"
#include "rom_loader.h"

// lcd_font.h contains the backend's storage definition rather than a
// declaration.  lcd.cpp owns that storage; the adapter only needs to read it.
extern uint8_t lcd_font[240][10];

namespace
{
constexpr uint64_t gsResetFallbackCycles = 24000000;
constexpr double dcBlockerHz = 5.0;
constexpr double pi = 3.14159265358979323846;

void backendDiagnostic (Diag_Category category, std::string_view message)
{
    // Unknown peripheral accesses are useful when working on the backend, but
    // they are very noisy during an ordinary Debug run. Opt into that stream
    // explicitly while keeping warnings and errors visible.
    if (category == Diag_Category::Debug && std::getenv ("NUKED_SC55_CORE_DEBUG") == nullptr)
        return;

    sc55debug::log ("backend[%s] %.*s", ToCString (category),
                    static_cast<int> (message.size()), message.data());
}

void installBackendDiagnostics()
{
    static std::once_flag flag;
    std::call_once (flag, [] { Diag_SetCallback (&backendDiagnostic); });
}

uint32_t frontPanelButtonMask (NukedSC55Emulator::FrontPanelButton button) noexcept
{
    using Button = NukedSC55Emulator::FrontPanelButton;

    switch (button)
    {
        case Button::partDec:          return 1u << MCU_BUTTON_PART_L;
        case Button::partInc:          return 1u << MCU_BUTTON_PART_R;
        case Button::instrumentDec:    return 1u << MCU_BUTTON_INST_L;
        case Button::instrumentInc:    return 1u << MCU_BUTTON_INST_R;
        case Button::levelDec:         return 1u << MCU_BUTTON_LEVEL_L;
        case Button::levelInc:         return 1u << MCU_BUTTON_LEVEL_R;
        case Button::panDec:           return 1u << MCU_BUTTON_PAN_L;
        case Button::panInc:           return 1u << MCU_BUTTON_PAN_R;
        case Button::reverbDec:        return 1u << MCU_BUTTON_REVERB_L;
        case Button::reverbInc:        return 1u << MCU_BUTTON_REVERB_R;
        case Button::chorusDec:        return 1u << MCU_BUTTON_CHORUS_L;
        case Button::chorusInc:        return 1u << MCU_BUTTON_CHORUS_R;
        case Button::keyShiftDec:      return 1u << MCU_BUTTON_KEY_SHIFT_L;
        case Button::keyShiftInc:      return 1u << MCU_BUTTON_KEY_SHIFT_R;
        case Button::midiChannelDec:   return 1u << MCU_BUTTON_MIDI_CH_L;
        case Button::midiChannelInc:   return 1u << MCU_BUTTON_MIDI_CH_R;
        case Button::all:              return 1u << MCU_BUTTON_INST_ALL;
        case Button::mute:             return 1u << MCU_BUTTON_INST_MUTE;
        case Button::solo:             return (1u << MCU_BUTTON_INST_ALL) | (1u << MCU_BUTTON_INST_MUTE);
        case Button::standbyOn: case Button::standbyOff:
        case Button::fastScrollOn: case Button::fastScrollOff:
        case Button::exclusiveOn: case Button::exclusiveOff:
        case Button::resetReceiveOn: case Button::resetReceiveOff:
        case Button::checksumIgnoreOn: case Button::checksumIgnoreOff:
        case Button::programReceiveOn: case Button::programReceiveOff: return 0;
    }

    return 0;
}

NukedSC55Emulator::RomFamily romFamilyForRomset (Romset romset) noexcept
{
    switch (romset)
    {
        case Romset::MK1:       return NukedSC55Emulator::RomFamily::sc55;
        case Romset::MK2:       return NukedSC55Emulator::RomFamily::sc55mk2;
        case Romset::SC155:
        case Romset::SC155MK2:  return NukedSC55Emulator::RomFamily::sc155;
        default:                return NukedSC55Emulator::RomFamily::other;
    }
}

const char* frontPanelButtonName (NukedSC55Emulator::FrontPanelButton button) noexcept
{
    using Button = NukedSC55Emulator::FrontPanelButton;

    switch (button)
    {
        case Button::partDec:          return "part-dec";
        case Button::partInc:          return "part-inc";
        case Button::instrumentDec:    return "instrument-dec";
        case Button::instrumentInc:    return "instrument-inc";
        case Button::levelDec:         return "level-dec";
        case Button::levelInc:         return "level-inc";
        case Button::panDec:           return "pan-dec";
        case Button::panInc:           return "pan-inc";
        case Button::reverbDec:        return "reverb-dec";
        case Button::reverbInc:        return "reverb-inc";
        case Button::chorusDec:        return "chorus-dec";
        case Button::chorusInc:        return "chorus-inc";
        case Button::keyShiftDec:      return "key-shift-dec";
        case Button::keyShiftInc:      return "key-shift-inc";
        case Button::midiChannelDec:   return "midi-channel-dec";
        case Button::midiChannelInc:   return "midi-channel-inc";
        case Button::all:              return "all";
        case Button::mute:             return "mute";
        case Button::solo:             return "solo";
        case Button::standbyOn:        return "standby on";
        case Button::standbyOff:       return "standby off";
        case Button::fastScrollOn:     return "fast scroll on";
        case Button::fastScrollOff:    return "fast scroll off";
        case Button::exclusiveOn:      return "SysEx receive on";
        case Button::exclusiveOff:     return "SysEx receive off";
        case Button::resetReceiveOn:   return "Reset receive on";
        case Button::resetReceiveOff:  return "Reset receive off";
        case Button::checksumIgnoreOn: return "Ignore checksum on";
        case Button::checksumIgnoreOff:return "Ignore checksum off";
        case Button::programReceiveOn: return "Program Change receive on";
        case Button::programReceiveOff:return "Program Change receive off";
    }

    return "unknown";
}
}

// The backend's LCD renderer is intentionally callback based. This adapter
// keeps the backend alive so LCD_Write records the hardware state, then takes a
// small character-RAM snapshot for the message thread. The mask is rendered
// with the same segment coordinates as Nuked's original plugin-side
// LCD_GetDisplayMask(), while the MCU/DSP implementation itself remains
// entirely in jcmoyer's backend.
class LcdCaptureBackend final : public LCD_Backend
{
public:
    bool Start (const lcd_t& next) override
    {
        const std::lock_guard lock (mutex);
        lcd = &next;
        snapshot = {};
        snapshot.width = next.width;
        snapshot.height = next.height;
        snapshot.valid = true;
        return true;
    }

    void Stop() override
    {
        const std::lock_guard lock (mutex);
        lcd = nullptr;
        snapshot = {};
    }

    void Render() override
    {
        captureState();
    }

    // LCD_Render() also draws the complete 741x268 pixel framebuffer. The
    // plugin only needs the character data below, so the audio callback uses
    // this lightweight path instead of invoking that full renderer.
    void captureState() noexcept
    {
        const lcd_t* current = nullptr;
        {
            std::unique_lock lock (mutex, std::try_to_lock);
            if (! lock.owns_lock())
                return;
            current = lcd;
        }

        if (current == nullptr)
            return;

        Snapshot next;
        {
            // LCD_Backend::Start exposes the device as const, while the LCD
            // implementation uses this mutex to protect its live registers.
            // The object itself is owned by Emulator and is non-const; only
            // the callback's view is const.
            auto& mutableLcd = const_cast<lcd_t&> (*current);
            if (! mutableLcd.mutex.try_lock())
                return;
            next.valid = true;
            next.width = current->width;
            next.height = current->height;
            next.enabled = current->enable.load (std::memory_order_relaxed);
            if (current->mcu != nullptr)
            {
                next.isCm300 = current->mcu->is_cm300;
                next.isSt = current->mcu->is_st;
                next.isScb55 = current->mcu->is_scb55;
                next.isJv880 = current->mcu->is_jv880;
            }
            next.displayControl = current->LCD_C;
            next.displayAddress = current->LCD_DD_RAM;
            std::copy (std::begin (current->LCD_Data), std::end (current->LCD_Data), next.data.begin());
            std::copy (std::begin (current->LCD_CG), std::end (current->LCD_CG), next.cg.begin());
            mutableLcd.mutex.unlock();
        }

        std::unique_lock lock (mutex, std::try_to_lock);
        if (! lock.owns_lock())
            return;
        if (lcd == current)
            snapshot = next;
    }

    // Message-thread rendering model. No firmware or audio-thread LCD writes.
    void captureNativeState (const sc55::SynthState& state, bool fastScroll)
    {
        const auto display = nativeDisplay.update (state.displayEvents, fastScroll);
        Snapshot next;
        next.valid = next.enabled = ! state.failed;
        next.width = LCD_DISPLAY_WIDTH; next.height = LCD_DISPLAY_HEIGHT;
        next.data.fill (' ');
        const auto gsPart = [](unsigned p) { return p == 9 ? 0u : p < 9 ? p + 1 : p; };
        const auto& part = state.parts[gsPart (state.selectedPart)];
        const auto number = [&](unsigned offset, int value)
        {
            char text[16]; std::snprintf (text, sizeof (text), "%3d", value);
            std::copy_n (text, 3, next.data.begin() + offset);
        };
        number (0, state.selectedPart + 1);
        number (3, part.program + 1);
        std::copy (part.name.begin(), part.name.end(), next.data.begin() + 7);
        number (40, part.volume);
        number (43, int (part.pan) - 64);
        number (49, part.reverb); number (46, part.chorus);
        number (52, int (part.keyShift) - 64);
        if (part.channel < 16) number (55, part.channel + 1);
        else std::copy_n ("OFF", 3, next.data.begin() + 55);
        if (state.allSelected)
        {
            next.data.fill (' ');
            std::copy_n ("ALL", 3, next.data.begin());
            std::copy_n ("ALL PARTS", 9, next.data.begin() + 7);
            number (40, state.masterVolume);
            number (43, int (state.masterPan) - 64);
            number (49, state.reverbLevel); number (46, state.chorusLevel);
            number (52, int (state.masterKeyShift) - 64);
            number (55, state.midiInput.deviceId + 1);
        }
        if (display.text.visible)
        {
            sc55::DisplayFrame::Line normal;
            std::copy_n (next.data.begin() + 3, normal.size(), normal.begin());
            const auto text = display.text.compose (normal);
            std::copy (text.begin(), text.end(), next.data.begin() + 3);
        }
        std::array<uint16_t, 16> meterTargets {};
        for (unsigned p = 0; p < 16; ++p)
            meterTargets[p] = state.parts[gsPart (p)].envelopeLevel;
        const auto meterBars = nativeMeters.update (meterTargets,
            std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count());
        for (unsigned matrix = 0; matrix < 2; ++matrix)
        {
            for (unsigned group = 0; group < 4; ++group)
                next.data[20 + matrix * 40 + group] = uint8_t (matrix * 4 + group);
            for (unsigned p = 0; p < 16; ++p)
            {
                const unsigned bars = meterBars[p];
                for (unsigned row = 0; row < 8; ++row)
                    if (bars >= (1 - matrix) * 8 + 8 - row)
                        next.cg[(matrix * 4 + p / 5) * 8 + row] |= uint8_t (1u << (4 - p % 5));
            }
        }
        // 04:3065..3072 selects the received 64-byte CG bank while CF34
        // is active; the LCD consumes it in the same order as normal bars.
        if (display.bitmapVisible)
            std::copy (display.bitmap.begin(), display.bitmap.end(), next.cg.begin());
        const std::lock_guard lock (mutex);
        snapshot = next;
    }

    bool copyMask (uint8_t* destination, size_t destinationStride) const
    {
        if (destination == nullptr || destinationStride < static_cast<size_t> (LCD_DISPLAY_WIDTH))
            return false;

        Snapshot current;
        {
            const std::lock_guard lock (mutex);
            current = snapshot;
        }

        for (int y = 0; y < LCD_DISPLAY_HEIGHT; ++y)
            std::memset (destination + static_cast<size_t> (y) * destinationStride,
                         0, static_cast<size_t> (LCD_DISPLAY_WIDTH));

        if (! current.valid
            || current.width != static_cast<size_t> (LCD_DISPLAY_WIDTH)
            || current.height != static_cast<size_t> (LCD_DISPLAY_HEIGHT)
            || ! current.enabled
            || current.isCm300 || current.isSt || current.isScb55 || current.isJv880)
        {
            return false;
        }

        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                11, 34 + i * 35, current.data[static_cast<size_t> (i)], current);
        for (int i = 0; i < 16; ++i)
            renderStandardMask (destination, destinationStride,
                                11, 153 + i * 35, current.data[static_cast<size_t> (3 + i)], current);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                75, 34 + i * 35, current.data[static_cast<size_t> (40 + i)], current);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                75, 153 + i * 35, current.data[static_cast<size_t> (43 + i)], current);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                139, 34 + i * 35, current.data[static_cast<size_t> (49 + i)], current);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                139, 153 + i * 35, current.data[static_cast<size_t> (46 + i)], current);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                203, 34 + i * 35, current.data[static_cast<size_t> (52 + i)], current);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                203, 153 + i * 35, current.data[static_cast<size_t> (55 + i)], current);

        renderLevelIndicators (destination, destinationStride, current);
        renderLeftRightIndicator (destination, destinationStride, current);
        return true;
    }

private:
    struct Snapshot
    {
        bool valid = false;
        bool enabled = false;
        bool isCm300 = false;
        bool isSt = false;
        bool isScb55 = false;
        bool isJv880 = false;
        size_t width = 0;
        size_t height = 0;
        uint32_t displayControl = 0;
        uint32_t displayAddress = 0;
        std::array<uint8_t, 80> data {};
        std::array<uint8_t, 64> cg {};
    };

    static const uint8_t* glyph (const Snapshot& state, uint8_t character)
    {
        if (character >= 16)
            return &lcd_font[character - 16][0];

        return &state.cg[(character & 7u) * 8u];
    }

    static void setMaskRectangle (uint8_t* destination, size_t stride,
                                  int screenY, int screenX, int height, int width,
                                  uint8_t value)
    {
        for (int y = 0; y < height; ++y)
        {
            if (screenY + y < 0 || screenY + y >= LCD_DISPLAY_HEIGHT)
                continue;

            for (int x = 0; x < width; ++x)
            {
                if (screenX + x >= 0 && screenX + x < LCD_DISPLAY_WIDTH)
                {
                    destination[static_cast<size_t> (screenY + y) * stride
                                + static_cast<size_t> (screenX + x)] = value;
                }
            }
        }
    }

    static void renderStandardMask (uint8_t* destination, size_t stride,
                                    int x, int y, uint8_t character,
                                    const Snapshot& state)
    {
        const auto* f = glyph (state, character);
        for (int i = 0; i < 7; ++i)
        {
            for (int j = 0; j < 5; ++j)
            {
                const uint8_t value = (f[i] & (1 << (4 - j))) != 0 ? 1 : 2;
                setMaskRectangle (destination, stride,
                                  x + i * 6, y + j * 6, 5, 5, value);
            }
        }
    }

    static void renderLevelMask (uint8_t* destination, size_t stride,
                                 int x, int y, uint8_t character, int width,
                                 const Snapshot& state)
    {
        const auto* f = glyph (state, character);
        for (int i = 0; i < 8; ++i)
        {
            for (int j = 0; j < width; ++j)
            {
                const uint8_t value = (f[i] & (1 << (4 - j))) != 0 ? 1 : 2;
                setMaskRectangle (destination, stride,
                                  x + i * 11, y + j * 26, 9, 24, value);
            }
        }
    }

    static void renderLevelIndicators (uint8_t* destination, size_t stride,
                                       const Snapshot& state)
    {
        // These coordinates are outside the 741x268 SC-55 LCD mask in the
        // current Nuked layout, but retaining the source renderer's calls here
        // keeps this adapter aligned if the panel dimensions are extended.
        for (int i = 0; i < 2; ++i)
        {
            for (int j = 0; j < 4; ++j)
            {
                renderLevelMask (destination, stride,
                                 71 + i * 88, 293 + j * 130,
                                 state.data[static_cast<size_t> (20 + j + i * 40)],
                                 j == 3 ? 1 : 5, state);
            }
        }
    }

    static void renderLeftRightIndicator (uint8_t* destination, size_t stride,
                                           const Snapshot& state)
    {
        static constexpr uint8_t pattern[2][12][11] =
        {
            {
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
                { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }
            },
            {
                { 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0 },
                { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0 },
                { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 },
                { 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1 },
                { 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1 }
            }
        };
        static constexpr int origin[2][2] = { { 70, 264 }, { 232, 264 } };

        const auto* f = glyph (state, state.data[58]);
        const uint8_t value = (f[0] & 1) != 0 ? 1 : 2;
        for (int letter = 0; letter < 2; ++letter)
        {
            for (int i = 0; i < 12; ++i)
            {
                for (int j = 0; j < 11; ++j)
                {
                    if (pattern[letter][i][j] != 0)
                    {
                        setMaskRectangle (destination, stride,
                                          origin[letter][1] + j,
                                          origin[letter][0] + i,
                                          1, 1, value);
                    }
                }
            }
        }
    }

    mutable std::mutex mutex;
    const lcd_t* lcd = nullptr;
    Snapshot snapshot;
    sc55::DisplayPresentation nativeDisplay; // Message-thread only.
    NativeMeterDecay nativeMeters; // Message-thread only.
};

NukedSC55Emulator::NukedSC55Emulator()
    : lcdBackend (std::make_unique<LcdCaptureBackend>())
{
    installBackendDiagnostics();
}

NukedSC55Emulator::~NukedSC55Emulator()
{
    release();
}

void NukedSC55Emulator::setError (const std::string& message)
{
    error = message;
}

bool NukedSC55Emulator::hasRomSet (const std::string& romDirectory, bool* supportsNative)
{
    if (supportsNative != nullptr)
        *supportsNative = false;
    installBackendDiagnostics();

    std::error_code filesystemError;
    // JUCE supplies UTF-8; a narrow path constructor uses the ANSI code page on Windows.
    const auto path = std::filesystem::u8path (romDirectory);
    if (! std::filesystem::is_directory (path, filesystemError))
        return false;

    common::LoadRomsetResult result {};
    common::RomOverrides overrides {};
    const auto loadError = common::LoadRomset (path, {},
                                               common::RomLoader::Hashing, overrides, result);
    if (loadError != common::LoadRomsetError {})
        return false;

    if (supportsNative != nullptr)
    {
        const auto& data = result.romset_info.rom_data;
        *supportsNative = sc55::CanImportSoundData (data[static_cast<size_t> (RomLocation::ROM1)],
                                                  data[static_cast<size_t> (RomLocation::ROM2)]);
    }
    return true;
}

void NukedSC55Emulator::logRomSetDiagnostics (const std::string& romDirectory)
{
#if JUCE_DEBUG
    const auto path = std::filesystem::u8path (romDirectory);
    std::fprintf (stderr, "[DEBUG-SC55] ROM diagnostics path=\"%s\"\n",
                  romDirectory.c_str());

    std::error_code filesystemError;
    if (! std::filesystem::is_directory (path, filesystemError))
    {
        std::fprintf (stderr, "[DEBUG-SC55] ROM diagnostics directory=0 error=\"%s\"\n",
                      filesystemError ? filesystemError.message().c_str() : "not a directory");
        std::fflush (stderr);
        return;
    }

    size_t regularFileCount = 0;
    std::error_code iteratorError;
    std::filesystem::directory_iterator iterator (path, iteratorError);
    const std::filesystem::directory_iterator end;
    for (; iterator != end && ! iteratorError; iterator.increment (iteratorError))
    {
        std::error_code entryError;
        if (! iterator->is_regular_file (entryError))
            continue;

        ++regularFileCount;
        const auto fileSize = iterator->file_size (entryError);
        std::fprintf (stderr, "[DEBUG-SC55] ROM file[%zu]=\"%s\" bytes=%llu readable=%d\n",
                      regularFileCount,
                      iterator->path().generic_string().c_str(),
                      static_cast<unsigned long long> (entryError ? 0 : fileSize),
                      entryError ? 0 : 1);
    }

    if (iteratorError)
        std::fprintf (stderr, "[DEBUG-SC55] ROM directory iteration error=\"%s\"\n",
                      iteratorError.message().c_str());

    common::LoadRomsetResult result {};
    common::RomOverrides overrides {};
    const auto loadError = common::LoadRomset (path, {}, common::RomLoader::Hashing, overrides, result);
    std::fprintf (stderr,
                  "[DEBUG-SC55] ROM scan files=%zu error=\"%s\" picked=\"%s\"\n",
                  regularFileCount,
                  common::ToCString (loadError),
                  result.picked_name.c_str());
    common::PrintLoadRomsetDiagnostics (stderr, loadError, result);
    std::fflush (stderr);
#else
    (void) romDirectory;
#endif
}

bool NukedSC55Emulator::usesNativeEngine (EngineMode mode) noexcept
{
    if (mode != EngineMode::environment)
        return mode == EngineMode::native;
    const auto* option = std::getenv ("NUKED_SC55_USE_H8");
    return option == nullptr || std::string_view (option) != "1";
}

bool NukedSC55Emulator::initialise (const std::string& romDirectory, double newHostSampleRate,
                                  const std::string& nativeCacheDirectory, EngineMode mode, unsigned maximumVoices)
{
    sc55debug::log ("initialise requested directory=\"%s\" hostRate=%.2f",
                    romDirectory.c_str(), newHostSampleRate);

    release();
    error.clear();
    hostSampleRate = newHostSampleRate;

    if (! std::isfinite (hostSampleRate) || hostSampleRate <= 0.0)
    {
        setError ("Invalid host sample rate");
        return false;
    }

    auto nextRoms = std::make_unique<common::LoadRomsetResult>();
    common::RomOverrides overrides {};
    const auto loadError = common::LoadRomset (std::filesystem::u8path (romDirectory), {},
                                               common::RomLoader::Hashing, overrides, *nextRoms);
    if (loadError != common::LoadRomsetError {})
    {
        setError (std::string ("ROM loading failed: ") + common::ToCString (loadError));
        sc55debug::log ("ROM initialisation failed: %s", error.c_str());
        return false;
    }

    if (usesNativeEngine (mode)) try
    {
        const auto& data = nextRoms->romset_info.rom_data;
        const auto generated = sc55::EnsureNativeSoundDataCache (
            data[static_cast<size_t> (RomLocation::ROM1)],
            data[static_cast<size_t> (RomLocation::ROM2)],
            nativeCacheDirectory.empty() ? juce::File()
                : juce::File (juce::String::fromUTF8 (nativeCacheDirectory.c_str())));
        if (generated)
            sc55debug::log ("Generated sc55-native.sdata from the loaded ROM pair");
    }
    catch (const std::exception& exception)
    {
        setError (std::string ("Native sound-data preparation failed: ") + exception.what());
        return false;
    }

    std::unique_ptr<Emulator> nextCore;
    std::unique_ptr<sc55::NativeSynth> nextNativePlayer;
    // Normal app and plug-in launches use the C++ controller, including AUv3
    // extension processes which do not inherit a Standalone scheme's environment.
    // The H8 implementation remains an explicitly selected comparison oracle.
    if (usesNativeEngine (mode))
    {
        const auto& loaded = nextRoms->romset_info.rom_data;
        if (! sc55::CanImportSoundData (loaded[static_cast<size_t> (RomLocation::ROM1)],
                                       loaded[static_cast<size_t> (RomLocation::ROM2)]))
        {
            setError ("The C++ engine requires SC-55 v1.21 ROMs. Use NUKED_SC55_USE_H8=1 for other ROM sets.");
            return false;
        }
        const auto asset = juce::File (juce::String::fromUTF8 (nativeCacheDirectory.c_str()))
            .getChildFile ("mk1-v1.21-md15/sc55-native.sdata");
        juce::MemoryBlock bytes;
        if (! asset.loadFileAsData (bytes))
        {
            setError ("Could not load the generated native sound data");
            return false;
        }
        const auto& data = nextRoms->romset_info.rom_data;
        try
        {
            // Explicit comparison switch, not the native engine's default:
            // some tones still differ substantially from the chip renderer.
            const auto* simulation = std::getenv ("SC55_SIM");
            const auto rendering = simulation != nullptr && std::string_view (simulation) == "1"
                ? sc55::NativeSynth::VoiceRendering::nativeVoices
                : sc55::NativeSynth::VoiceRendering::referenceChip;
            nextNativePlayer = std::make_unique<sc55::NativeSynth> (
                std::span (static_cast<const uint8_t*> (bytes.getData()), bytes.getSize()),
                data[static_cast<size_t> (RomLocation::ROM1)],
                data[static_cast<size_t> (RomLocation::ROM2)],
                data[static_cast<size_t> (RomLocation::WAVEROM1)],
                data[static_cast<size_t> (RomLocation::WAVEROM2)],
                data[static_cast<size_t> (RomLocation::WAVEROM3)], rendering, maximumVoices);
        }
        catch (const std::exception& exception)
        {
            setError (exception.what());
            return false;
        }
        sourceSampleRate = sc55::NativeSynth::sampleRate;
        sc55debug::log ("NATIVE SYNTH: MIDI, voice control and PCM; no H8 or LCD execution");
    }
    else
    {
        nextCore = std::make_unique<Emulator>();
        EMU_Options options;
        options.lcd_backend = lcdBackend.get();
        if (! nextCore->Init (options)
            || ! nextCore->LoadRoms (nextRoms->romset, nextRoms->romset_info))
        {
            setError ("Failed to initialise the selected SC-55 ROM set");
            return false;
        }
        nextCore->Reset();
        // Comparison mode must execute H8, including routines for which the
        // older emulator has optional v1.21 C++ instruction shortcuts.
        nextCore->GetMCU().native_v121_enabled = false;
        // The UI's unoptimised mode means H8 plus the original integer PCM
        // renderer, regardless of the Emulator constructor's defaults or
        // SC55_SIM/SC55_FXSIM. Select before querying the output sample rate.
        PCM_UseSimulation (nextCore->GetPCM(), false);
        nextCore->GetPCM().use_float_effects = false;
        nextCore->GetMCU().button_pressed.store (0, std::memory_order_relaxed);
        nextCore->SetSampleCallback (&NukedSC55Emulator::sampleSink, this);
        if (! nextCore->StartLCD())
        {
            nextCore->StopLCD();
            setError ("Failed to initialise the SC-55 LCD backend");
            return false;
        }
        sourceSampleRate = static_cast<double> (PCM_GetOutputFrequency (nextCore->GetPCM()));
    }
    {
        const std::lock_guard lock (coreMutex);
        core = std::move (nextCore);
        nativePlayer = std::move (nextNativePlayer);
        // RomsetInfo must outlive Emulator::LoadRoms(). Keep it beside the
        // core until release() destroys the core first.
        loadedRoms = std::move (nextRoms);
    }

    debugRomFamily.store (static_cast<uint8_t> (romFamilyForRomset (loadedRoms->romset)),
                          std::memory_order_release);

    sourceRead.store (0, std::memory_order_relaxed);
    sourceWrite.store (0, std::memory_order_relaxed);
    renderCallCount = 0;
    midiPacketCount.store (0, std::memory_order_relaxed);
    sourceSamplesProduced.store (0, std::memory_order_relaxed);
    sourceNonZeroSamples.store (0, std::memory_order_relaxed);
    sourceDroppedSamples.store (0, std::memory_order_relaxed);
    midiDroppedBytes.store (0, std::memory_order_relaxed);
    sourceUnderruns.store (0, std::memory_order_relaxed);
    lastLoggedMidiPacketCount = 0;
    sourcePosition = 0.0;
    lastSourceFrame[0] = 0.0f;
    lastSourceFrame[1] = 0.0f;
    dcCoefficient = 1.0 - (2.0 * pi * dcBlockerHz / hostSampleRate);
    dcPreviousInput[0] = dcPreviousInput[1] = 0.0f;
    dcPreviousOutput[0] = dcPreviousOutput[1] = 0.0f;
    midiDropMessage = false;
    gsResetSent = false;

    if (nativePlayer != nullptr)
        nativePlayer->setMidiInputSettings (midiInputState.read());
    publishDebugState();
    if (nativePlayer != nullptr)
        nativeStateExchange.publish (nativePlayer->state());
    nativeEngineActive.store (nativePlayer != nullptr, std::memory_order_release);
    ready.store (true, std::memory_order_release);

    sc55debug::log ("initialise succeeded romset=%s mk1=%d sourceRate=%.2f",
                    loadedRoms != nullptr ? RomsetName (loadedRoms->romset) : "unknown",
                    nativePlayer != nullptr || (core != nullptr && core->GetMCU().is_mk1) ? 1 : 0, sourceSampleRate);
    return true;
}

void NukedSC55Emulator::release()
{
    nativeEngineActive.store (false, std::memory_order_release);
    const bool wasReady = ready.load (std::memory_order_acquire);
    if (wasReady)
    {
        sc55debug::log ("release after renders=%llu midiPackets=%llu sourceSamples=%llu nonZero=%llu",
                        static_cast<unsigned long long> (renderCallCount),
                        static_cast<unsigned long long> (midiPacketCount.load (std::memory_order_relaxed)),
                        static_cast<unsigned long long> (sourceSamplesProduced.load (std::memory_order_relaxed)),
                        static_cast<unsigned long long> (sourceNonZeroSamples.load (std::memory_order_relaxed)));
    }

    ready.store (false, std::memory_order_release);
    debugRomFamily.store (static_cast<uint8_t> (RomFamily::unknown),
                          std::memory_order_release);

    {
        const std::lock_guard lock (coreMutex);
        if (core != nullptr)
            core->StopLCD();
        else if (lcdBackend != nullptr)
            lcdBackend->Stop();
        nativePlayer.reset();
        core.reset();
        loadedRoms.reset();
    }

    clearFrontPanelButtons();

    sourceRead.store (0, std::memory_order_relaxed);
    sourceWrite.store (0, std::memory_order_relaxed);
    renderCallCount = 0;
    midiPacketCount.store (0, std::memory_order_relaxed);
    sourceSamplesProduced.store (0, std::memory_order_relaxed);
    sourceNonZeroSamples.store (0, std::memory_order_relaxed);
    sourceDroppedSamples.store (0, std::memory_order_relaxed);
    midiDroppedBytes.store (0, std::memory_order_relaxed);
    sourceUnderruns.store (0, std::memory_order_relaxed);
    lastLoggedMidiPacketCount = 0;
    sourcePosition = 0.0;
    midiDropMessage = false;
    gsResetSent = false;
    debugAllLed.store (false, std::memory_order_relaxed);
    debugMuteLed.store (false, std::memory_order_relaxed);
    debugSoloEnabled.store (false, std::memory_order_relaxed);
    debugStandby.store (false, std::memory_order_relaxed);
    debugFastDisplayScroll.store (false, std::memory_order_relaxed);
}

void NukedSC55Emulator::clearPendingMidi() noexcept
{
    const auto write = midiWrite.load (std::memory_order_acquire);
    midiRead.store (write, std::memory_order_release);
}

void NukedSC55Emulator::clearFrontPanelButtons() noexcept
{
    nativePanelRead.store (nativePanelWrite.load (std::memory_order_acquire), std::memory_order_release);
    nativePanelGeneration.fetch_add (1, std::memory_order_release);
    frontPanelPendingMask.store (0, std::memory_order_release);
    frontPanelPressedMask.store (0, std::memory_order_release);
    frontPanelReleaseFrame.store (0, std::memory_order_release);
}

void NukedSC55Emulator::sendMidi (const uint8_t* data, int size)
{
    if (data == nullptr || size <= 0)
        return;

    midiPacketCount.fetch_add (1, std::memory_order_relaxed);

    for (int i = 0; i < size; ++i)
    {
        if (! enqueueMidiByte (data[i]))
        {
            midiDroppedBytes.fetch_add (static_cast<uint64_t> (size - i), std::memory_order_relaxed);
            break;
        }
    }
}

void NukedSC55Emulator::pressFrontPanelButton (FrontPanelButton button)
{
    if (nativeEngineActive.load (std::memory_order_acquire))
    {
        const auto generation = nativePanelGeneration.load (std::memory_order_acquire);
        if (panelGeneration != generation)
        {
            panelGeneration = generation;
            panelPart = 0;
            panelAll = panelSolo = panelStandby = false;
        }
        const auto value = static_cast<unsigned> (button);
        if (panelStandby && value <= static_cast<unsigned> (FrontPanelButton::solo))
            return;
        if (button == FrontPanelButton::fastScrollOn || button == FrontPanelButton::fastScrollOff)
        {
            // Display-only preference. Never enqueue it for processBlock.
            if (panelStandby)
            {
                debugFastDisplayScroll.store (button == FrontPanelButton::fastScrollOn, std::memory_order_relaxed);
            }
            return;
        }
        const auto write = nativePanelWrite.load (std::memory_order_relaxed);
        const auto next = (write + 1) % nativePanelCapacity;
        if (next == nativePanelRead.load (std::memory_order_acquire))
        {
            sc55debug::log ("Native panel command queue is full");
            return;
        }
        sc55::SynthCommand command;
        using Kind = sc55::SynthCommand::Kind;
        command.part = panelPart;
        command.all = panelAll;
        const auto delta = (value & 1u) != 0 ? 1 : -1;
        if (value < 2)
        {
            panelPart = static_cast<uint8_t> (std::clamp (int (panelPart) + delta, 0, 15));
            command.part = panelPart;
        }
        else if (value < 16)
        {
            command.kind = Kind::adjust;
            command.parameter = static_cast<sc55::PartParameter> ((value - 2) / 2);
            command.delta = delta;
        }
        else switch (button)
        {
            case FrontPanelButton::all:
                panelAll = ! panelAll;
                command.all = panelAll;
                command.cancelBitmap = true;
                break;
            case FrontPanelButton::mute: command.kind = Kind::toggleMute; break;
            case FrontPanelButton::solo:
                command.kind = Kind::solo;
                command.enabled = panelSolo = ! panelSolo;
                break;
            case FrontPanelButton::standbyOn: case FrontPanelButton::standbyOff:
                command.kind = Kind::standby;
                command.enabled = panelStandby = button == FrontPanelButton::standbyOn;
                break;
            case FrontPanelButton::exclusiveOn: case FrontPanelButton::exclusiveOff:
                command.kind = Kind::receiveExclusive;
                command.enabled = button == FrontPanelButton::exclusiveOn;
                break;
            case FrontPanelButton::resetReceiveOn: case FrontPanelButton::resetReceiveOff:
                command.kind = Kind::receiveReset;
                command.enabled = button == FrontPanelButton::resetReceiveOn;
                break;
            case FrontPanelButton::checksumIgnoreOn: case FrontPanelButton::checksumIgnoreOff:
                command.kind = Kind::ignoreChecksum;
                command.enabled = button == FrontPanelButton::checksumIgnoreOn;
                break;
            case FrontPanelButton::programReceiveOn: case FrontPanelButton::programReceiveOff:
                command.kind = Kind::receiveProgramChanges;
                command.enabled = button == FrontPanelButton::programReceiveOn;
                break;
            default: return;
        }
        nativePanelQueue[write] = command;
        nativePanelWrite.store (next, std::memory_order_release);
        return;
    }
    const auto mask = frontPanelButtonMask (button);
    if (mask == 0)
        return;

    frontPanelPendingMask.fetch_or (mask, std::memory_order_release);

    sc55debug::log ("front-panel button=%s mask=%08x",
                    frontPanelButtonName (button), mask);
}

float NukedSC55Emulator::blockDc (int channel, float input) noexcept
{
    const float output = input - dcPreviousInput[channel]
                       + static_cast<float> (dcCoefficient) * dcPreviousOutput[channel];
    dcPreviousInput[channel] = input;
    dcPreviousOutput[channel] = output;
    return output;
}

void NukedSC55Emulator::sampleSink (void* userData, const AudioFrame<int32_t>& sample)
{
    if (userData != nullptr)
        static_cast<NukedSC55Emulator*> (userData)->pushSample (sample);
}

void NukedSC55Emulator::pushSample (const AudioFrame<int32_t>& sample)
{
    sourceSamplesProduced.fetch_add (1, std::memory_order_relaxed);
    if (sample.left != 0 || sample.right != 0)
        sourceNonZeroSamples.fetch_add (1, std::memory_order_relaxed);

    const auto write = sourceWrite.load (std::memory_order_relaxed);
    const auto nextWrite = (write + 1) % sourceFifoFrames;
    if (nextWrite == sourceRead.load (std::memory_order_acquire))
    {
        sourceDroppedSamples.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    AudioFrame<float> normalized;
    Normalize (sample, normalized);
    sourceFifo[write][0] = normalized.left;
    sourceFifo[write][1] = normalized.right;
    sourceWrite.store (nextWrite, std::memory_order_release);
}

bool NukedSC55Emulator::enqueueMidiByte (uint8_t byte) noexcept
{
    const auto write = midiWrite.load (std::memory_order_relaxed);
    const auto nextWrite = (write + 1) % midiFifoBytes;
    if (nextWrite == midiRead.load (std::memory_order_acquire))
        return false;

    midiFifo[write] = byte;
    midiWrite.store (nextWrite, std::memory_order_release);
    return true;
}

uint32_t NukedSC55Emulator::availableSourceFrames() const noexcept
{
    const auto read = sourceRead.load (std::memory_order_acquire);
    const auto write = sourceWrite.load (std::memory_order_acquire);
    return write >= read ? write - read : sourceFifoFrames - (read - write);
}

const float* NukedSC55Emulator::sourceFrame (uint32_t offset) const noexcept
{
    const uint32_t index = (sourceRead.load (std::memory_order_acquire) + offset) % sourceFifoFrames;
    return sourceFifo[index];
}

void NukedSC55Emulator::consumeSourceFrames (uint32_t count) noexcept
{
    const auto read = sourceRead.load (std::memory_order_relaxed);
    const auto available = availableSourceFrames();
    count = std::min (count, available);
    sourceRead.store ((read + count) % sourceFifoFrames, std::memory_order_release);
}

void NukedSC55Emulator::drainNativePanel() noexcept
{
    auto read = nativePanelRead.load (std::memory_order_relaxed);
    const auto write = nativePanelWrite.load (std::memory_order_acquire);
    while (read != write)
    {
        if (! nativePlayer->applyCommand (nativePanelQueue[read]))
            break; // Backpressure: retain this command and its resolved target.
        read = (read + 1) % nativePanelCapacity;
    }
    nativePanelRead.store (read, std::memory_order_release);
}

void NukedSC55Emulator::updateFrontPanelButtons() noexcept
{
    const auto currentFrame = sourceSamplesProduced.load (std::memory_order_relaxed);
    const auto pendingMask = frontPanelPendingMask.exchange (0, std::memory_order_acquire);
    auto pressedMask = frontPanelPressedMask.load (std::memory_order_relaxed);

    if (pendingMask != 0)
    {
        pressedMask |= pendingMask;
        frontPanelPressedMask.store (pressedMask, std::memory_order_relaxed);

        const auto pulseFrames = static_cast<uint64_t> (std::max (1.0, sourceSampleRate * 0.05));
        const auto requestedRelease = currentFrame + pulseFrames;
        auto releaseFrame = frontPanelReleaseFrame.load (std::memory_order_relaxed);
        while (releaseFrame < requestedRelease
               && ! frontPanelReleaseFrame.compare_exchange_weak (
                   releaseFrame, requestedRelease,
                   std::memory_order_release, std::memory_order_relaxed))
        {
        }
    }

    const auto releaseFrame = frontPanelReleaseFrame.load (std::memory_order_acquire);
    if (pendingMask == 0 && pressedMask != 0 && currentFrame >= releaseFrame)
    {
        pressedMask = 0;
        frontPanelPressedMask.store (0, std::memory_order_relaxed);
    }

    if (core != nullptr)
        core->GetMCU().button_pressed.store (pressedMask, std::memory_order_relaxed);
}

void NukedSC55Emulator::drainMidi()
{
    if (core == nullptr && nativePlayer == nullptr)
        return;

    if (nativePlayer != nullptr)
    {
        auto read = midiRead.load (std::memory_order_relaxed);
        const auto write = midiWrite.load (std::memory_order_acquire);
        while (read != write)
        {
            const uint8_t byte = midiFifo[read];
            if (nativePlayer->push (std::span (&byte, 1)) != 1)
                break; // Retain the suffix until the native queue has room.
            read = (read + 1) % midiFifoBytes;
        }
        midiRead.store (read, std::memory_order_release);
        return;
    }

    auto& mcu = core->GetMCU();
    auto read = midiRead.load (std::memory_order_relaxed);
    const auto write = midiWrite.load (std::memory_order_acquire);
    while (read != write)
    {
        const uint8_t byte = midiFifo[read];

        if (byte >= 0x80 && byte < 0xf7)
        {
            const uint32_t backlog = (mcu.uart_write_ptr + uart_buffer_size - mcu.uart_read_ptr)
                                   % uart_buffer_size;
            const bool ringNearlyFull = backlog >= uart_buffer_size - uartRingHeadroom;

            // Do not gate MIDI on firmware boot. The UART ring is the hardware
            // boundary; once it is close to full, discard the rest of the
            // current MIDI message rather than overwriting unread bytes.
            midiDropMessage = ringNearlyFull;
        }

        if (! midiDropMessage)
            core->PostMIDI (byte);
        else
            midiDroppedBytes.fetch_add (1, std::memory_order_relaxed);

        read = (read + 1) % midiFifoBytes;
    }
    midiRead.store (read, std::memory_order_release);
}

void NukedSC55Emulator::publishDebugState() noexcept
{
    if (nativePlayer != nullptr)
    {
        const auto state = nativePlayer->state();
        debugCycles.store (state.renderedFrames * 625, std::memory_order_relaxed);
        debugVoiceMask.store (state.activeVoiceMask, std::memory_order_relaxed);
        debugVoiceMaskPending.store (state.activeVoiceMask, std::memory_order_relaxed);
        debugCp.store (0, std::memory_order_relaxed);
        debugPc.store (0, std::memory_order_relaxed);
        const unsigned part = state.selectedPart == 9 ? 0 : state.selectedPart < 9 ? state.selectedPart + 1 : state.selectedPart;
        debugAllLed.store (state.allSelected, std::memory_order_relaxed);
        debugMuteLed.store (state.allSelected ? state.globalMuted : state.parts[part].muted, std::memory_order_relaxed);
        debugSoloEnabled.store (state.soloEnabled, std::memory_order_relaxed);
        debugStandby.store (nativePlayer->standby(), std::memory_order_relaxed);
        return;
    }
    if (core == nullptr)
        return;

    const auto& mcu = core->GetMCU();
    const auto& pcm = core->GetPCM();
    debugCp.store (mcu.cp, std::memory_order_relaxed);
    debugPc.store (mcu.pc, std::memory_order_relaxed);
    debugCycles.store (mcu.cycles, std::memory_order_relaxed);
    debugSleep.store (mcu.sleep, std::memory_order_relaxed);
    debugScr.store (mcu.dev_register[DEV_SCR], std::memory_order_relaxed);
    debugSsr.store (mcu.dev_register[DEV_SSR], std::memory_order_relaxed);
    debugVoiceMask.store (pcm.voice_mask, std::memory_order_relaxed);
    debugVoiceMaskPending.store (pcm.voice_mask_pending, std::memory_order_relaxed);
    debugPcmConfig3c.store (pcm.config_reg_3c, std::memory_order_relaxed);
    debugPcmConfig3d.store (pcm.config_reg_3d, std::memory_order_relaxed);
    debugUartWrite.store (mcu.uart_write_ptr, std::memory_order_relaxed);
    debugUartRead.store (mcu.uart_read_ptr, std::memory_order_relaxed);

    // The SC-55 front-panel indicators are active-low outputs. The original
    // Nuked frontend exposed these as mcu_led; the backend keeps the raw port
    // values, so derive the two indicators from the same hardware outputs.
    const auto ledPort = mcu.is_mk1 ? mcu.io_sd : mcu.p0_data;
    debugAllLed.store ((ledPort & 0x40) == 0, std::memory_order_relaxed);
    debugMuteLed.store ((ledPort & 0x20) == 0, std::memory_order_relaxed);
}

NukedSC55Emulator::DebugState NukedSC55Emulator::getDebugState() const noexcept
{
    // 次にエミュレータが回るときに公開してもらう。返す値は 1 回ぶん古いが、
    // 表示のための情報なので問題にならない。
    debugStateRequested.store (true, std::memory_order_release);

    DebugState state;
    state.nativeEngine = nativeEngineActive.load (std::memory_order_acquire);
    state.ready = ready.load (std::memory_order_acquire);
    state.backendRunning = state.ready;
    state.romFamily = static_cast<RomFamily> (debugRomFamily.load (std::memory_order_acquire));
    state.cp = debugCp.load (std::memory_order_relaxed);
    state.pc = debugPc.load (std::memory_order_relaxed);
    state.cycles = debugCycles.load (std::memory_order_relaxed);
    state.sleep = debugSleep.load (std::memory_order_relaxed);
    state.scr = debugScr.load (std::memory_order_relaxed);
    state.ssr = debugSsr.load (std::memory_order_relaxed);
    state.voiceMask = debugVoiceMask.load (std::memory_order_relaxed);
    state.voiceMaskPending = debugVoiceMaskPending.load (std::memory_order_relaxed);
    state.pcmConfig3c = debugPcmConfig3c.load (std::memory_order_relaxed);
    state.pcmConfig3d = debugPcmConfig3d.load (std::memory_order_relaxed);
    state.uartWrite = debugUartWrite.load (std::memory_order_relaxed);
    state.uartRead = debugUartRead.load (std::memory_order_relaxed);
    state.allLed = debugAllLed.load (std::memory_order_relaxed);
    state.muteLed = debugMuteLed.load (std::memory_order_relaxed);
    state.soloEnabled = debugSoloEnabled.load (std::memory_order_relaxed);
    state.standby = debugStandby.load (std::memory_order_relaxed);
    state.fastDisplayScroll = debugFastDisplayScroll.load (std::memory_order_relaxed);
    const auto input = midiInputState.read();
    state.receiveExclusive = input.receiveExclusive;
    state.receiveReset = input.receiveReset;
    state.ignoreChecksum = input.ignoreChecksum;
    state.receiveProgramChanges = input.receiveProgramChanges;
    state.sourceFrames = availableSourceFrames();
    state.midiPackets = midiPacketCount.load (std::memory_order_relaxed);
    state.midiDroppedBytes = midiDroppedBytes.load (std::memory_order_relaxed);
    state.sourceSamplesProduced = sourceSamplesProduced.load (std::memory_order_relaxed);
    state.sourceNonZeroSamples = sourceNonZeroSamples.load (std::memory_order_relaxed);
    state.sourceDroppedSamples = sourceDroppedSamples.load (std::memory_order_relaxed);
    state.sourceUnderruns = sourceUnderruns.load (std::memory_order_relaxed);
    return state;
}

void NukedSC55Emulator::driveCoreUntilSourceFrames (uint32_t minimumFrames) noexcept
{
    if (core == nullptr && nativePlayer == nullptr)
        return;

    if (nativePlayer != nullptr)
    {
        if (const auto input = midiInputState.takeRestore())
            nativePlayer->setMidiInputSettings (*input);
        drainMidi();
        drainNativePanel();
        std::array<AudioFrame<int32_t>, 256> frames;
        while (availableSourceFrames() < minimumFrames && ! nativePlayer->failed())
        {
            const auto count = std::min<uint32_t> (frames.size(), minimumFrames - availableSourceFrames());
            nativePlayer->render (std::span (frames.data(), count));
            for (uint32_t i = 0; i < count; ++i)
                sampleSink (this, frames[i]);
            drainMidi();
        }
        if (nativePlayer->failed())
            ready.store (false, std::memory_order_release);
        midiInputState.publish (nativePlayer->midiInputSettings());
        if (nativeStateRequested.exchange (false, std::memory_order_acquire))
            nativeStateExchange.publish (nativePlayer->state());
        if (debugStateRequested.exchange (false, std::memory_order_acquire))
            publishDebugState();
        return;
    }

    updateFrontPanelButtons();

    auto& mcu = core->GetMCU();
    if (! gsResetSent
        && (((mcu.dev_register[DEV_SCR] & 0x10) != 0 && mcu.sleep != 0)
            || mcu.cycles > gsResetFallbackCycles))
    {
        core->PostSystemReset (EMU_SystemReset::GS_RESET);
        gsResetSent = true;
        sc55debug::log ("GS reset sent at cycles=%llu",
                        static_cast<unsigned long long> (mcu.cycles));
    }

    // MIDI is inserted at the current emulated audio position. The firmware's
    // UART model still determines when the byte is actually consumed.
    drainMidi();

    while (availableSourceFrames() < minimumFrames)
    {
        core->Step();

        // The old worker checked this before every instruction. Checking
        // immediately after a step preserves the reset boundary without adding
        // a second MIDI polling pass to every emulated instruction.
        auto& steppedMcu = core->GetMCU();
        if (! gsResetSent
            && (((steppedMcu.dev_register[DEV_SCR] & 0x10) != 0 && steppedMcu.sleep != 0)
                || steppedMcu.cycles > gsResetFallbackCycles))
        {
            core->PostSystemReset (EMU_SystemReset::GS_RESET);
            gsResetSent = true;
            sc55debug::log ("GS reset sent at cycles=%llu",
                            static_cast<unsigned long long> (steppedMcu.cycles));
        }
    }

    // UI が見に来ていなければ何もしない。状態はエミュレータを動かしているスレッドが
    // 持っているので GUI から直接は読めないが、要求されたときだけ公開すれば、
    // 音声コールバックに表示のための仕事は残らない。
    if (debugStateRequested.exchange (false, std::memory_order_acquire))
        publishDebugState();
}

bool NukedSC55Emulator::getNativeState (sc55::SynthState& destination) const noexcept
{
    if (! nativeEngineActive.load (std::memory_order_acquire))
        return false;
    destination = nativeStateExchange.read();
    // Snapshot consumer (UI): no live synth access, PCM reads or audio locks.
    destination.calculateDisplayLevels();
    nativeStateRequested.store (true, std::memory_order_release);
    return true;
}

bool NukedSC55Emulator::copyLcdDisplay (uint8_t* destination, size_t destinationStride) const
{
    if (destination == nullptr || destinationStride < static_cast<size_t> (LCD_DISPLAY_WIDTH))
        return false;

    if (lcdBackend == nullptr)
    {
        for (int y = 0; y < LCD_DISPLAY_HEIGHT; ++y)
            std::memset (destination + static_cast<size_t> (y) * destinationStride,
                         0, static_cast<size_t> (LCD_DISPLAY_WIDTH));
        return false;
    }

    // LCD の文字 RAM を取り込むのはここ。読みに来た側のスレッドで行う。
    // 以前は音声コールバックの中で毎ブロック取り込んでいたが、これは表示のための
    // データ作成であって信号処理ではない。
    sc55::SynthState nativeState;
    if (getNativeState (nativeState)) lcdBackend->captureNativeState (nativeState, debugFastDisplayScroll.load (std::memory_order_relaxed));
    else lcdBackend->captureState();
    return lcdBackend->copyMask (destination, destinationStride);
}

void NukedSC55Emulator::render (float* left, float* right, int numSamples)
{
    if (left == nullptr || numSamples <= 0)
        return;
    if (! ready.load (std::memory_order_acquire)
        || hostSampleRate <= 0.0 || sourceSampleRate <= 0.0)
    {
        renderSegment (left, right, numSamples);
        return;
    }

    // A host/offline renderer may request more than the fixed source FIFO can
    // hold. Consume each bounded segment before producing the next; otherwise
    // the native producer waits forever for an impossible minimumFrames.
    // Leave room for interpolation lookahead and fractional source position.
    const double sourceStep = sourceSampleRate / hostSampleRate;
    const int segmentLimit = static_cast<int> (std::clamp (
        std::floor (double (sourceFifoFrames - 8) / sourceStep), 1.0, double (numSamples)));
    for (int offset = 0; offset < numSamples;)
    {
        const int count = std::min (segmentLimit, numSamples - offset);
        renderSegment (left + offset, right != nullptr ? right + offset : nullptr, count);
        offset += count;
    }
}

void NukedSC55Emulator::renderSegment (float* left, float* right, int numSamples)
{
    ++renderCallCount;

    if (left == nullptr || numSamples <= 0)
        return;

    if (! ready.load (std::memory_order_acquire)
        || hostSampleRate <= 0.0 || sourceSampleRate <= 0.0)
    {
        std::memset (left, 0, static_cast<size_t> (numSamples) * sizeof (float));
        if (right != nullptr)
            std::memset (right, 0, static_cast<size_t> (numSamples) * sizeof (float));
        return;
    }

    const double sourceStep = sourceSampleRate / hostSampleRate;
    const double lastSourcePosition = sourcePosition
                                    + sourceStep * static_cast<double> (numSamples - 1);
    const auto minimumSourceFrames = static_cast<uint32_t> (std::floor (lastSourcePosition)) + 4u;

    // The audio callback is the owner of the emulator clock. This call may
    // execute many H8 instructions, but it never advances beyond the source
    // frames required by this render segment (apart from interpolation lookahead).
    driveCoreUntilSourceFrames (minimumSourceFrames);

    if (! ready.load (std::memory_order_acquire))
    {
        std::memset (left, 0, static_cast<size_t> (numSamples) * sizeof (float));
        if (right != nullptr)
            std::memset (right, 0, static_cast<size_t> (numSamples) * sizeof (float));
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const uint32_t base = static_cast<uint32_t> (sourcePosition);
        float leftSample = lastSourceFrame[0];
        float rightSample = lastSourceFrame[1];

        if (availableSourceFrames() >= base + 4)
        {
            // Catmull-Rom across four frames rather than a straight line
            // between two. The source is 32 kHz now that the chip's
            // oversampling is gone, so a linear interpolator would fold
            // everything near the top of the band back down.
            const float* p0 = sourceFrame (base);
            const float* p1 = sourceFrame (base + 1);
            const float* p2 = sourceFrame (base + 2);
            const float* p3 = sourceFrame (base + 3);
            const float t = static_cast<float> (sourcePosition - base);

            const auto interpolate = [t] (float y0, float y1, float y2, float y3)
            {
                const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
                const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                const float c = -0.5f * y0 + 0.5f * y2;
                return ((a * t + b) * t + c) * t + y1;
            };

            leftSample = interpolate (p0[0], p1[0], p2[0], p3[0]);
            rightSample = interpolate (p0[1], p1[1], p2[1], p3[1]);
            lastSourceFrame[0] = leftSample;
            lastSourceFrame[1] = rightSample;
        }
        else
        {
            sourceUnderruns.fetch_add (1, std::memory_order_relaxed);
        }

        left[i] = blockDc (0, leftSample);
        if (right != nullptr)
            right[i] = blockDc (1, rightSample);

        sourcePosition += sourceStep;
        const uint32_t consumed = static_cast<uint32_t> (sourcePosition);
        if (consumed != 0)
        {
            consumeSourceFrames (consumed);
            sourcePosition -= consumed;
        }
    }

    if (sc55debug::enabled()
        && (renderCallCount <= 10
            || renderCallCount % 1000 == 0
            || midiPacketCount.load (std::memory_order_relaxed) != lastLoggedMidiPacketCount))
    {
        float outputPeak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            outputPeak = std::max (outputPeak, std::abs (left[i]));
            if (right != nullptr)
                outputPeak = std::max (outputPeak, std::abs (right[i]));
        }

        sc55debug::log (
            "render #%llu samples=%d peak=%.7f source=%u underruns=%llu produced=%llu nonZero=%llu "
            "midi=%llu pc=%02x:%04x cycles=%llu sleep=%d uart=%u/%u SCR=%02x SSR=%02x "
            "voices=%08x/%08x pcmCfg=%02x/%02x",
            static_cast<unsigned long long> (renderCallCount), numSamples, outputPeak,
            availableSourceFrames(),
            static_cast<unsigned long long> (sourceUnderruns.load (std::memory_order_relaxed)),
            static_cast<unsigned long long> (sourceSamplesProduced.load (std::memory_order_relaxed)),
            static_cast<unsigned long long> (sourceNonZeroSamples.load (std::memory_order_relaxed)),
            static_cast<unsigned long long> (midiPacketCount.load (std::memory_order_relaxed)),
            debugCp.load (std::memory_order_relaxed), debugPc.load (std::memory_order_relaxed),
            static_cast<unsigned long long> (debugCycles.load (std::memory_order_relaxed)),
            debugSleep.load (std::memory_order_relaxed),
            debugUartWrite.load (std::memory_order_relaxed), debugUartRead.load (std::memory_order_relaxed),
            debugScr.load (std::memory_order_relaxed), debugSsr.load (std::memory_order_relaxed),
            debugVoiceMask.load (std::memory_order_relaxed),
            debugVoiceMaskPending.load (std::memory_order_relaxed),
            debugPcmConfig3c.load (std::memory_order_relaxed),
            debugPcmConfig3d.load (std::memory_order_relaxed));
        lastLoggedMidiPacketCount = midiPacketCount.load (std::memory_order_relaxed);
    }
}
