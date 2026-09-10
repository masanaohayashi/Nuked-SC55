#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include "NativeSynthStateExchange.h"
#include "NativeMidiInputState.h"
#include "sc55_synth_command.h"

template <typename SampleType>
struct AudioFrame;

struct Emulator;
class LcdCaptureBackend;
namespace sc55 { class NativeSynth; }

namespace common
{
struct LoadRomsetResult;
}

class NukedSC55Emulator final
{
public:
    enum class RomFamily : uint8_t
    {
        unknown,
        sc55,
        sc55mk2,
        sc155,
        other
    };

    struct DebugState
    {
        bool nativeEngine = false;
        bool ready = false;
        bool backendRunning = false;
        RomFamily romFamily = RomFamily::unknown;
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
        bool soloEnabled = false;
        bool standby = false, fastDisplayScroll = false;
        bool receiveExclusive = true, receiveReset = true, ignoreChecksum = false;
        bool receiveProgramChanges = true;
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
        mute,
        solo, // Normal-screen ALL + MUTE chord.
        standbyOn, standbyOff, fastScrollOn, fastScrollOff, // Native semantic options.
        exclusiveOn, exclusiveOff, resetReceiveOn, resetReceiveOff, checksumIgnoreOn, checksumIgnoreOff,
        programReceiveOn, programReceiveOff
    };

    NukedSC55Emulator();
    ~NukedSC55Emulator();

    enum class EngineMode { environment, native, h8 };
    static bool usesNativeEngine (EngineMode mode) noexcept;
    bool initialise (const std::string& romDirectory, double hostSampleRate,
                     const std::string& nativeCacheDirectory,
                     EngineMode mode = EngineMode::environment);
    void release();
    void clearPendingMidi() noexcept;

    /** Returns true when the directory contains a supported SC-55 ROM set. */
    static bool hasRomSet (const std::string& romDirectory, bool* supportsNative = nullptr);

    /** Writes detailed ROM detection diagnostics in Debug builds. */
    static void logRomSetDiagnostics (const std::string& romDirectory);

    void sendMidi (const uint8_t* data, int size);
    void pressFrontPanelButton (FrontPanelButton button, NukedSC55Emulator* mirror = nullptr);
    void render (float* left, float* right, int numSamples);

    // Message-thread only. Returns the latest complete native sound state;
    // requests a refresh on the next audio render. No access to the live synth.
    bool getNativeState (sc55::SynthState& destination) const noexcept;

    /** Copies the current SC-55 LCD segment mask into a row-major buffer. */
    bool copyLcdDisplay (uint8_t* destination, size_t destinationStride) const;

    /** Copies a display with channel-specific LCD content merged from another instance. */
    bool copyMergedLcdDisplay (const NukedSC55Emulator& alternate,
                               uint8_t* destination, size_t destinationStride) const;

    bool isReady() const noexcept { return ready.load (std::memory_order_acquire); }

    /** Source-rate frames currently staged for host-rate conversion. */
    uint32_t availableFrames() const noexcept { return availableSourceFrames(); }
    DebugState getDebugState() const noexcept;
    uint32_t savedMidiInputState() const noexcept {return midiInputState.encoded();}
    void restoreMidiInputState(int64_t value) noexcept {midiInputState.restore(value);}
    const std::string& getError() const noexcept { return error; }

private:
    enum
    {
        sourceFifoFrames = 65536,
        midiFifoBytes = 8192,
        // A few MIDI messages' worth of emulated UART backlog. Beyond this the
        // firmware is not consuming, so queueing more only delays notes.
        uartRingHeadroom = 256
    };

    static void sampleSink (void* userData, const AudioFrame<int32_t>& sample);

    float blockDc (int channel, float input) noexcept;
    void setError (const std::string& message);

    void driveCoreUntilSourceFrames (uint32_t minimumFrames) noexcept;
    void renderSegment (float* left, float* right, int numSamples);
    void drainMidi();
    void drainNativePanel() noexcept;
    void updateFrontPanelButtons() noexcept;
    void clearFrontPanelButtons() noexcept;
    void publishDebugState() noexcept;
    void pushSample (const AudioFrame<int32_t>& sample);
    bool enqueueMidiByte (uint8_t byte) noexcept;
    uint32_t availableSourceFrames() const noexcept;
    const float* sourceFrame (uint32_t offset) const noexcept;
    void consumeSourceFrames (uint32_t count) noexcept;

    float sourceFifo[sourceFifoFrames][2] {};
    std::atomic<uint32_t> sourceRead { 0 };
    std::atomic<uint32_t> sourceWrite { 0 };

    bool midiDropMessage = false;
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

    // デバッグ UI が状態を見に来たときだけ公開する。音声コールバックは信号処理と
    // MIDI だけを扱い、表示のためのデータ作成はしない。
    mutable std::atomic<bool> debugStateRequested { false };
    bool gsResetSent = false;

    // Front-panel presses are momentary events from the message thread. Keep
    // the hand-off lock-free; the audio thread turns them into a 50 ms pulse
    // measured in source frames, so a stalled host clock cannot release a
    // button while the emulated machine is stopped.
    std::atomic<uint32_t> frontPanelPendingMask { 0 };
    std::atomic<uint32_t> frontPanelPressedMask { 0 };
    std::atomic<uint64_t> frontPanelReleaseFrame { 0 };

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
    std::atomic<uint8_t> debugRomFamily { static_cast<uint8_t> (RomFamily::unknown) };
    std::atomic<bool> debugAllLed { false };
    std::atomic<bool> debugMuteLed { false };
    std::atomic<bool> debugSoloEnabled { false };
    std::atomic<bool> debugStandby { false }, debugFastDisplayScroll { false };
    NativeMidiInputState midiInputState;

    // The jcmoyer backend is per-instance. The mutex only protects the object
    // lifetime while the message-thread LCD snapshot is taken.
    std::mutex coreMutex;
    std::unique_ptr<LcdCaptureBackend> lcdBackend;
    std::unique_ptr<Emulator> core;
    std::unique_ptr<sc55::NativeSynth> nativePlayer;
    mutable NativeSynthStateExchange nativeStateExchange;
    mutable std::atomic<bool> nativeStateRequested { false };
    std::atomic<bool> nativeEngineActive { false };
    static constexpr unsigned nativePanelCapacity = 64;
    std::array<sc55::SynthCommand,nativePanelCapacity> nativePanelQueue {};
    std::atomic<unsigned> nativePanelRead { 0 }, nativePanelWrite { 0 };
    // Message-thread-only interaction state. Audio never reads these members.
    // Lifecycle reset is handed off by generation, not a concurrent UI write.
    std::atomic<unsigned> nativePanelGeneration { 0 };
    unsigned panelGeneration=0;
    uint8_t panelPart=0;
    bool panelAll=false, panelSolo=false, panelStandby=false;
    std::unique_ptr<common::LoadRomsetResult> loadedRoms;

    std::string error;
};
