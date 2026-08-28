#include "NukedSC55Emulator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>

#if defined (__APPLE__)
 #include <pthread.h>
#endif

#include "SC55Debug.h"
#include "mcu.h"
#include "mcu_timer.h"
#include "pcm.h"
#include "submcu.h"
#include "utils/files.h"

namespace
{
const size_t rom1Size = 0x8000;
const size_t rom2FullSize = 0x80000;
const size_t rom2HalfSize = 0x40000;
const size_t mk2WaveRom1Size = 0x200000;
const size_t mk2WaveRom2Size = 0x100000;
const size_t mk1WaveRomSize = 0x100000;
const size_t subMcuRomSize = 0x1000;

// mcu, pcm, the ROM arrays and the sample sink are all globals, so a second
// running instance would step the same CPU state from a second thread and emit
// noise.  One claim per process; losing it means staying silent, not corrupting.
std::atomic<bool> emulatorInUse { false };

// Upstream's -gs option: put the module into a known state at start-up instead
// of whatever the firmware powers up with.
const uint8_t gsReset[] = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
const uint64_t gsResetFallbackCycles = 24000000;

const double dcBlockerHz = 5.0;
const double pi = 3.14159265358979323846;

struct RomSetDescription
{
    int romSet;
    bool mcuMk1;
    const char* rom1Name;
    const char* rom2Name;
    const char* waveRom1Name;
    const char* waveRom2Name;
    const char* waveRom3Name;
    const char* subMcuRomName;
    size_t waveRom1Size;
    size_t waveRom2Size;
    size_t waveRom3Size;
};

// The original SC-55 and the SC-55mkII use different ROM layouts.  In
// particular, the original SC-55 has three 1 MiB wave ROMs and no sub-MCU ROM.
const RomSetDescription romSetDescriptions[] =
{
    {
        ROM_SET_MK1,
        true,
        "sc55_rom1.bin",
        "sc55_rom2.bin",
        "sc55_waverom1.bin",
        "sc55_waverom2.bin",
        "sc55_waverom3.bin",
        nullptr,
        mk1WaveRomSize,
        mk1WaveRomSize,
        mk1WaveRomSize
    },
    {
        ROM_SET_MK2,
        false,
        "rom1.bin",
        "rom2.bin",
        "waverom1.bin",
        "waverom2.bin",
        nullptr,
        "rom_sm.bin",
        mk2WaveRom1Size,
        mk2WaveRom2Size,
        0
    }
};

std::string joinPath (const std::string& directory, const char* name)
{
    if (directory.empty())
        return name;

    const char last = directory[directory.size() - 1];
    return directory + (last == '/' ? "" : "/") + name;
}

bool fileExists (const std::string& path)
{
    FILE* file = Files::utf8_fopen (path.c_str(), "rb");
    if (file == nullptr)
        return false;

    std::fclose (file);
    return true;
}

bool containsRomSet (const std::string& directory, const RomSetDescription& description)
{
    return fileExists (joinPath (directory, description.rom1Name))
        && fileExists (joinPath (directory, description.rom2Name))
        && fileExists (joinPath (directory, description.waveRom1Name))
        && fileExists (joinPath (directory, description.waveRom2Name))
        && (description.waveRom3Name == nullptr
            || fileExists (joinPath (directory, description.waveRom3Name)))
        && (description.subMcuRomName == nullptr
            || fileExists (joinPath (directory, description.subMcuRomName)));
}

const RomSetDescription* findRomSet (const std::string& directory)
{
    for (const auto& description : romSetDescriptions)
    {
        if (containsRomSet (directory, description))
            return &description;
    }

    return nullptr;
}
}

