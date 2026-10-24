#pragma once
// The plug-in's sound engine: the native SC-55 (v1.21) synth behind lock-free
// MIDI and panel queues, rendered at its own 32 kHz and resampled to the host.
// The message thread reads published snapshots only.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "NativeSynthStateExchange.h"
#include "NativeMidiInputState.h"
#include "sc55_synth_command.h"
#include "sc55_panel_raster.h"

namespace sc55 { class NativeSynth; }
class GS55LcdModel;

class GS55Engine final
{
public:
    struct DebugState
    {
        bool ready = false;
        uint64_t renderedFrames = 0;
        uint32_t keyedVoices = 0;
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

    // What the LCD shows: the text fields, the eight user glyphs (codes 0-7)
    // and the part level meter, either as levels or as a SysEx bitmap held in
    // the glyphs.
    struct LcdFrame
    {
        bool enabled = false;
        std::array<uint8_t, 3> part {}, level {}, pan {}, reverb {}, chorus {}, keyShift {}, midiChannel {};
        std::array<uint8_t, 16> instrument {};
        std::array<std::array<uint8_t, 8>, 8> glyphs {};
        std::array<unsigned, 16> meter {};  // 0..16 lit cells per part
        bool meterFromGlyphs = false;
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
        standbyOn, standbyOff, fastScrollOn, fastScrollOff,
        exclusiveOn, exclusiveOff, resetReceiveOn, resetReceiveOff, checksumIgnoreOn, checksumIgnoreOff,
        programReceiveOn, programReceiveOff
    };

    GS55Engine();
    ~GS55Engine();

    bool initialise (const std::string& romDirectory, double hostSampleRate,
                     const std::string& soundDataCacheDirectory, unsigned maximumVoices = 24);
    void release();
    void clearPendingMidi() noexcept;

    /** True when the directory holds the five SC-55 v1.21 ROMs. */
    static bool hasRomSet (const std::string& romDirectory);
    /** Logs which ROMs are missing, in Debug builds. */
    static void logRomSetDiagnostics (const std::string& romDirectory);

    void sendMidi (const uint8_t* data, int size);
    void pressFrontPanelButton (FrontPanelButton button);
    void render (float* left, float* right, int numSamples);

    // Message thread only: the latest complete sound state; asks the audio
    // thread for a fresh one. Never touches the live synth.
    bool getNativeState (sc55::SynthState& destination) const noexcept;
    // Message thread only.
    bool getLcdFrame (LcdFrame& destination) const;
    /** The private SysEx panel image, when one is shown (741x268, two-bit palette). */
    bool copyPanelRaster (uint8_t* destination, size_t stride, bool* contentChanged, bool forceCopy) const;

    bool isReady() const noexcept { return ready.load (std::memory_order_acquire); }
    uint32_t availableFrames() const noexcept { return availableSourceFrames(); }
    DebugState getDebugState() const noexcept;
    uint32_t savedMidiInputState() const noexcept { return midiInputState.encoded(); }
    void restoreMidiInputState (int64_t value) noexcept { midiInputState.restore (value); }
    const std::string& getError() const noexcept { return error; }

private:
    enum
    {
        sourceFifoFrames = 65536,
        midiFifoBytes = 8192
    };

    float blockDc (int channel, float input) noexcept;
    void renderSegment (float* left, float* right, int numSamples);
    void produceSourceFrames (uint32_t minimumFrames) noexcept;
    void drainMidi();
    void drainPanel() noexcept;
    void publishDebugState() noexcept;
    void pushSample (int32_t left, int32_t right);
    bool enqueueMidiByte (uint8_t byte) noexcept;
    uint32_t availableSourceFrames() const noexcept;
    const float* sourceFrame (uint32_t offset) const noexcept;
    void consumeSourceFrames (uint32_t count) noexcept;

    float sourceFifo[sourceFifoFrames][2] {};
    std::atomic<uint32_t> sourceRead { 0 };
    std::atomic<uint32_t> sourceWrite { 0 };

    uint8_t midiFifo[midiFifoBytes] {};
    std::atomic<uint32_t> midiRead { 0 };
    std::atomic<uint32_t> midiWrite { 0 };

    uint64_t renderCallCount = 0;
    std::atomic<uint64_t> midiPacketCount { 0 };
    std::atomic<uint64_t> sourceSamplesProduced { 0 };
    std::atomic<uint64_t> sourceNonZeroSamples { 0 };
    std::atomic<uint64_t> sourceDroppedSamples { 0 };
    std::atomic<uint64_t> midiDroppedBytes { 0 };
    std::atomic<uint64_t> sourceUnderruns { 0 };

    // A 5 Hz one-pole high-pass, as the output coupling capacitor.
    double dcCoefficient = 0.0;
    float dcPreviousInput[2] { 0.0f, 0.0f };
    float dcPreviousOutput[2] { 0.0f, 0.0f };
    float lastSourceFrame[2] { 0.0f, 0.0f };

    double hostSampleRate = 0.0;
    double sourceSampleRate = 0.0;
    double sourcePosition = 0.0;
    std::atomic<bool> ready { false };

    // Published only when the UI asks; the audio callback builds no display data.
    mutable std::atomic<bool> debugStateRequested { false };
    std::atomic<uint64_t> debugRenderedFrames { 0 };
    std::atomic<uint32_t> debugKeyedVoices { 0 };
    std::atomic<bool> debugAllLed { false };
    std::atomic<bool> debugMuteLed { false };
    std::atomic<bool> debugSoloEnabled { false };
    std::atomic<bool> debugStandby { false }, debugFastDisplayScroll { false };
    NativeMidiInputState midiInputState;
    mutable std::unique_ptr<sc55::PanelRasterSysExController> panelRasterSysEx;
    std::unique_ptr<GS55LcdModel> lcdModel; // message thread

    std::unique_ptr<sc55::NativeSynth> synth;
    mutable NativeSynthStateExchange stateExchange;
    mutable std::atomic<bool> stateRequested { false };
    static constexpr unsigned panelCapacity = 64;
    std::array<sc55::SynthCommand, panelCapacity> panelQueue {};
    std::atomic<unsigned> panelRead { 0 }, panelWrite { 0 };
    // Message-thread interaction state; audio never reads it. A lifecycle
    // reset is handed over by generation, not by a concurrent write.
    std::atomic<unsigned> panelGenerationShared { 0 };
    unsigned panelGeneration = 0;
    uint8_t panelPart = 0;
    bool panelAll = false, panelSolo = false, panelStandby = false;

    std::string error;
};
