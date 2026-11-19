// SIDiMIDI – reads the header of PSID/RSID files (v1 … v4) exactly as described in HVSC's SID_file_format.txt.
#pragma once
#include <juce_core/juce_core.h>
#include <vector>

namespace sm
{
struct SidInfo
{
    bool valid = false;
    bool rsid = false;
    int version = 0;                 // 1 … 4
    int dataOffset = 0;
    juce::uint16 loadAddr = 0;       // as stored in the header (0 = first two data bytes)
    juce::uint16 initAddr = 0, playAddr = 0;
    int songs = 1, startSong = 1;
    juce::uint32 speed = 0;          // one bit per song: 0 = 50/60 Hz video, 1 = CIA 1 timer
    juce::String title, author, released;
    juce::uint16 flags = 0;
    int clock = 0;                   // 0 unknown, 1 PAL, 2 NTSC, 3 both
    int model = 0;                   // 0 unknown, 1 MOS6581, 2 MOS8580, 3 both
    int model2 = 0, model3 = 0;
    juce::uint8 startPage = 0, pageLength = 0, sid2 = 0, sid3 = 0;
    int sidCount = 1;                // 1 … 3 chips
    bool mus = false;                // Compute's Sidplayer (needs an external player)
    bool basicTune = false;          // RSID: starts from BASIC
    // data after the header (load address already removed)
    juce::uint16 effLoad = 0;
    std::vector<juce::uint8> code;
    juce::int64 fileSize = 0;
    juce::String md5;                // MD5 of the complete file (key of HVSC's Songlengths.md5)

    juce::uint16 effInit() const { return initAddr != 0 ? initAddr : effLoad; }
    bool cia (int song1) const { return song1 >= 1 && song1 <= 32 ? ((speed >> (song1 - 1)) & 1u) != 0 : ((speed >> 31) & 1u) != 0; }
    // video clock: PAL unless the tune is flagged NTSC-only
    bool ntsc() const { return clock == 2; }
    double frameRate() const { return ntsc() ? 60.0 : 50.0; }
    juce::String formatName() const;        // "PSID v2" / "RSID v3"
    juce::String modelName() const;         // "6581" / "8580" / "6581/8580" / ""
    juce::String clockName() const;         // "PAL" / "NTSC" / "PAL/NTSC" / ""
    int year() const;                       // first 4-digit year in "released", else 0
};

juce::String latin1String (const char* p, size_t n);   // ISO-8859-1 bytes to a string
SidInfo parseSidBytes (const void* data, size_t size, bool wantCode = true);
SidInfo parseSidFile (const juce::File& file, bool wantCode = true);
bool hasSidExtension (const juce::String& name);
juce::String formatTime (double seconds, bool withMillis = false);   // "3:57"
juce::String formatLong (double seconds);                            // "3:57" or "85 h 12 min" for sums
} // namespace sm
