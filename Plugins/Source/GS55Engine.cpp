#include "GS55Engine.h"

#include "SC55Debug.h"
#include "NativeSoundDataCache.h"
#include "NativeMeterDecay.h"
#include "sc55_synth.h"
#include "sc55_display.h"
#include "sc55_rom_set.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace
{
constexpr double dcBlockerHz = 5.0;
constexpr double pi = 3.14159265358979323846;
}

// Builds the LCD's contents from the synth's published state, on the message
// thread: the selected part's (or all parts') values, display messages from
// SysEx over the instrument field, and the decaying part meters.
class GS55LcdModel
{
public:
    GS55Engine::LcdFrame update (const sc55::SynthState& state, bool fastScroll)
    {
        GS55Engine::LcdFrame frame;
        frame.enabled = ! state.failed;
        const auto view = display.update (state.displayEvents, fastScroll);
        const auto gsPart = [] (unsigned p) { return p == 9 ? 0u : p < 9 ? p + 1 : p; };
        const auto number = [] (std::array<uint8_t, 3>& field, int value)
        {
            char text[16];
            std::snprintf (text, sizeof (text), "%3d", value);
            std::copy_n (text, 3, field.begin());
        };
        frame.instrument.fill (' ');
        if (state.allSelected)
        {
            std::copy_n ("ALL", 3, frame.part.begin());
            std::copy_n ("    ALL PARTS   ", 16, frame.instrument.begin());
            number (frame.level, state.masterVolume);
            number (frame.pan, int (state.masterPan) - 64);
            number (frame.reverb, state.reverbLevel);
            number (frame.chorus, state.chorusLevel);
            number (frame.keyShift, int (state.masterKeyShift) - 64);
            number (frame.midiChannel, state.midiInput.deviceId + 1);
        }
        else
        {
            const auto& part = state.parts[gsPart (state.selectedPart)];
            number (frame.part, state.selectedPart + 1);
            std::array<uint8_t, 16> line;
            line.fill (' ');
            // The part line shows the program number, then the name.
            char program[8];
            std::snprintf (program, sizeof (program), "%3d ", part.program + 1);
            std::copy_n (program, 4, line.begin());
            std::copy_n (part.name.begin(), std::min<std::size_t> (part.name.size(), 12), line.begin() + 4);
            frame.instrument = line;
            number (frame.level, part.volume);
            number (frame.pan, int (part.pan) - 64);
            number (frame.reverb, part.reverb);
            number (frame.chorus, part.chorus);
            number (frame.keyShift, int (part.keyShift) - 64);
            if (part.channel < 16) number (frame.midiChannel, part.channel + 1);
            else std::copy_n ("OFF", 3, frame.midiChannel.begin());
        }
        if (view.text.visible)
            frame.instrument = view.text.compose (frame.instrument);

        std::array<uint16_t, 16> targets {};
        for (unsigned p = 0; p < 16; ++p)
            targets[p] = state.parts[gsPart (p)].envelopeLevel;
        frame.meter = meters.update (targets,
            std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count());
        frame.meterFromGlyphs = view.bitmapVisible;
        if (view.bitmapVisible)
            for (unsigned g = 0; g < 8; ++g)
                std::copy_n (view.bitmap.begin() + g * 8, 8, frame.glyphs[g].begin());
        return frame;
    }

private:
    sc55::DisplayPresentation display;
    NativeMeterDecay meters;
};

GS55Engine::GS55Engine()
    : panelRasterSysEx (std::make_unique<sc55::PanelRasterSysExController>()),
      lcdModel (std::make_unique<GS55LcdModel>())
{
}

GS55Engine::~GS55Engine()
{
    release();
}

bool GS55Engine::hasRomSet (const std::string& romDirectory)
{
    // JUCE supplies UTF-8; a narrow path constructor uses the ANSI code page on Windows.
    return sc55::LoadRomSet (std::filesystem::u8path (romDirectory)).has_value();
}

void GS55Engine::logRomSetDiagnostics (const std::string& romDirectory)
{
#if JUCE_DEBUG
    std::string message;
    if (sc55::LoadRomSet (std::filesystem::u8path (romDirectory), &message))
        message = "complete";
    std::fprintf (stderr, "[DEBUG-SC55] ROM folder \"%s\": %s\n", romDirectory.c_str(), message.c_str());
    std::fflush (stderr);
#else
    (void) romDirectory;
#endif
}

