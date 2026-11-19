// SIDiMIDI – the HVSC (High Voltage SID Collection) documents: version, song lengths, STIL, bug list, musicians.
#pragma once
#include <juce_core/juce_core.h>
#include <unordered_map>
#include <memory>
#include <vector>

namespace sm
{
struct Musician { juce::String handle, name, group, country; };

class Hvsc
{
public:
    juce::File root;                       // the "C64Music" folder
    int version = 0;                       // 0 = unknown
    juce::String versionDate;              // "December 23, 2022" (from the update script), may be empty

    // a folder is an HVSC root if it holds DOCUMENTS\Songlengths.md5 (or STIL.txt) and a MUSICIANS or GAMES folder
    static bool isRoot (const juce::File& dir);
    // looks at the folder itself, its parents (up to 6 levels) and its children (2 levels); returns {} if none
    static juce::File findRoot (const juce::File& dir);
    static std::shared_ptr<Hvsc> open (const juce::File& rootDir);   // reads only the version (fast)

    // Reads Songlengths.md5, STIL.txt, BUGlist.txt, Musicians.txt (a second or two). Call once, from a worker thread.
    void loadDocs (const std::function<bool()>& cancelled = {});
    bool docsLoaded() const { return loaded; }

    juce::String relPath (const juce::File& f) const;                  // "/MUSICIANS/H/Hubbard_Rob/Commando.sid" or {}
    const std::vector<float>* lengths (const juce::String& md5) const; // seconds per sub-tune (HVSC's new format)
    juce::String stil (const juce::String& rel) const;                 // entry of the file (without directory comments)
    juce::String stilDirectory (const juce::String& rel) const;        // comments that belong to the folders above it
    juce::String bug (const juce::String& rel) const;
    bool hasStil (const juce::String& rel) const { return stilByPath.count (rel) != 0; }
    bool hasBug (const juce::String& rel) const { return bugByPath.count (rel) != 0; }
    const Musician* musicianFor (const juce::String& rel) const;       // by the composer's folder name
    int lengthEntries() const { return (int) lengthsByMd5.size(); }
    int stilEntries() const { return (int) stilByPath.size(); }
    int bugEntries() const { return (int) bugByPath.size(); }
    int musicianEntries() const { return (int) musicians.size(); }
    juce::String docsFolderName() const { return "DOCUMENTS"; }

    // the version found in DOCUMENTS (highest UpdateNN.hvs, else hv_sids.txt)
    static int detectVersion (const juce::File& rootDir, juce::String* date = nullptr);
    // parsing helpers (also used by the test tool)
    static std::vector<float> parseLengthList (const juce::String& value);

private:
    bool loaded = false;
    std::unordered_map<std::string, std::vector<float>> lengthsByMd5;
    std::unordered_map<juce::String, juce::String> stilByPath, bugByPath;
    std::unordered_map<juce::String, Musician> musicians;               // normalized names -> musician
public:
    static juce::String normaliseName (const juce::String& s);
private:
    void loadLengths (const juce::File& f);
    void loadStil (const juce::File& f);
    void loadBugs (const juce::File& f);
    void loadMusicians (const juce::File& f);
};

// text of a file in HVSC's documents: UTF-8 if valid, else Latin-1
juce::String readDocumentText (const juce::File& f);
} // namespace sm
