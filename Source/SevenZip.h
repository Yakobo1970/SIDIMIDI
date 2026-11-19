// SIDiMIDI – reader for 7-Zip archives (.7z): LZMA, LZMA2 and "copy" coders, solid blocks, encoded headers.
// Self-contained (no 7-Zip or other library needed, same code on Windows and macOS).
// Not supported: encryption, BCJ2, multi-volume archives. Filters on top of LZMA (BCJ/Delta) are applied for small folders.
#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

namespace sm
{
class SevenZip
{
public:
    struct Entry
    {
        juce::String path;                 // with forward slashes
        bool isDir = false;
        juce::uint64 size = 0;
        juce::uint32 crc = 0;
        bool hasCrc = false;
        int folder = -1;                   // -1 = empty file / directory
        juce::uint32 attributes = 0;
    };

    // progress(bytesDone, bytesTotal, currentFile) – return false to cancel
    using Progress = std::function<bool (juce::uint64 done, juce::uint64 total, const juce::String& file)>;

    static bool list (const juce::File& archive, std::vector<Entry>& out, juce::String& error);
    // Extracts everything below destDir (creates folders). Files that a coder cannot decode are listed in `skipped`.
    static bool extractAll (const juce::File& archive, const juce::File& destDir, const Progress& progress, juce::String& error,
                            juce::StringArray* skipped = nullptr);
    // Extracts only files whose path matches the wildcard (e.g. "DOCUMENTS/*") into memory/destDir (used by the updater)
    static bool extractMatching (const juce::File& archive, const juce::File& destDir, const juce::String& wildcard, const Progress& progress, juce::String& error);

    static juce::uint32 crc32 (const void* data, size_t size, juce::uint32 crc = 0);
};
} // namespace sm