bool GS55Engine::initialise (const std::string& romDirectory, double newHostSampleRate,
                             const std::string& soundDataCacheDirectory, unsigned maximumVoices)
{
    sc55debug::log ("initialise requested directory=\"%s\" hostRate=%.2f",
                    romDirectory.c_str(), newHostSampleRate);
    release();
    error.clear();
    hostSampleRate = newHostSampleRate;
    if (! std::isfinite (hostSampleRate) || hostSampleRate <= 0.0)
    {
        error = "Invalid host sample rate";
        return false;
    }

    std::string romError;
    const auto roms = sc55::LoadRomSet (std::filesystem::u8path (romDirectory), &romError);
    if (! roms)
    {
        error = romError;
        sc55debug::log ("ROM initialisation failed: %s", error.c_str());
        return false;
    }

    try
    {
        const auto cacheRoot = soundDataCacheDirectory.empty() ? juce::File()
            : juce::File (juce::String::fromUTF8 (soundDataCacheDirectory.c_str()));
        if (sc55::EnsureNativeSoundDataCache (roms->program, roms->programExtra, cacheRoot))
            sc55debug::log ("Generated sc55-native.sdata from the loaded ROMs");
        juce::MemoryBlock bytes;
        if (! cacheRoot.getChildFile ("mk1-v1.21-md15/sc55-native.sdata").loadFileAsData (bytes))
        {
            error = "Could not load the generated sound data";
            return false;
        }
        synth = std::make_unique<sc55::NativeSynth> (
            std::span (static_cast<const uint8_t*> (bytes.getData()), bytes.getSize()),
            roms->program, roms->programExtra, roms->waves[0], roms->waves[1], roms->waves[2], maximumVoices);
    }
    catch (const std::exception& exception)
    {
        error = std::string ("Sound engine preparation failed: ") + exception.what();
        synth.reset();
        return false;
    }

    sourceSampleRate = sc55::NativeSynth::sampleRate;
    sourceRead.store (0, std::memory_order_relaxed);
    sourceWrite.store (0, std::memory_order_relaxed);
    renderCallCount = 0;
    midiPacketCount.store (0, std::memory_order_relaxed);
    sourceSamplesProduced.store (0, std::memory_order_relaxed);
    sourceNonZeroSamples.store (0, std::memory_order_relaxed);
    sourceDroppedSamples.store (0, std::memory_order_relaxed);
    midiDroppedBytes.store (0, std::memory_order_relaxed);
    sourceUnderruns.store (0, std::memory_order_relaxed);
    sourcePosition = 0.0;
    lastSourceFrame[0] = lastSourceFrame[1] = 0.0f;
    dcCoefficient = 1.0 - (2.0 * pi * dcBlockerHz / hostSampleRate);
    dcPreviousInput[0] = dcPreviousInput[1] = 0.0f;
    dcPreviousOutput[0] = dcPreviousOutput[1] = 0.0f;

    synth->setMidiInputSettings (midiInputState.read());
    publishDebugState();
    stateExchange.publish (synth->state());
    ready.store (true, std::memory_order_release);
    sc55debug::log ("initialise succeeded sourceRate=%.2f voices=%u", sourceSampleRate, maximumVoices);
    return true;
}

void GS55Engine::release()
{
    ready.store (false, std::memory_order_release);
    panelRasterSysEx->reset();
    synth.reset();

    panelRead.store (panelWrite.load (std::memory_order_acquire), std::memory_order_release);
    panelGenerationShared.fetch_add (1, std::memory_order_release);
    sourceRead.store (0, std::memory_order_relaxed);
    sourceWrite.store (0, std::memory_order_relaxed);
    sourcePosition = 0.0;
    debugAllLed.store (false, std::memory_order_relaxed);
    debugMuteLed.store (false, std::memory_order_relaxed);
    debugSoloEnabled.store (false, std::memory_order_relaxed);
    debugStandby.store (false, std::memory_order_relaxed);
    debugFastDisplayScroll.store (false, std::memory_order_relaxed);
}

void GS55Engine::clearPendingMidi() noexcept
{
    midiRead.store (midiWrite.load (std::memory_order_acquire), std::memory_order_release);
}

void GS55Engine::sendMidi (const uint8_t* data, int size)
{
    if (data == nullptr || size <= 0)
        return;
    midiPacketCount.fetch_add (1, std::memory_order_relaxed);

    const auto panelResult = panelRasterSysEx->receiveSysEx (
        std::span<const uint8_t> (data, static_cast<size_t> (size)),
        midiInputState.read().receiveExclusive);
    if (panelResult != sc55::PanelRasterSysExController::PacketResult::ignored)
        return; // Private panel images never reach the sound engine.

    for (int i = 0; i < size; ++i)
    {
        if (! enqueueMidiByte (data[i]))
        {
            midiDroppedBytes.fetch_add (static_cast<uint64_t> (size - i), std::memory_order_relaxed);
            break;
        }
    }
}

