#include "NukedSC55Emulator.h"

#include "SC55Lcd.h"
#include "SC55Debug.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string_view>

#if defined (__APPLE__)
 #include <pthread.h>
#endif

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
}

// The backend's LCD renderer is intentionally callback based. This adapter
// keeps the backend alive so LCD_Write records the hardware state, then takes a
// snapshot on the message thread. The mask is rendered with the same segment
// coordinates as Nuked's original plugin-side LCD_GetDisplayMask(), while the
// MCU/DSP implementation itself remains entirely in jcmoyer's backend.
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
        const lcd_t* current = nullptr;
        {
            const std::lock_guard lock (mutex);
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
            const std::lock_guard lock (mutableLcd.mutex);
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
        }

        const std::lock_guard lock (mutex);
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
    midiGateOpen = false;
    lastUartReadPtr = 0;

    publishDebugState();
    ready.store (true, std::memory_order_release);
    emulationThreadRunning.store (true, std::memory_order_release);

    try
    {
        emulationThread = std::thread ([this] { emulationThreadMain(); });
    }
    catch (const std::exception& exception)
    {
        emulationThreadRunning.store (false, std::memory_order_release);
        ready.store (false, std::memory_order_release);
        release();
        setError (std::string ("Cannot start emulation thread: ") + exception.what());
        return false;
    }

    // This wait happens on the message thread, never in the audio callback.
    // It lets the firmware leave its reset path before the first audio block.
    {
        std::unique_lock lock (emulationMutex);
        emulationCondition.wait_for (lock, std::chrono::milliseconds (250), [this]
        {
            return ! emulationThreadRunning.load (std::memory_order_acquire)
                || availableSourceFrames() >= sourceTargetFrames;
        });
    }

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
    emulationThreadRunning.store (false, std::memory_order_release);
    emulationCondition.notify_all();

    if (emulationThread.joinable())
        emulationThread.join();

    {
        const std::lock_guard lock (coreMutex);
        if (core != nullptr)
            core->StopLCD();
        core.reset();
        loadedRoms.reset();
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
    midiDropMessage = false;
    midiGateOpen = false;
    lastUartReadPtr = 0;
}

void NukedSC55Emulator::clearPendingMidi() noexcept
{
    const auto write = midiWrite.load (std::memory_order_acquire);
    midiRead.store (write, std::memory_order_release);
}

namespace
{
std::FILE* midiTraceFile()
{
    static std::FILE* file = []() -> std::FILE*
    {
        const auto* home = std::getenv ("HOME");
        if (home == nullptr)
            return nullptr;

        char marker[1024];
        std::snprintf (marker, sizeof (marker), "%s/.sc55_midi_trace", home);
        if (std::FILE* probe = std::fopen (marker, "rb"))
            std::fclose (probe);
        else
            return nullptr;

        char path[1024];
        std::snprintf (path, sizeof (path), "%s/Library/Logs/SC-55-midi.log", home);
        return std::fopen (path, "w");
    }();

    return file;
}
}