NukedSC55Emulator::NukedSC55Emulator()
    : sourceRead (0),
      sourceWrite (0),
      renderCallCount (0),
      midiPacketCount (0),
      sourceSamplesProduced (0),
      sourceNonZeroSamples (0),
      sourceDroppedSamples (0),
      midiDroppedBytes (0),
      lastLoggedMidiPacketCount (0),
      hostSampleRate (0.0),
      sourceSampleRate (0.0),
      sourcePosition (0.0),
      ready (false)
{
    std::memset (sourceFifo, 0, sizeof (sourceFifo));
}

NukedSC55Emulator::~NukedSC55Emulator()
{
    release();
}

void NukedSC55Emulator::setError (const std::string& message)
{
    error = message;
}

bool NukedSC55Emulator::readFile (const std::string& path, uint8_t* destination, size_t size)
{
    FILE* file = Files::utf8_fopen (path.c_str(), "rb");
    if (file == nullptr)
    {
        setError ("Cannot open ROM file: " + path);
        return false;
    }

    const size_t bytesRead = std::fread (destination, 1, size, file);
    std::fclose (file);

    if (bytesRead != size)
    {
        setError ("ROM file has an unexpected size: " + path);
        return false;
    }

    return true;
}

bool NukedSC55Emulator::readAndUnscramble (const std::string& path, uint8_t* destination, int size)
{
    if (! readFile (path, tempbuf, static_cast<size_t> (size)))
        return false;

    unscramble (tempbuf, destination, size);
    return true;
}

bool NukedSC55Emulator::hasRomSet (const std::string& romDirectory)
{
    return findRomSet (romDirectory) != nullptr;
}

