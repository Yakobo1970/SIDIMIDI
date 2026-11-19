// SIDiMIDI – tab "C64-SID": the SID archive (database of all SID files), the course of the selected tune, HVSC information.
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
    void playNext (int delta);

    // folders and scanning
    void addFolders (const juce::StringArray& paths);
    void addFolderDialog();
    void startScanAll();
    void showFolderMenu();
    void showHvscMenu();
    void focusSearch();

    SidTable& table() { return list; }
    SidView& view() { return graph; }
    Entry* selected() const { return list.selectedEntry(); }
    int chosenSong() const;

    // used by the test tool
    void selectRowForTest (int row) { list.selectRow (row); }

private:
    MainComponent& main;
    Archive& arch;
    SidTable list;
    SidView graph;
    DragBar dragBar;
    PositionBar position;
    juce::TextButton folders, scan, hvscBtn, playBtn, stopBtn, filterBtn, ratingBtn, clearFilters, zoomReset;
    juce::ToggleButton onlyFav, autoNext;
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
    void convertToMidi (const std::vector<Entry*>& entries, int songOrZero);   // 0 = start song, -1 = all songs
    juce::File midiTargetFor (const Entry& e, int song, bool allSongs) const;
    void renameEntry (Entry& e);
    void copyOrMove (const std::vector<Entry*>& entries, bool move);
    void trashEntries (const std::vector<Entry*>& entries);
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidTab)
};
} // namespace sm
