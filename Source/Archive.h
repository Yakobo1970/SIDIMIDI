// SIDiMIDI – the SID archive: folders, scanned entries, ratings and favourites. Shared by all windows of the process.
#pragma once
#include <functional>
#include <deque>
#include <memory>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include "SidInfo.h"
#include "Hvsc.h"

namespace sm
{
enum Flags : juce::uint32
{
    fMissing = 4, fError = 8, fRsid = 16, fHasStil = 32, fHasBug = 64, fHvsc = 128, fMulti = 256,
    fMus = 512, fBasic = 1024,
    fProtected = 2048,     // inside a write-protected folder (not stored, recomputed)
    fSameFile = 4096       // another entry has exactly the same content (not stored, recomputed)
};

struct Entry
{
    int id = 0;
    juce::String path, name, folder, rel;       // rel: path inside HVSC ("/MUSICIANS/H/Hubbard_Rob/Commando.sid"), else empty
    juce::int64 size = 0, mtime = 0;
    juce::uint32 flags = 0;
    juce::String md5;
    juce::String title, author, released, error;
    juce::String composer, area, country, stilFirst;   // folder-derived: composer folder, GAMES/DEMOS/MUSICIANS, country, first STIL line
    int year = 0, songs = 1, startSong = 1, version = 1;
    juce::uint8 clock = 0, model = 0, sidCount = 1;
    juce::uint16 load = 0, init = 0, play = 0;
    int dataSize = 0;
    float seconds = 0;                      // length of the start sub-tune (0 = unknown)
    std::vector<float> lengths;             // per sub-tune (HVSC song length database), may be empty
    int rating = 0;
    bool favourite = false;
    int sameGroup = 0;                      // runtime: number of entries with the same content (>1 = duplicates)

    bool has (Flags f) const { return (flags & f) != 0; }
    double lengthOf (int song1) const
    {
        if (song1 >= 1 && song1 <= (int) lengths.size()) return lengths[(size_t) song1 - 1];
        return 0.0;
    }
    double totalSeconds() const { double t = 0; for (auto v : lengths) t += v; return t; }
};

class Archive : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    Archive();
    ~Archive() override;

    // ---- data (message thread only)
    const std::vector<std::unique_ptr<Entry>>& entries() const { return list; }
    Entry* byId (int id) const;
    Entry* byPath (const juce::String& path) const;
    juce::uint32 version() const { return changeCounter; }

    juce::StringArray folders;
    juce::ValueTree settings { "SETTINGS" };   // UI layout, theme, language, protected folders ...

    juce::String addFolder (const juce::File& dir);   // returns the archive folder that now covers dir
    juce::StringArray protectedFolders() const;
    bool isProtected (const juce::String& path) const;
    void setProtected (const juce::String& folder, bool on);
    int removeFolder (const juce::String& path);      // takes the folder and its entries out of the archive
    int countInFolder (const juce::String& path) const;
    int removeMissing();

    // ---- HVSC
    // the collection(s) found in the archive folders (docs are loaded in the background)
    std::vector<std::shared_ptr<Hvsc>> hvscList() const;
    std::shared_ptr<Hvsc> hvscFor (const juce::String& path) const;
    void reloadHvsc();                                 // re-reads the version and the documents (after an update)

    // ---- scanning
    void startScan (bool fullRescan = false, const juce::StringArray& only = {});
    void stopScan();
    bool isScanning() const { return scanner != nullptr; }
    juce::String scanStatus() const;
    int scanTotal() const { return total.load(); }
    int scanDone() const { return done.load(); }
    void pump();                   // integrates scan results (called by the timer; tools may call it directly)
    void waitForScan();            // tools: blocks and pumps until finished

    // ---- edits (database only, never touches a file)
    void setRating (Entry& e, int rating);
    void setFavourite (Entry& e, bool fav);
    void removeEntries (const std::vector<int>& ids);                       // database only
    void relocate (Entry& e, const juce::File& newFile);                    // after a move/rename: keeps id, rating, favourite
    Entry* addFile (const juce::File& f);                                   // analyse now
    bool isInsideFolders (const juce::File& f) const;
    int duplicateCount() const { return duplicates; }

    // ---- editing the SID header (title, author, released) - writes into the file, keeps the original once, can be undone
    enum SidField { fieldTitle = 0, fieldAuthor = 1, fieldReleased = 2 };
    bool setSidField (Entry& e, int field, const juce::String& value, juce::String& err, bool record = true);
    juce::File backupFolder() const { return dataFolder().getChildFile ("Sicherung"); }
    juce::String protectedMessage() const;
    // everything written inside one group is undone together
    struct UndoGroup
    {
        UndoGroup (Archive& a, const juce::String& label);
        ~UndoGroup();
        Archive& arch;
    };
    bool canUndo() const { return ! undoStack.empty(); }
    juce::String undoLabel() const { return undoStack.empty() ? juce::String() : undoStack.back().label; }
    int undo (juce::String& failed);                                        // returns the number of restored values

    // ---- persistence
    juce::File dataFolder() const;
    juce::File settingsFile() const;
    juce::File databaseFile() const;
    void save();
    void saveSoon() { dirty = true; }
    void settingsChanged() { settingsDirty = true; }     // layout, theme ... (a small file, saved within seconds)
    bool load();

private:
    struct Scanner;
    std::vector<std::unique_ptr<Entry>> list;
    std::unordered_map<juce::String, Entry*> pathIndex;
    std::unordered_map<int, Entry*> idIndex;
    int nextId = 1;
    juce::uint32 changeCounter = 1;
    bool dirty = false, settingsDirty = false;
    int saveCountdown = 0;
    int duplicates = 0;

    struct UndoItem { juce::String path; int field = 0; juce::String oldValue; };
    struct UndoStep { juce::String label; std::vector<UndoItem> items; };
    std::vector<UndoStep> undoStack;
    int groupDepth = 0;

    std::unique_ptr<Scanner> scanner;
    juce::CriticalSection pendingLock;
    std::deque<std::unique_ptr<Entry>> pending;
    juce::StringArray seenPaths;
    std::atomic<int> total { 0 }, done { 0 };
    juce::String scanPhase;
    juce::uint32 scanStart = 0;

    mutable juce::CriticalSection hvscLock;
    std::vector<std::shared_ptr<Hvsc>> hvscs;
    std::thread hvscThread;
    std::atomic<bool> hvscLoading { false };
    std::shared_ptr<Hvsc> ensureHvsc (const juce::File& folder);   // any thread: opens (and loads) the collection around a folder
    void startHvscLoad();

    int removeWhere (const std::function<bool (const Entry&)>& pred);
    void applyProtection();
    void markProtection (Entry& e, const juce::StringArray& prot) const;
    void markSameContent();
    void insertOrReplace (std::unique_ptr<Entry> e);
    void changed() { ++changeCounter; sendChangeMessage(); }
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Archive)
};

// builds an Entry from a parsed header (and the HVSC documents, if given)
std::unique_ptr<Entry> makeEntry (const juce::File& file, const SidInfo& si, const Hvsc* hvsc);
juce::String lengthText (const Entry& e);
} // namespace sm