bool NukedSC55Emulator::loadRomSet (const std::string& romDirectory)
{
    const auto* description = findRomSet (romDirectory);
    if (description == nullptr)
    {
        setError ("The directory does not contain a supported SC-55 ROM set");
        return false;
    }

    romset = description->romSet;
    mcu_mk1 = description->mcuMk1 ? 1 : 0;
    mcu_cm300 = 0;
    mcu_st = 0;
    mcu_jv880 = 0;
    mcu_scb55 = 0;
    mcu_sc155 = 0;

    std::memset (rom1, 0, sizeof (rom1));
    std::memset (rom2, 0, sizeof (rom2));
    std::memset (waverom1, 0, mk2WaveRom1Size);
    std::memset (waverom2, 0, mk2WaveRom1Size);
    std::memset (waverom3, 0, mk2WaveRom2Size);
    std::memset (sm_rom, 0, sizeof (sm_rom));

    if (! readFile (joinPath (romDirectory, description->rom1Name), rom1, rom1Size))
        return false;

    const std::string rom2Path = joinPath (romDirectory, description->rom2Name);
    FILE* rom2File = Files::utf8_fopen (rom2Path.c_str(), "rb");
    if (rom2File == nullptr)
    {
        setError ("Cannot open ROM file: " + rom2Path);
        return false;
    }

    if (std::fseek (rom2File, 0, SEEK_END) != 0)
    {
        std::fclose (rom2File);
        setError ("Cannot inspect ROM file: " + rom2Path);
        return false;
    }

    const long rom2Length = std::ftell (rom2File);
    std::rewind (rom2File);

    if (rom2Length != static_cast<long> (rom2HalfSize)
        && rom2Length != static_cast<long> (rom2FullSize))
    {
        std::fclose (rom2File);
        setError ("ROM file has an unexpected size: " + rom2Path);
        return false;
    }

    const size_t rom2Size = static_cast<size_t> (rom2Length);
    const size_t rom2BytesRead = std::fread (rom2, 1, rom2Size, rom2File);
    std::fclose (rom2File);
    if (rom2BytesRead != rom2Size)
    {
        setError ("Failed to read ROM file: " + rom2Path);
        return false;
    }
    rom2_mask = static_cast<int> (rom2Size - 1);

    if (! readAndUnscramble (joinPath (romDirectory, description->waveRom1Name), waverom1,
                             static_cast<int> (description->waveRom1Size)))
        return false;

    if (! readAndUnscramble (joinPath (romDirectory, description->waveRom2Name), waverom2,
                             static_cast<int> (description->waveRom2Size)))
        return false;

    if (description->waveRom3Name != nullptr
        && ! readAndUnscramble (joinPath (romDirectory, description->waveRom3Name), waverom3,
                                static_cast<int> (description->waveRom3Size)))
        return false;

    if (description->subMcuRomName != nullptr
        && ! readFile (joinPath (romDirectory, description->subMcuRomName), sm_rom,
                       subMcuRomSize))
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

    bool expected = false;
    if (! emulatorInUse.compare_exchange_strong (expected, true))
    {
        setError ("Another SC-55 instance is already running in this process");
        sc55debug::log ("initialise refused: %s", error.c_str());
        return false;
    }
    ownsEmulator = true;

    if (! loadRomSet (romDirectory))
    {
        sc55debug::log ("ROM initialisation failed: %s", error.c_str());
        releaseClaim();
        return false;
    }

    std::memset (dev_register, 0, 0x80);
    std::memset (uart_buffer, 0, uart_buffer_size);
    uart_write_ptr = 0;
    uart_read_ptr = 0;
    mcu_button_pressed.store (0, std::memory_order_relaxed);

    MCU_Init();
    TIMER_Reset();
    MCU_PatchROM();
    MCU_Reset();
    SM_Reset();
    PCM_Reset();

    sourceRead.store (0, std::memory_order_relaxed);
    sourceWrite.store (0, std::memory_order_relaxed);
    renderCallCount = 0;
    midiPacketCount.store (0, std::memory_order_relaxed);
    sourceSamplesProduced.store (0, std::memory_order_relaxed);
    sourceNonZeroSamples.store (0, std::memory_order_relaxed);
    sourceDroppedSamples.store (0, std::memory_order_relaxed);
    midiDroppedBytes.store (0, std::memory_order_relaxed);
    lastLoggedMidiPacketCount = 0;
    sourcePosition = 0.0;
    lastSourceFrame[0] = lastSourceFrame[1] = 0.0f;
    sourceUnderruns.store (0, std::memory_order_relaxed);
    dcCoefficient = 1.0 - (2.0 * pi * dcBlockerHz / hostSampleRate);
    dcPreviousInput[0] = dcPreviousInput[1] = 0.0f;
    dcPreviousOutput[0] = dcPreviousOutput[1] = 0.0f;
    sourceSampleRate = (mcu_mk1 || mcu_jv880) ? 64000.0 : 66207.0;
    MCU_SetSampleSink (&NukedSC55Emulator::sampleSink, this);

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
        MCU_SetSampleSink (nullptr, nullptr);
        setError (std::string ("Cannot start emulation thread: ") + exception.what());
        releaseClaim();
        return false;
    }

    // Let the firmware get through its reset path before the first host audio
    // callback consumes the FIFO.  This wait is off the audio thread.
    {
        std::unique_lock lock (emulationMutex);
        emulationCondition.wait_for (lock, std::chrono::milliseconds (250), [this]
        {
            return ! emulationThreadRunning.load (std::memory_order_acquire)
                || availableSourceFrames() >= sourceTargetFrames;
        });
    }

    sc55debug::log ("initialise succeeded romset=%d mk1=%d sourceRate=%.2f",
                    romset, mcu_mk1, sourceSampleRate);
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

    // Only the instance that owns the core may touch its globals; clearing the
    // sink unconditionally would silence whichever instance is actually running.
    if (ownsEmulator)
        MCU_SetSampleSink (nullptr, nullptr);

    sourceRead.store (0, std::memory_order_relaxed);
    sourceWrite.store (0, std::memory_order_relaxed);
    renderCallCount = 0;
    midiPacketCount.store (0, std::memory_order_relaxed);
    sourceSamplesProduced.store (0, std::memory_order_relaxed);
    sourceNonZeroSamples.store (0, std::memory_order_relaxed);
    sourceDroppedSamples.store (0, std::memory_order_relaxed);
    midiDroppedBytes.store (0, std::memory_order_relaxed);
    lastLoggedMidiPacketCount = 0;
    sourcePosition = 0.0;
    midiDropMessage = false;
    midiGateOpen = false;
    lastUartReadPtr = 0;
    releaseClaim();
}