void GS55Engine::pressFrontPanelButton (FrontPanelButton button)
{
    const auto generation = panelGenerationShared.load (std::memory_order_acquire);
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
        // A display preference, never queued for the audio thread.
        if (panelStandby)
            debugFastDisplayScroll.store (button == FrontPanelButton::fastScrollOn, std::memory_order_relaxed);
        return;
    }
    const auto write = panelWrite.load (std::memory_order_relaxed);
    const auto next = (write + 1) % panelCapacity;
    if (next == panelRead.load (std::memory_order_acquire))
    {
        sc55debug::log ("Panel command queue is full");
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
    panelQueue[write] = command;
    panelWrite.store (next, std::memory_order_release);
}

float GS55Engine::blockDc (int channel, float input) noexcept
{
    const float output = input - dcPreviousInput[channel]
                       + static_cast<float> (dcCoefficient) * dcPreviousOutput[channel];
    dcPreviousInput[channel] = input;
    dcPreviousOutput[channel] = output;
    return output;
}

void GS55Engine::pushSample (int32_t left, int32_t right)
{
    sourceSamplesProduced.fetch_add (1, std::memory_order_relaxed);
    if (left != 0 || right != 0)
        sourceNonZeroSamples.fetch_add (1, std::memory_order_relaxed);
    const auto write = sourceWrite.load (std::memory_order_relaxed);
    const auto nextWrite = (write + 1) % sourceFifoFrames;
    if (nextWrite == sourceRead.load (std::memory_order_acquire))
    {
        sourceDroppedSamples.fetch_add (1, std::memory_order_relaxed);
        return;
    }
    constexpr float scale = 1.0f / 2147483648.0f;
    sourceFifo[write][0] = float (left) * scale;
    sourceFifo[write][1] = float (right) * scale;
    sourceWrite.store (nextWrite, std::memory_order_release);
}

bool GS55Engine::enqueueMidiByte (uint8_t byte) noexcept
{
    const auto write = midiWrite.load (std::memory_order_relaxed);
    const auto nextWrite = (write + 1) % midiFifoBytes;
    if (nextWrite == midiRead.load (std::memory_order_acquire))
        return false;
    midiFifo[write] = byte;
    midiWrite.store (nextWrite, std::memory_order_release);
    return true;
}

uint32_t GS55Engine::availableSourceFrames() const noexcept
{
    const auto read = sourceRead.load (std::memory_order_acquire);
    const auto write = sourceWrite.load (std::memory_order_acquire);
    return write >= read ? write - read : sourceFifoFrames - (read - write);
}

const float* GS55Engine::sourceFrame (uint32_t offset) const noexcept
{
    return sourceFifo[(sourceRead.load (std::memory_order_acquire) + offset) % sourceFifoFrames];
}

void GS55Engine::consumeSourceFrames (uint32_t count) noexcept
{
    const auto read = sourceRead.load (std::memory_order_relaxed);
    count = std::min (count, availableSourceFrames());
    sourceRead.store ((read + count) % sourceFifoFrames, std::memory_order_release);
}

void GS55Engine::drainPanel() noexcept
{
    auto read = panelRead.load (std::memory_order_relaxed);
    const auto write = panelWrite.load (std::memory_order_acquire);
    while (read != write)
    {
        if (! synth->applyCommand (panelQueue[read]))
            break; // Backpressure: keep this command and its resolved target.
        read = (read + 1) % panelCapacity;
    }
    panelRead.store (read, std::memory_order_release);
}

void GS55Engine::drainMidi()
{
    auto read = midiRead.load (std::memory_order_relaxed);
    const auto write = midiWrite.load (std::memory_order_acquire);
    while (read != write)
    {
        const uint8_t byte = midiFifo[read];
        if (synth->push (std::span (&byte, 1)) != 1)
            break; // Keep the rest until the synth's queue has room.
        read = (read + 1) % midiFifoBytes;
    }
    midiRead.store (read, std::memory_order_release);
}

void GS55Engine::publishDebugState() noexcept
{
    const auto state = synth->state();
    debugRenderedFrames.store (state.renderedFrames, std::memory_order_relaxed);
    debugKeyedVoices.store (state.activeVoiceMask, std::memory_order_relaxed);
    const unsigned part = state.selectedPart == 9 ? 0 : state.selectedPart < 9 ? state.selectedPart + 1 : state.selectedPart;
    debugAllLed.store (state.allSelected, std::memory_order_relaxed);
    debugMuteLed.store (state.allSelected ? state.globalMuted : state.parts[part].muted, std::memory_order_relaxed);
    debugSoloEnabled.store (state.soloEnabled, std::memory_order_relaxed);
    debugStandby.store (synth->standby(), std::memory_order_relaxed);
}

