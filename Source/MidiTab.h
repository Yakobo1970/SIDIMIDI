// SIDiMIDI – tab "MIDI -> SID": open a folder of MIDI files, convert single files to SID with different depths.
#pragma once
#include "VoiceMix.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <array>
#include <map>
#include <thread>
#include "Look.h"
#include "Lang.h"

namespace sm
{
class MainComponent;

struct MidiItem
{
    juce::File file;
    juce::String name, folder;      // folder relative to the opened folder
    int tracks = 0, notes = 0;      // tracks that contain notes
    double seconds = 0;
    bool valid = false, roundTrip = false;
    juce::File result;              // the .sid written by the last conversion
};

class MidiTab : public juce::Component, private juce::TableListBoxModel, private juce::Timer
{
public:
    explicit MidiTab (MainComponent& m);
    ~MidiTab() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    bool handleKey (const juce::KeyPress&);
    void applyLanguage();
    void applyTheme();
    void saveState();
    void openFolder (const juce::File& dir);
    juce::String statusSummary() const;
    void stopPreview();

    // conversion (also used by the track panel)
    enum class Depth { quick, targeted, roundTrip, tracker };
    void convert (int itemIndex, Depth depth);
    void convertTracks (int itemIndex, const std::array<int, 3>& tracks, const std::array<int, 3>& roles, bool roundTrip);
    void convertTracks (int itemIndex, const VoiceSources& sources, bool roundTrip);        // several tracks per voice
    void convertToTracker (int itemIndex, const VoiceSources& sources);
    void convertToTracker (int itemIndex, const std::array<int, 3>& parts, const std::array<int, 3>& roles);     // parts: MIDI tracks for the three voices (-1 = none); roles as in the targeted conversion
    void playMidi (int itemIndex);
    void playSidResult (int itemIndex);
    const MidiItem* item (int index) const { return index >= 0 && index < (int) items.size() ? &items[(size_t) index] : nullptr; }
    int itemOfRow (int row) const { return row >= 0 && row < (int) shown.size() ? shown[(size_t) row] : -1; }
    void selectFirstForTest() { if (! shown.empty()) table.selectRow (0); }
    void openTrackPanelForTest (int itemIndex, bool tracker = false) { convert (itemIndex, tracker ? Depth::tracker : Depth::targeted); }

private:
    MainComponent& main;
    juce::TableListBox table;
    std::vector<MidiItem> items;
    std::vector<int> shown;                  // indices into items (filtered, sorted)
    juce::File folder;
    juce::TextButton openBtn, recentBtn, rescanBtn, stopBtn, fundusBtn;
    juce::ToggleButton subfolders;
    juce::TextEditor search, description;
    juce::Label pathLabel, fundusText, descriptionTitle;
    juce::Rectangle<int> toolbarArea, infoBox;
    std::unique_ptr<juce::FileChooser> chooser;
    int sortCol = 2;
    bool sortForward = true;

    // scanning
    struct Scan;
    std::unique_ptr<Scan> scan;
    juce::CriticalSection pendingLock;
    std::vector<MidiItem> pending;
    std::atomic<bool> scanFinished { true };
    std::atomic<int> scanTotal { 0 };
    int playingItem = -1;
    juce::String lastMessage;

    std::map<juce::String, juce::String> knownResults;   // MIDI path -> the .sid we wrote from it (also in other folders)
    void rememberResult (const juce::File& midi, const juce::File& sid);
    void startScan();
    void stopScan();
    void rebuildShown();
    void showRecentMenu();
    void runConversion (int itemIndex, const VoiceSources& sources, bool roundTrip, const juce::File& target);
    void timerCallback() override;

    // TableListBoxModel
    int getNumRows() override { return (int) shown.size(); }
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    juce::Component* refreshComponentForCell (int row, int col, bool selected, juce::Component* existing) override;
    void sortOrderChanged (int col, bool forwards) override { sortCol = col; sortForward = forwards; rebuildShown(); }
    void cellDoubleClicked (int row, int col, const juce::MouseEvent&) override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiTab)
};
} // namespace sm