void NukedSC55Emulator::releaseClaim() noexcept
{
    if (ownsEmulator)
    {
        emulatorInUse.store (false, std::memory_order_release);
        ownsEmulator = false;
    }
}

void NukedSC55Emulator::clearPendingMidi() noexcept
{
    const auto write = midiWrite.load (std::memory_order_acquire);
    midiRead.store (write, std::memory_order_release);
}

namespace
{
// Opt-in raw capture of what the host actually hands over, message by message.
// Enabled only while ~/.sc55_midi_trace exists, so it costs nothing normally.
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

    // The emulation thread owns the H8/UART state.  Keeping this queue alive
    // while the ROM is being selected also matches the original program,
    // whose RtMidi callback writes to the UART ring before the CPU worker is
    // started.
}

float NukedSC55Emulator::blockDc (int channel, float input) noexcept
{
    const float output = input - dcPreviousInput[channel]
                       + static_cast<float> (dcCoefficient) * dcPreviousOutput[channel];
    dcPreviousInput[channel] = input;
    dcPreviousOutput[channel] = output;
    return output;
}

void NukedSC55Emulator::sampleSink (const int* sample, void* userData)
{
    if (userData != nullptr)
        static_cast<NukedSC55Emulator*> (userData)->pushSample (sample);
}