GS55Engine::DebugState GS55Engine::getDebugState() const noexcept
{
    // Published on the next render; one render old, which is fine for display.
    debugStateRequested.store (true, std::memory_order_release);
    DebugState state;
    state.ready = ready.load (std::memory_order_acquire);
    state.renderedFrames = debugRenderedFrames.load (std::memory_order_relaxed);
    state.keyedVoices = debugKeyedVoices.load (std::memory_order_relaxed);
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

void GS55Engine::produceSourceFrames (uint32_t minimumFrames) noexcept
{
    if (synth == nullptr)
        return;
    if (const auto input = midiInputState.takeRestore())
        synth->setMidiInputSettings (*input);
    drainMidi();
    drainPanel();
    std::array<sc55::Frame, 256> frames;
    while (availableSourceFrames() < minimumFrames && ! synth->failed())
    {
        const auto count = std::min<uint32_t> (frames.size(), minimumFrames - availableSourceFrames());
        synth->render (std::span (frames.data(), count));
        for (uint32_t i = 0; i < count; ++i)
            pushSample (frames[i].left, frames[i].right);
        drainMidi();
    }
    if (synth->failed())
        ready.store (false, std::memory_order_release);
    midiInputState.publish (synth->midiInputSettings());
    if (stateRequested.exchange (false, std::memory_order_acquire))
        stateExchange.publish (synth->state());
    if (debugStateRequested.exchange (false, std::memory_order_acquire))
        publishDebugState();
}

bool GS55Engine::getNativeState (sc55::SynthState& destination) const noexcept
{
    if (! ready.load (std::memory_order_acquire))
        return false;
    destination = stateExchange.read();
    destination.calculateDisplayLevels();
    stateRequested.store (true, std::memory_order_release);
    return true;
}

bool GS55Engine::getLcdFrame (LcdFrame& destination) const
{
    sc55::SynthState state;
    if (! getNativeState (state))
        return false;
    destination = lcdModel->update (state, debugFastDisplayScroll.load (std::memory_order_relaxed));
    return true;
}

bool GS55Engine::copyPanelRaster (uint8_t* destination, size_t stride, bool* contentChanged, bool forceCopy) const
{
    return panelRasterSysEx->copyLatestToMask (destination, stride, contentChanged, forceCopy);
}

void GS55Engine::render (float* left, float* right, int numSamples)
{
    if (left == nullptr || numSamples <= 0)
        return;
    if (! ready.load (std::memory_order_acquire) || hostSampleRate <= 0.0 || sourceSampleRate <= 0.0)
    {
        renderSegment (left, right, numSamples);
        return;
    }
    // A host may ask for more than the source FIFO holds: consume each bounded
    // segment before producing the next. Leave room for interpolation lookahead.
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

void GS55Engine::renderSegment (float* left, float* right, int numSamples)
{
    ++renderCallCount;
    const auto silence = [&]
    {
        std::memset (left, 0, static_cast<size_t> (numSamples) * sizeof (float));
        if (right != nullptr)
            std::memset (right, 0, static_cast<size_t> (numSamples) * sizeof (float));
    };
    if (! ready.load (std::memory_order_acquire) || hostSampleRate <= 0.0 || sourceSampleRate <= 0.0)
        return silence();

    const double sourceStep = sourceSampleRate / hostSampleRate;
    const double lastSourcePosition = sourcePosition + sourceStep * static_cast<double> (numSamples - 1);
    produceSourceFrames (static_cast<uint32_t> (std::floor (lastSourcePosition)) + 4u);
    if (! ready.load (std::memory_order_acquire))
        return silence();

    for (int i = 0; i < numSamples; ++i)
    {
        const uint32_t base = static_cast<uint32_t> (sourcePosition);
        float leftSample = lastSourceFrame[0];
        float rightSample = lastSourceFrame[1];
        if (availableSourceFrames() >= base + 4)
        {
            // Catmull-Rom across four 32 kHz frames.
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
            sourceUnderruns.fetch_add (1, std::memory_order_relaxed);

        left[i] = blockDc (0, leftSample);
        if (right != nullptr)
            right[i] = blockDc (1, rightSample);
        sourcePosition += sourceStep;
        if (const uint32_t consumed = static_cast<uint32_t> (sourcePosition); consumed != 0)
        {
            consumeSourceFrames (consumed);
            sourcePosition -= consumed;
        }
    }
}