void NukedSC55Emulator::sendMidi (const uint8_t* data, int size)
{
    if (std::FILE* trace = midiTraceFile(); trace != nullptr && data != nullptr && size > 0)
    {
        for (int i = 0; i < size; ++i)
            std::fprintf (trace, "%02X ", data[i]);
        std::fputc ('\n', trace);
        std::fflush (trace);
    }

    if (data == nullptr || size <= 0)
    {
        sc55debug::log ("MIDI rejected: invalid packet data=%p size=%d", data, size);
        return;
    }

    const auto packetNumber = midiPacketCount.fetch_add (1, std::memory_order_relaxed) + 1;
    const int byte0 = data[0];
    const int byte1 = size > 1 ? data[1] : 0;
    const int byte2 = size > 2 ? data[2] : 0;

    for (int i = 0; i < size; ++i)
    {
        if (! enqueueMidiByte (data[i]))
        {
            midiDroppedBytes.fetch_add (static_cast<uint64_t> (size - i), std::memory_order_relaxed);
            sc55debug::log ("MIDI #%llu input queue full size=%d data=%02x %02x %02x",
                            static_cast<unsigned long long> (packetNumber), size,
                            byte0, byte1, byte2);
            break;
        }
    }
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
    emulationCondition.notify_all();
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

bool NukedSC55Emulator::midiPending() const noexcept
{
    return midiRead.load (std::memory_order_acquire)
        != midiWrite.load (std::memory_order_acquire);
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

void NukedSC55Emulator::drainMidi()
{
    if (core == nullptr)
        return;

    auto& mcu = core->GetMCU();
    if (! midiGateOpen && mcu.uart_read_ptr != lastUartReadPtr)
        midiGateOpen = true;
    lastUartReadPtr = mcu.uart_read_ptr;

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

            // SysEx is allowed through the boot gate; hosts commonly send the
            // initial setup bank before the firmware has consumed a note.
            midiDropMessage = byte == 0xf0 ? ringNearlyFull
                                           : (! midiGateOpen || ringNearlyFull);
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
}

NukedSC55Emulator::DebugState NukedSC55Emulator::getDebugState() const noexcept
{
    DebugState state;
    state.ready = ready.load (std::memory_order_acquire);
    state.emulationThreadRunning = emulationThreadRunning.load (std::memory_order_acquire);
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
    state.sourceFrames = availableSourceFrames();
    state.midiPackets = midiPacketCount.load (std::memory_order_relaxed);
    state.midiDroppedBytes = midiDroppedBytes.load (std::memory_order_relaxed);
    state.sourceSamplesProduced = sourceSamplesProduced.load (std::memory_order_relaxed);
    state.sourceNonZeroSamples = sourceNonZeroSamples.load (std::memory_order_relaxed);
    state.sourceDroppedSamples = sourceDroppedSamples.load (std::memory_order_relaxed);
    state.sourceUnderruns = sourceUnderruns.load (std::memory_order_relaxed);
    return state;
}

void NukedSC55Emulator::emulationThreadMain()
{
#if defined (__APPLE__)
    // Keep the source FIFO fed while the standalone app is idle in the
    // background. This is the same real-time requirement as the old wrapper.
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    bool gsResetSent = false;

    while (emulationThreadRunning.load (std::memory_order_acquire))
    {
        if (core == nullptr)
            break;

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

        drainMidi();

        if (availableSourceFrames() >= sourceTargetFrames)
        {
            std::unique_lock lock (emulationMutex);
            emulationCondition.wait_for (lock, std::chrono::milliseconds (1), [this]
            {
                return ! emulationThreadRunning.load (std::memory_order_acquire)
                    || availableSourceFrames() < sourceTargetFrames
                    || midiPending();
            });
            continue;
        }

        core->Step();
        publishDebugState();
    }
}

bool NukedSC55Emulator::copyLcdDisplay (uint8_t* destination, size_t destinationStride)
{
    if (destination == nullptr || destinationStride < static_cast<size_t> (LCD_DISPLAY_WIDTH))
        return false;

    const std::lock_guard lock (coreMutex);
    if (core == nullptr || lcdBackend == nullptr)
    {
        for (int y = 0; y < LCD_DISPLAY_HEIGHT; ++y)
            std::memset (destination + static_cast<size_t> (y) * destinationStride,
                         0, static_cast<size_t> (LCD_DISPLAY_WIDTH));
        return false;
    }

    LCD_Render (core->GetLCD());
    return lcdBackend->copyMask (destination, destinationStride);
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
    for (int i = 0; i < numSamples; ++i)
    {
        const uint32_t base = static_cast<uint32_t> (sourcePosition);
        float leftSample = lastSourceFrame[0];
        float rightSample = lastSourceFrame[1];

        if (availableSourceFrames() >= base + 2)
        {
            const float* first = sourceFrame (base);
            const float* second = sourceFrame (base + 1);
            const float fraction = static_cast<float> (sourcePosition - base);
            leftSample = first[0] + (second[0] - first[0]) * fraction;
            rightSample = first[1] + (second[1] - first[1]) * fraction;
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
