#pragma once

#include <JuceHeader.h>

#include <memory>

class NukedSC55AudioProcessor;
struct MidiFileData;

/**
    Standalone-only companion window for an RCP's WRD text and MAG background
    layers.  The window polls an immutable frame list on the message thread.
*/
class WrdDisplayWindow final : public juce::DocumentWindow,
                               private juce::Timer
{
public:
    explicit WrdDisplayWindow (NukedSC55AudioProcessor& processor);
    ~WrdDisplayWindow() override;

    void showForFile (const juce::String& fileName);
    void hideForPlaybackStop();

private:
    class WindowConstrainer;
    class Content;

    void updateWindowFrameSize();
    void timerCallback() override;
    void closeButtonPressed() override;

    NukedSC55AudioProcessor& processor;
    std::unique_ptr<WindowConstrainer> windowConstrainer;
    Content* content = nullptr;
    const MidiFileData* displayedFile = nullptr;
    std::size_t displayedFrame = static_cast<std::size_t> (-1);
    bool userClosed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WrdDisplayWindow)
};
