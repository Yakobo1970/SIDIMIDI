// SIDiMIDI – the tracker window: edits tunes of JCH's NewPlayer editor (see NpTune.h) and plays them through the emulation.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include "NpTune.h"
#include "Look.h"
#include "Lang.h"

namespace sm
{
class MainComponent;
class TrackerPanel;

class TrackerWindow : public juce::DocumentWindow
{
public:
    explicit TrackerWindow (MainComponent& m);
    ~TrackerWindow() override;
    void closeButtonPressed() override;
    TrackerPanel& panel() { return *content; }
    void importMidiFile (const juce::File& midi, std::array<int, 3> parts = { { -2, -2, -2 } }, std::array<int, 3> roles = { { 0, 0, 0 } });         // MIDI -> NewPlayer tune, loaded into the tracker (asks before it drops unsaved changes)
    void setupForTest (const juce::File& tune, int row, int voice, bool play, const juce::String& keys = {}, const juce::File& exportSid = {});   // UiShot: load, put the cursor, optionally play
    std::function<void()> onClosed;                 // the owner drops the window

private:
    MainComponent& main;
    TrackerPanel* content = nullptr;                // owned by the window
};
} // namespace sm
