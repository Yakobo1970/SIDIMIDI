// SIDiMIDI – tab "MIDI -> SID": open a folder of MIDI files, convert single files to SID with different depths.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Look.h"
#include "Lang.h"

namespace sm
{
class MainComponent;

class MidiTab : public juce::Component
{
public:
    explicit MidiTab (MainComponent& m);
    ~MidiTab() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    bool handleKey (const juce::KeyPress&) { return false; }
    void applyLanguage() {}
    void applyTheme() { repaint(); }
    void saveState() {}
    void openFolder (const juce::File&) {}
    juce::String statusSummary() const { return {}; }
private:
    MainComponent& main;
};
}
