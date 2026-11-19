// SIDiMIDI – the table of SID files: columns, sorting, search masks under the columns, hearts and stars.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "Archive.h"
#include "Look.h"
#include "Lang.h"

namespace sm
{
enum Col
{
    cFav = 1, cRating, cTitle, cAuthor, cReleased, cSongs, cLength, cType, cSid, cClock, cChips,
    cComposer, cCountry, cArea, cLoad, cSize, cFile, cPath, cStil, cNote, cLandscape, cLastCol
};

struct ColDef
{
    int id;
    const char* name;      // German (T())
    int width;
    bool numeric;
    bool visible;
    const char* tip;
};
const ColDef& colDef (int id);

juce::String niceCountry (const juce::String& upper);
juce::String cellText (const Entry& e, int col);

// miniature of a tune for the "Verlauf" column: the three voices stacked as little mountain ranges
struct Landscape
{
    static constexpr int kBuckets = 160;
    std::array<std::vector<juce::uint8>, 3> height;   // per voice and time bucket: 0 = silent, 1 … 255 = relative pitch
};

class SidTable : public juce::Component, private juce::TableListBoxModel, private juce::Timer
{
public:
    explicit SidTable (Archive& a);
    ~SidTable() override;

    std::function<void (Entry*)> onSelect;                              // selection changed (nullptr = none)
    std::function<void (Entry*)> onPlay;                                // double click / Enter
    std::function<void (Entry&, int col, juce::Point<int> screen)> onMenu;
    std::function<void()> onListChanged;                                // rows rebuilt

    void refresh();                                                     // rebuild the rows after the archive changed
    void setGlobalSearch (const juce::String& s);
    void setOnlyFavourites (bool b);
    void setMinRating (int stars);
    void setOnlyType (int t);                                           // 0 all, 1 HVSC only, 2 not in HVSC, 3 with STIL, 4 with bug note, 5 identical files
    void clearMasks();
    bool hasFilters() const;

    int numRows() const { return (int) rows.size(); }
    Entry* rowEntry (int row) const { return row >= 0 && row < (int) rows.size() ? rows[(size_t) row] : nullptr; }
    Entry* selectedEntry() const;
    std::vector<int> selectedIds() const;
    std::vector<Entry*> selectedEntries() const;
    void selectEntry (int id);
    void selectRow (int row);
    int rowOf (int id) const;
    juce::TableListBox& list() { return table; }
    int totalDurationOfShown (double& seconds) const;

    void applyLanguage();
    juce::String layoutString() const;
    void restoreLayout (const juce::String& s);
    void showPlayingRow (int id) { playingId = id; table.repaint(); }

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    Archive& arch;
    juce::TableListBox table;
    std::vector<Entry*> rows;
    std::map<int, std::unique_ptr<juce::TextEditor>> masks;
    juce::Component footer;
    juce::String globalSearch;
    bool onlyFav = false;
    int minRating = 0, onlyType = 0;
    int sortCol = cTitle;
    bool sortForward = true;
    int playingId = 0;
    int lastSelectedId = 0;
    bool inRefresh = false, masksChanged = false;
    void* watcher = nullptr;   // HeaderWatcher (SidTable.cpp)

    // "Verlauf" column: computed in the background for the rows on screen only
    struct LandscapeState { std::atomic<int> from { 0 }, to { 0 }; std::atomic<bool> alive { true }; };
    std::shared_ptr<LandscapeState> lsState = std::make_shared<LandscapeState>();
    std::unordered_map<int, std::shared_ptr<Landscape>> landscapes;   // by entry id
    std::unordered_set<int> landscapeAsked;
    void requestLandscape (const Entry& e, int row);
    void drawLandscape (juce::Graphics& g, const Landscape* l, int w, int h);

    const Theme& theme() const;
    void layoutFooter();
    bool passes (const Entry& e) const;
    void sortRows();

    int getNumRows() override { return (int) rows.size(); }
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    void sortOrderChanged (int newSortColumnId, bool isForwards) override;
    void cellClicked (int row, int col, const juce::MouseEvent&) override;
    void cellDoubleClicked (int row, int col, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void returnKeyPressed (int lastRow) override;
    juce::String getCellTooltip (int row, int col) override;
    void backgroundClicked (const juce::MouseEvent&) override;
    void timerCallback() override;

    juce::ThreadPool landscapePool { 1 };   // last member: stops first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidTable)
};
} // namespace sm
