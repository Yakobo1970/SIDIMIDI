// SIDiMIDI – HVSC updates: version check against the mirrors, download, and applying the official update scripts (UpdateNN.hvs)
// without the Windows-only update tool. Every file that is replaced, moved over or deleted is backed up first.
#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

namespace sm
{
struct HvscOp
{
    enum Type { replace, move, del, mkdir, title, author, released, songs, speed, initplay, flags, clock, sidmodel, freepages, fixload, credits, playsid };
    Type type = replace;
    juce::String path;            // path inside the collection ("/MUSICIANS/H/Hubbard_Rob/Commando.sid")
    juce::StringArray values;     // the lines that follow (destination, new title, ...)
};

// Reads an update script. Unknown or broken parts are reported in `warnings`, never silently dropped.
std::vector<HvscOp> parseHvscScript (const juce::String& text, juce::StringArray& warnings);

struct HvscApplyResult
{
    int moved = 0, replaced = 0, deleted = 0, patched = 0, missing = 0, errors = 0;
    juce::StringArray messages;
};

// Applies the operations to the collection root (the folder that holds DOCUMENTS, MUSICIANS, ...).
// Files that are overwritten, deleted or patched are copied to backupDir first (same relative path).
bool applyHvscScript (const juce::File& root, const std::vector<HvscOp>& ops, const juce::File& backupDir, HvscApplyResult& result,
                      const std::function<bool (int done, int total)>& progress = {}, const juce::File& stagingTop = {});

// Installs a downloaded update package (HVSC_Update_NN.7z / .zip) into the collection:
// extract → find the package folder (the one with DOCUMENTS/UpdateNN.hvs) → copy it over the collection (overwritten files are backed up)
// → run the script → remove the "update" staging folder. progress(stage text, fraction 0..1) returns false to cancel.
struct HvscInstallResult
{
    HvscApplyResult applied;
    int copiedFiles = 0;
    bool ok = false;
    bool cancelled = false;
    juce::String error;
    juce::StringArray warnings;
};
HvscInstallResult installHvscPackage (const juce::File& root, int version, const juce::File& packageFile, const juce::File& backupDir,
                                      const std::function<bool (const juce::String& stage, double fraction)>& progress);

// Test helper: compares the header patches of a script with the actual files (the script of the installed version must match).
int verifyHvscHeaders (const juce::File& root, const std::vector<HvscOp>& ops, juce::StringArray& mismatches, int* checked = nullptr);

class HvscOnline
{
public:
    static constexpr int kFirstSevenZip = 75;                           // updates before #75 are .zip
    static juce::StringArray mirrors();                                 // base URLs with a trailing slash
    static juce::String updateFileName (int version);                   // "HVSC_Update_85.7z"
    static juce::String fullFileName (int version);                     // "HVSC_85-all-of-them.7z"
    // size of the file on the first mirror that has it (HEAD request), -1 if no mirror has it
    static juce::int64 remoteSize (const juce::String& fileName, juce::String* usedUrl = nullptr, int timeoutMs = 8000);
    // newest published update number >= localVersion (probes localVersion+1, +2 ...), 0 = no connection
    static int latestVersion (int localVersion, const std::function<bool()>& cancel = {});
    // downloads to target (partial file is deleted on failure); progress(done, total) returns false to cancel
    static bool download (const juce::String& fileName, const juce::File& target, const std::function<bool (juce::int64, juce::int64)>& progress, juce::String& error);
};
} // namespace sm
