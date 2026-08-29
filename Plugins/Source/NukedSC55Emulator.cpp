#include "NukedSC55Emulator.h"

#include "SC55Lcd.h"
#include "SC55Debug.h"

#include <algorithm>
#include <array>
#include <cmath>
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
    }

    return 0;
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

    bool copyMergedMask (const LcdCaptureBackend& alternate,
                         uint8_t* destination, size_t destinationStride) const
    {
        if (destination == nullptr || destinationStride < static_cast<size_t> (LCD_DISPLAY_WIDTH))
            return false;

        Snapshot primary;
        Snapshot secondary;
        copySnapshot (primary);
        alternate.copySnapshot (secondary);
        clearMask (destination, destinationStride);

        const bool primaryRenderable = isRenderable (primary);
        const bool secondaryRenderable = isRenderable (secondary);
        if (! primaryRenderable && ! secondaryRenderable)
            return false;

        // The selected-part fields are global UI state. They are mirrored to
        // both instances by the processor, so keep one coherent copy instead
        // of drawing two different values over the same LCD pixels.
        const auto& common = primaryRenderable ? primary : secondary;

        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                11, 34 + i * 35, common.data[static_cast<size_t> (i)], common);

        // The 16 channel indicators use the same MIDI channel index as the
        // routing rule: even channels come from the primary instance and odd
        // channels from the alternate instance.
        for (int i = 0; i < 16; ++i)
        {
            const Snapshot* source = (i & 1) == 0
                                   ? (primaryRenderable ? &primary : &common)
                                   : (secondaryRenderable ? &secondary : &common);
            renderStandardMask (destination, destinationStride,
                                11, 153 + i * 35,
                                source->data[static_cast<size_t> (3 + i)], *source);
        }

        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                75, 34 + i * 35, common.data[static_cast<size_t> (40 + i)], common);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                75, 153 + i * 35, common.data[static_cast<size_t> (43 + i)], common);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                139, 34 + i * 35, common.data[static_cast<size_t> (49 + i)], common);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                139, 153 + i * 35, common.data[static_cast<size_t> (46 + i)], common);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                203, 34 + i * 35, common.data[static_cast<size_t> (52 + i)], common);
        for (int i = 0; i < 3; ++i)
            renderStandardMask (destination, destinationStride,
                                203, 153 + i * 35, common.data[static_cast<size_t> (55 + i)], common);

        renderMergedLevelIndicators (destination, destinationStride,
                                     primary, secondary,
                                     primaryRenderable, secondaryRenderable);
        renderLeftRightIndicator (destination, destinationStride, common);
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

    void copySnapshot (Snapshot& destination) const
    {
        const std::lock_guard lock (mutex);
        destination = snapshot;
    }

    static bool isRenderable (const Snapshot& state) noexcept
    {
        return state.valid
            && state.width == static_cast<size_t> (LCD_DISPLAY_WIDTH)
            && state.height == static_cast<size_t> (LCD_DISPLAY_HEIGHT)
            && state.enabled
            && ! state.isCm300 && ! state.isSt && ! state.isScb55 && ! state.isJv880;
    }

    static void clearMask (uint8_t* destination, size_t destinationStride)
    {
        for (int y = 0; y < LCD_DISPLAY_HEIGHT; ++y)
            std::memset (destination + static_cast<size_t> (y) * destinationStride,
                         0, static_cast<size_t> (LCD_DISPLAY_WIDTH));
    }

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

    static void renderLevelColumn (uint8_t* destination, size_t stride,
                                   int screenY, int screenX, int column,
                                   int matrix, const Snapshot& state)
    {
        const auto dataOffset = static_cast<size_t> (matrix == 0 ? 20 : 60);
        const auto character = state.data[dataOffset + static_cast<size_t> (column / 5)];
        const auto* f = glyph (state, character);
        const auto glyphColumn = column % 5;

        for (int row = 0; row < 8; ++row)
        {
            const uint8_t value = (f[row] & (1 << (4 - glyphColumn))) != 0 ? 1 : 2;
            setMaskRectangle (destination, stride,
                              screenY + row * 11,
                              screenX + column * 26,
                              9, 24, value);
        }
    }

    static void renderMergedLevelIndicators (uint8_t* destination, size_t stride,
                                             const Snapshot& primary,
                                             const Snapshot& secondary,
                                             bool primaryRenderable,
                                             bool secondaryRenderable)
    {
        const auto& fallback = primaryRenderable ? primary : secondary;
        for (int matrix = 0; matrix < 2; ++matrix)
        {
            for (int column = 0; column < 16; ++column)
            {
                const Snapshot* source = (column & 1) == 0
                                       ? (primaryRenderable ? &primary : &fallback)
                                       : (secondaryRenderable ? &secondary : &fallback);
                renderLevelColumn (destination, stride,
                                   71 + matrix * 88, 293, column, matrix, *source);
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

bool NukedSC55Emulator::hasRomSet (const std::string& romDirectory)
{
    installBackendDiagnostics();

    std::error_code filesystemError;
    if (! std::filesystem::is_directory (std::filesystem::path (romDirectory), filesystemError))
        return false;

    common::LoadRomsetResult result {};
    common::RomOverrides overrides {};
    const auto loadError = common::LoadRomset (std::filesystem::path (romDirectory), {},
                                               common::RomLoader::Hashing, overrides, result);
    if (loadError != common::LoadRomsetError {})
        return false;

    return true;
}

bool NukedSC55Emulator::initialise (const std::string& romDirectory, double newHostSampleRate)
{
    sc55debug::log ("initialise requested directory=\"%s\" hostRate=%.2f",
                    romDirectory.c_str(), newHostSampleRate);

    release();
    error.clear();
    hostSampleRate = newHostSampleRate;

    if (hostSampleRate <= 0.0)
    {
        setError ("Invalid host sample rate");
        return false;
    }

    auto nextRoms = std::make_unique<common::LoadRomsetResult>();
    common::RomOverrides overrides {};
    const auto loadError = common::LoadRomset (std::filesystem::path (romDirectory), {},
                                               common::RomLoader::Hashing, overrides, *nextRoms);
    if (loadError != common::LoadRomsetError {})
    {
        setError (std::string ("ROM loading failed: ") + common::ToCString (loadError));
        sc55debug::log ("ROM initialisation failed: %s", error.c_str());
        return false;
    }

    auto nextCore = std::make_unique<Emulator>();
    EMU_Options options;
    options.lcd_backend = lcdBackend.get();

    if (! nextCore->Init (options))
    {
        setError ("Failed to initialise the Nuked-SC55 backend");
        return false;
    }

    if (! nextCore->LoadRoms (nextRoms->romset, nextRoms->romset_info))
    {
        setError ("Failed to load the selected SC-55 ROM set");
        return false;
    }

    nextCore->Reset();
    nextCore->GetMCU().button_pressed.store (0, std::memory_order_relaxed);
    nextCore->SetSampleCallback (&NukedSC55Emulator::sampleSink, this);
    if (! nextCore->StartLCD())
    {
        nextCore->StopLCD();
        setError ("Failed to initialise the SC-55 LCD backend");
        return false;
    }

    sourceSampleRate = static_cast<double> (PCM_GetOutputFrequency (nextCore->GetPCM()));
    {
        const std::lock_guard lock (coreMutex);
        core = std::move (nextCore);
        // RomsetInfo must outlive Emulator::LoadRoms(). Keep it beside the
        // core until release() destroys the core first.
        loadedRoms = std::move (nextRoms);
    }

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

    publishDebugState();
    ready.store (true, std::memory_order_release);

    const auto& mcu = core->GetMCU();
    sc55debug::log ("initialise succeeded romset=%s mk1=%d sourceRate=%.2f",
                    loadedRoms != nullptr ? RomsetName (loadedRoms->romset) : "unknown",
                    mcu.is_mk1 ? 1 : 0, sourceSampleRate);
    return true;
}

void NukedSC55Emulator::release()
{
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

    {
        const std::lock_guard lock (coreMutex);
        if (core != nullptr)
            core->StopLCD();
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
}

void NukedSC55Emulator::clearPendingMidi() noexcept
{
    const auto write = midiWrite.load (std::memory_order_acquire);
    midiRead.store (write, std::memory_order_release);
}

void NukedSC55Emulator::clearFrontPanelButtons() noexcept
{
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
    if (core == nullptr)
        return;

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
    DebugState state;
    state.ready = ready.load (std::memory_order_acquire);
    state.backendRunning = state.ready;
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
    if (core == nullptr)
        return;

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

    // Capture only the LCD character state on the same thread as the emulator.
    // Calling the backend's LCD_Render here would also redraw its complete
    // pixel framebuffer for every audio block.
    if (lcdBackend != nullptr)
        lcdBackend->captureState();
    publishDebugState();
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

    return lcdBackend->copyMask (destination, destinationStride);
}

bool NukedSC55Emulator::copyMergedLcdDisplay (const NukedSC55Emulator& alternate,
                                              uint8_t* destination, size_t destinationStride) const
{
    if (lcdBackend == nullptr)
        return alternate.copyLcdDisplay (destination, destinationStride);

    if (alternate.lcdBackend == nullptr)
        return copyLcdDisplay (destination, destinationStride);

    return lcdBackend->copyMergedMask (*alternate.lcdBackend,
                                       destination, destinationStride);
}

void NukedSC55Emulator::render (float* left, float* right, int numSamples)
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
