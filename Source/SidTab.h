// SIDiMIDI – tab "SID>MIDI": the SID archive (database of all SID files), the course of the selected tune, HVSC information.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "SidTable.h"
#include "SidView.h"
#include "Look.h"
#include "Lang.h"

class AudioPlayerEngine;

namespace sm
{
class MainComponent;

class DragBar : public juce::Component
{
public:
    std::function<void (int dy)> onDrag;
    std::function<void()> onEnd;
    DragBar() { setMouseCursor (juce::MouseCursor::UpDownResizeCursor); }
    void paint (juce::Graphics& g) override;
    void mouseDrag (const juce::MouseEvent& e) override { if (onDrag) onDrag (e.getDistanceFromDragStartY()); }
    void mouseUp (const juce::MouseEvent&) override { if (onEnd) onEnd(); }
};

class PositionBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<double()> getFraction;       // 0 … 1
    std::function<juce::String()> getText;
    std::function<void (double)> onSeekFraction;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override { seek (e); }
    void mouseDrag (const juce::MouseEvent& e) override { seek (e); }
private:
    void seek (const juce::MouseEvent& e) { if (onSeekFraction) onSeekFraction (juce::jlimit (0.0, 1.0, (double) e.x / (double) std::max (1, getWidth()))); }
};

// the repeat symbol (two arrows chasing each other in a circle) as a toggle button
class RepeatButton : public juce::Button
{
public:
    RepeatButton() : juce::Button ("repeat") { setClickingTogglesState (true); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
};

// previous / next song (sub-tune) of the tune: yellow while there is one to go to, dark blue otherwise
class SkipButton : public juce::Button
{
public:
    explicit SkipButton (bool forwardDirection) : juce::Button ("skip"), forward (forwardDirection) {}
    void paintButton (juce::Graphics&, bool over, bool down) override;
private:
    bool forward;
};

class SidTab : public juce::Component, private juce::Timer
{
public:
    explicit SidTab (MainComponent& m);
    ~SidTab() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    bool handleKey (const juce::KeyPress&);

    void refreshAll();                  // the archive changed
    void applyLanguage();
    void applyTheme();
    void saveState();
    void restoreState();

    // playback
    void play (Entry* e, int song = 0);
    void togglePlay();                  // space
    void stopPlayback();
    bool isPlaying() const;
    void playNext (int delta, bool wrap = false);
    void applyPlayRange();              // the song length ends the playback; with repeat it starts again at once (no pause)
    void stepTune (int delta);          // previous / next tune of the list (plays it when something is playing); repeat stays as set
    void stepSong (int delta);          // previous / next song (sub-tune) of the selected tune

    // folders and scanning
    void addFolders (const juce::StringArray& paths);
    void addFolderDialog();
    void startScanAll();
    void showFolderMenu();
    void showHvscMenu();                // the HVSC window (from the settings menu)
    int hvscNewestVersion() const { return hvscNewest; }   // 0 = none known
    void focusSearch();
    void undoEdit();                    // Ctrl + Z: takes back the last change of title / author / released

    SidTable& table() { return list; }
    SidView& view() { return graph; }
    Entry* selected() const { return list.selectedEntry(); }
    int chosenSong() const;

    // used by the test tool
    void selectRowForTest (int row) { list.selectRow (row); }
    void selectSongForTest (int song) { songBox.setSelectedId (song, juce::dontSendNotification); onSongChosen(); }

private:
    MainComponent& main;
    Archive& arch;
    SidTable list;
    SidView graph;
    DragBar dragBar;
    PositionBar position;
    juce::TextButton folders, scan, playBtn, stopBtn, filterBtn, ratingBtn, clearFilters, zoomReset;
    juce::TextButton onlyFav, trackerBtn;     // only the heart (favourites) and the green tracker button
    RepeatButton repeatBtn;                  // repeat the current song
    SkipButton prevSong { false }, nextSong { true };
    juce::TextButton prevTune, nextTune;     // one whole tune up / down the list
    juce::TextEditor search, info;
    juce::ComboBox songBox;
    juce::Label emptyHint, songLabel;
    juce::Rectangle<int> toolbarArea, graphHeader;
    std::unique_ptr<juce::FileChooser> chooser;
    int viewHeight = 330, dragStart = 330;
    int infoWidth = 0;
    bool hvscChecked = false;
    int hvscNewest = 0;
    int filterType = 0, minStars = 0;
    int playingId = 0, playingSong = 0;
    bool pendingPlay = false, paused = false;
    juce::int64 pendingStartFrame = 0;
    int shownInfoId = -1;
    juce::String shownInfoText;
    juce::uint32 lastListVersion = 0;
    juce::uint32 lastInfoTick = 0;

    void onSelectionChanged (Entry* e);
    void onSongChosen();
    void startPlayback (juce::uint32 frame);
    void updateInfo (Entry* e);
    juce::String infoText (const Entry& e) const;
    void showEntryMenu (Entry& e, int col, juce::Point<int> screen);
    void showFilterMenu();
    void showRatingMenu();
    void updateButtons();
    void updateEmptyHint();
    // 0 = start song, -1 = all songs; folder = where the files go (default: next to the SID file)
    void convertToMidi (const std::vector<Entry*>& entries, int songOrZero, const juce::File& folder = {});
    void saveMidiAs (Entry& e, int song);             // file dialog with a name, then converts at once
    void saveMidiInto (const std::vector<Entry*>& entries);   // folder dialog, then converts at once
    juce::File midiTargetFor (const Entry& e, int song, bool allSongs, const juce::File& folder) const;
    juce::File lastMidiDir (const Entry& e) const;
    void runMidiJobs (std::vector<std::tuple<juce::String, int, juce::File, juce::String>> jobs);
    void renameEntry (Entry& e);
    void copyOrMove (const std::vector<Entry*>& entries, bool move);
    void trashEntries (const std::vector<Entry*>& entries);
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidTab)
};
} // namespace sm
