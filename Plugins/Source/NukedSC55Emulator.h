#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

template <typename SampleType>
struct AudioFrame;

struct Emulator;
class LcdCaptureBackend;

namespace common
{
struct LoadRomsetResult;
}

class NukedSC55Emulator final
{
public:
    struct DebugState
    {
        bool ready = false;
        bool emulationThreadRunning = false;
        uint8_t cp = 0;
        uint16_t pc = 0;
        uint64_t cycles = 0;
        uint8_t sleep = 0;
        uint8_t scr = 0;
        uint8_t ssr = 0;
        uint32_t voiceMask = 0;
        uint32_t voiceMaskPending = 0;
        uint8_t pcmConfig3c = 0;
        uint8_t pcmConfig3d = 0;
        uint32_t uartWrite = 0;
        uint32_t uartRead = 0;
        uint32_t sourceFrames = 0;
        uint64_t midiPackets = 0;
        uint64_t midiDroppedBytes = 0;
        uint64_t sourceSamplesProduced = 0;
        uint64_t sourceNonZeroSamples = 0;
        uint64_t sourceDroppedSamples = 0;
        uint64_t sourceUnderruns = 0;
        bool allLed = false;
        bool muteLed = false;
    };

    enum class FrontPanelButton : uint8_t
    {
        partDec,
        partInc,
        instrumentDec,
        instrumentInc,
        levelDec,
        levelInc,
        panDec,
        panInc,
        reverbDec,
        reverbInc,
        chorusDec,
        chorusInc,
        keyShiftDec,
        keyShiftInc,
        midiChannelDec,
        midiChannelInc,
        all,
        mute
    };

    NukedSC55Emulator();
    ~NukedSC55Emulator();

    bool initialise (const std::string& romDirectory, double hostSampleRate);
    void release();
    void clearPendingMidi() noexcept;

    /** Returns true when the directory contains a supported SC-55 ROM set. */
    static bool hasRomSet (const std::string& romDirectory);

    void sendMidi (const uint8_t* data, int size);
    void pressFrontPanelButton (FrontPanelButton button);
    void render (float* left, float* right, int numSamples);

    /** Copies the current SC-55 LCD segment mask into a row-major buffer. */
    bool copyLcdDisplay (uint8_t* destination, size_t destinationStride);

    bool isReady() const noexcept { return ready.load (std::memory_order_acquire); }

    /** Frames waiting in the FIFO; offline renderers use it to pace themselves. */
    uint32_t availableFrames() const noexcept { return availableSourceFrames(); }
    DebugState getDebugState() const noexcept;
    const std::string& getError() const noexcept { return error; }

private:
    enum
    {
        sourceFifoFrames = 65536,
        sourceTargetFrames = 1024,
        midiFifoBytes = 8192,
        // A few MIDI messages' worth of emulated UART backlog. Beyond this the
        // firmware is not consuming, so queueing more only delays notes.
        uartRingHeadroom = 256
    };

    static void sampleSink (void* userData, const AudioFrame<int32_t>& sample);

    float blockDc (int channel, float input) noexcept;
    void setError (const std::string& message);

    void emulationThreadMain();
    void drainMidi();
    void updateFrontPanelButtons() noexcept;
    void clearFrontPanelButtons() noexcept;
    void publishDebugState() noexcept;
    void pushSample (const AudioFrame<int32_t>& sample);
    bool enqueueMidiByte (uint8_t byte) noexcept;
    bool midiPending() const noexcept;
    uint32_t availableSourceFrames() const noexcept;
    const float* sourceFrame (uint32_t offset) const noexcept;
    void consumeSourceFrames (uint32_t count) noexcept;

    float sourceFifo[sourceFifoFrames][2] {};
    std::atomic<uint32_t> sourceRead { 0 };
    std::atomic<uint32_t> sourceWrite { 0 };

    bool midiDropMessage = false;
    bool midiGateOpen = false;
    uint32_t lastUartReadPtr = 0;
    uint8_t midiFifo[midiFifoBytes] {};
    std::atomic<uint32_t> midiRead { 0 };
    std::atomic<uint32_t> midiWrite { 0 };

    uint64_t renderCallCount = 0;
    std::atomic<uint64_t> midiPacketCount { 0 };
    std::atomic<uint64_t> sourceSamplesProduced { 0 };
    std::atomic<uint64_t> sourceNonZeroSamples { 0 };
    std::atomic<uint64_t> sourceDroppedSamples { 0 };
    std::atomic<uint64_t> midiDroppedBytes { 0 };
    uint64_t lastLoggedMidiPacketCount = 0;

    // The PCM chip adds a constant bias to its DAC that real hardware loses in
    // the output coupling capacitor. This is that capacitor: a 5 Hz one-pole.
    double dcCoefficient = 0.0;
    float lastSourceFrame[2] { 0.0f, 0.0f };
    std::atomic<uint64_t> sourceUnderruns { 0 };

    float dcPreviousInput[2] { 0.0f, 0.0f };
    float dcPreviousOutput[2] { 0.0f, 0.0f };

    double hostSampleRate = 0.0;
    double sourceSampleRate = 0.0;
    double sourcePosition = 0.0;
    std::atomic<bool> ready { false };
    std::atomic<bool> emulationThreadRunning { false };
    std::thread emulationThread;
    std::mutex emulationMutex;
    std::condition_variable emulationCondition;

    struct FrontPanelPulse
    {
        uint32_t mask = 0;
        std::chrono::steady_clock::time_point releaseAt;
    };

    std::mutex frontPanelMutex;
    std::vector<FrontPanelPulse> frontPanelPulses;
    std::atomic<bool> frontPanelPulsesActive { false };
    std::atomic<uint64_t> frontPanelGeneration { 0 };

    std::atomic<uint8_t> debugCp { 0 };
    std::atomic<uint16_t> debugPc { 0 };
    std::atomic<uint64_t> debugCycles { 0 };
    std::atomic<uint8_t> debugSleep { 0 };
    std::atomic<uint8_t> debugScr { 0 };
    std::atomic<uint8_t> debugSsr { 0 };
    std::atomic<uint32_t> debugVoiceMask { 0 };
    std::atomic<uint32_t> debugVoiceMaskPending { 0 };
    std::atomic<uint8_t> debugPcmConfig3c { 0 };
    std::atomic<uint8_t> debugPcmConfig3d { 0 };
    std::atomic<uint32_t> debugUartWrite { 0 };
    std::atomic<uint32_t> debugUartRead { 0 };
    std::atomic<bool> debugAllLed { false };
    std::atomic<bool> debugMuteLed { false };

    // The jcmoyer backend is per-instance. The mutex only protects the object
    // lifetime while the message-thread LCD snapshot is taken.
    std::mutex coreMutex;
    std::unique_ptr<LcdCaptureBackend> lcdBackend;
    std::unique_ptr<Emulator> core;
    std::unique_ptr<common::LoadRomsetResult> loadedRoms;

    std::string error;
};
