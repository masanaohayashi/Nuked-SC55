#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdint.h>

#include <string>
#include <thread>

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
    };

    NukedSC55Emulator();
    ~NukedSC55Emulator();

    bool initialise (const std::string& romDirectory, double hostSampleRate);
    void release();
    void clearPendingMidi() noexcept;

    /** Returns true when directory contains a supported SC-55 ROM set. */
    static bool hasRomSet (const std::string& romDirectory);

    void sendMidi (const uint8_t* data, int size);
    void render (float* left, float* right, int numSamples);

    bool isReady() const noexcept { return ready.load (std::memory_order_acquire); }
    DebugState getDebugState() const noexcept;
    const std::string& getError() const noexcept { return error; }

private:
    enum
    {
        sourceFifoFrames = 65536,
        sourceTargetFrames = 1024,
        midiFifoBytes = 8192,
        // A few MIDI messages' worth of emulated UART backlog.  Beyond this the
        // firmware is not consuming, so queueing more only delays notes.
        maxUartBacklog = 64,
        // SysEx ignores that limit, but not the ring itself: past this the ring
        // would wrap and overwrite bytes the firmware has not read yet.
        uartRingHeadroom = 256
    };

    static void sampleSink (const int* sample, void* userData);

    float blockDc (int channel, float input) noexcept;
    void releaseClaim() noexcept;

    bool loadRomSet (const std::string& romDirectory);
    bool readFile (const std::string& path, uint8_t* destination, size_t size);
    bool readAndUnscramble (const std::string& path, uint8_t* destination, int size);
    void setError (const std::string& message);

    void emulationThreadMain();
    void drainMidi();
    void publishDebugState() noexcept;
    void pushSample (const int* sample);
    bool enqueueMidiByte (uint8_t byte) noexcept;
    bool midiPending() const noexcept;
    uint32_t availableSourceFrames() const noexcept;
    const float* sourceFrame (uint32_t offset) const noexcept;
    void consumeSourceFrames (uint32_t count) noexcept;

    float sourceFifo[sourceFifoFrames][2];
    std::atomic<uint32_t> sourceRead { 0 };
    std::atomic<uint32_t> sourceWrite { 0 };

    bool midiDropMessage = false;
    uint8_t midiFifo[midiFifoBytes] {};
    std::atomic<uint32_t> midiRead { 0 };
    std::atomic<uint32_t> midiWrite { 0 };

    uint64_t renderCallCount;
    std::atomic<uint64_t> midiPacketCount { 0 };
    std::atomic<uint64_t> sourceSamplesProduced { 0 };
    std::atomic<uint64_t> sourceNonZeroSamples { 0 };
    std::atomic<uint64_t> sourceDroppedSamples { 0 };
    std::atomic<uint64_t> midiDroppedBytes { 0 };
    uint64_t lastLoggedMidiPacketCount;

    // The PCM chip adds a constant bias to its DAC that real hardware loses in
    // the output coupling capacitor.  This is that capacitor: a 5 Hz one-pole.
    double dcCoefficient = 0.0;
    float lastSourceFrame[2] { 0.0f, 0.0f };
    std::atomic<uint64_t> sourceUnderruns { 0 };

    float dcPreviousInput[2] { 0.0f, 0.0f };
    float dcPreviousOutput[2] { 0.0f, 0.0f };

    double hostSampleRate;
    double sourceSampleRate;
    double sourcePosition;
    std::atomic<bool> ready { false };
    std::atomic<bool> emulationThreadRunning { false };
    std::thread emulationThread;
    std::mutex emulationMutex;
    std::condition_variable emulationCondition;

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

    // The emulator core is a pile of globals, so only one instance per process
    // may run it.  See releaseClaim().
    bool ownsEmulator = false;

    std::string error;
};