void NukedSC55Emulator::pushSample (const int* sample)
{
    if (sample == nullptr)
        return;

    sourceSamplesProduced.fetch_add (1, std::memory_order_relaxed);
    if (sample[0] != 0 || sample[1] != 0)
        sourceNonZeroSamples.fetch_add (1, std::memory_order_relaxed);

    const auto write = sourceWrite.load (std::memory_order_relaxed);
    const auto nextWrite = (write + 1) % sourceFifoFrames;
    if (nextWrite == sourceRead.load (std::memory_order_acquire))
    {
        sourceDroppedSamples.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    sourceFifo[write][0] = static_cast<float> (sample[0]) / 32768.0f;
    sourceFifo[write][1] = static_cast<float> (sample[1]) / 32768.0f;
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
    // Bytes have to reach the UART as they arrive.  Holding them back and
    // flushing the backlog later collapses every note-on/note-off pair into the
    // same instant of emulated time, which is silent.  While the firmware is
    // not draining its UART - during the boot sequence, mainly - the bytes are
    // dropped rather than queued, which is what the hardware does when you play
    // during power-on.
    // The firmware reads nothing while it boots, so anything sent then would
    // pile up and arrive as one burst.  The moment it consumes its first byte
    // the gate opens for good: after that nothing may be dropped for being
    // merely late, because losing one CC out of an RPN sequence (101/100/6/38)
    // silently loses the setting.
    if (! midiGateOpen && uart_read_ptr != lastUartReadPtr)
        midiGateOpen = true;

    lastUartReadPtr = uart_read_ptr;

    auto read = midiRead.load (std::memory_order_relaxed);
    const auto write = midiWrite.load (std::memory_order_acquire);
    while (read != write)
    {
        const uint8_t byte = midiFifo[read];

        // Decide once per message, at its status byte, so that a long SysEx is
        // never cut in half.  Data bytes follow the decision made for the
        // message they belong to, which also covers running status.
        // 0xf7 ends the message it belongs to and 0xf8 and above are realtime
        // bytes, so neither of them starts a new one.
        if (byte >= 0x80 && byte < 0xf7)
        {
            const uint32_t backlog = (uart_write_ptr - uart_read_ptr) % uart_buffer_size;
            const bool ringNearlyFull = backlog >= uart_buffer_size - uartRingHeadroom;

            // SysEx survives even the closed gate: a host sends its setup bank
            // while the module is still booting, and that has to arrive.
            midiDropMessage = byte == 0xf0 ? ringNearlyFull
                                           : (! midiGateOpen || ringNearlyFull);
        }

        if (! midiDropMessage)
            MCU_PostUART (byte);
        else
            midiDroppedBytes.fetch_add (1, std::memory_order_relaxed);

        read = (read + 1) % midiFifoBytes;
    }
    midiRead.store (read, std::memory_order_release);
}

void NukedSC55Emulator::publishDebugState() noexcept
{
    debugCp.store (mcu.cp, std::memory_order_relaxed);
    debugPc.store (mcu.pc, std::memory_order_relaxed);
    debugCycles.store (mcu.cycles, std::memory_order_relaxed);
    debugSleep.store (mcu.sleep, std::memory_order_relaxed);
    debugScr.store (dev_register[DEV_SCR], std::memory_order_relaxed);
    debugSsr.store (dev_register[DEV_SSR], std::memory_order_relaxed);
    debugVoiceMask.store (pcm.voice_mask, std::memory_order_relaxed);
    debugVoiceMaskPending.store (pcm.voice_mask_pending, std::memory_order_relaxed);
    debugPcmConfig3c.store (pcm.config_reg_3c, std::memory_order_relaxed);
    debugPcmConfig3d.store (pcm.config_reg_3d, std::memory_order_relaxed);
    debugUartWrite.store (uart_write_ptr, std::memory_order_relaxed);
    debugUartRead.store (uart_read_ptr, std::memory_order_relaxed);
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
    // A default-QoS thread gets throttled once the app has been idle for a
    // while, which starves the FIFO and turns the output into a square wave
    // between silence and the DAC bias.  This thread has to keep real time.
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    // Posted straight to the UART rather than through the MIDI queue, so the
    // boot-time drop guard cannot eat it.
    bool gsResetSent = false;

    while (emulationThreadRunning.load (std::memory_order_acquire))
    {
        if (! gsResetSent
            && (((dev_register[DEV_SCR] & 0x10) != 0 && mcu.sleep != 0)
                || mcu.cycles > gsResetFallbackCycles))
        {
            for (const uint8_t byte : gsReset)
                MCU_PostUART (byte);

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

        MCU_RunOneInstruction();

        if (sc55debug::enabled())
            publishDebugState();
    }
}

void NukedSC55Emulator::render (float* left, float* right, int numSamples)
{
    ++renderCallCount;

    if (left == nullptr || numSamples <= 0)
    {
        sc55debug::log ("render #%llu rejected left=%p samples=%d",
                        static_cast<unsigned long long> (renderCallCount), left, numSamples);
        return;
    }

    if (! ready.load (std::memory_order_acquire)
        || hostSampleRate <= 0.0 || sourceSampleRate <= 0.0)
    {
        sc55debug::log ("render #%llu silent ready=%d hostRate=%.2f sourceRate=%.2f samples=%d",
                        static_cast<unsigned long long> (renderCallCount),
                        ready.load (std::memory_order_acquire) ? 1 : 0,
                        hostSampleRate, sourceSampleRate, numSamples);
        std::memset (left, 0, static_cast<size_t> (numSamples) * sizeof (float));
        if (right != nullptr)
            std::memset (right, 0, static_cast<size_t> (numSamples) * sizeof (float));
        return;
    }

    const double sourceStep = sourceSampleRate / hostSampleRate;
    for (int i = 0; i < numSamples; ++i)
    {
        const uint32_t base = static_cast<uint32_t> (sourcePosition);

        // On underrun hold the last frame.  Dropping to zero would step the
        // output by the DAC bias and the result is an audible square wave.
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
