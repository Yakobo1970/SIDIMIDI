#include "SidInfo.h"
#include <juce_cryptography/juce_cryptography.h>
#include <string>

namespace sm
{
juce::String latin1String (const char* p, size_t n)
{
    std::string utf8;
    utf8.reserve (n + n / 8);
    for (size_t i = 0; i < n; ++i)
    {
        const auto c = (juce::uint8) p[i];
        if (c < 0x80) utf8 += (char) c;
        else { utf8 += (char) (0xC0 | (c >> 6)); utf8 += (char) (0x80 | (c & 0x3F)); }
    }
    return juce::String::fromUTF8 (utf8.data(), (int) utf8.size());
}

namespace
{
inline juce::uint16 be16 (const juce::uint8* p) { return (juce::uint16) ((p[0] << 8) | p[1]); }

juce::String latin1 (const juce::uint8* p, int n)
{
    int len = 0;
    while (len < n && p[len] != 0) ++len;
    return latin1String ((const char*) p, (size_t) len).trim();
}
} // namespace

SidInfo parseSidBytes (const void* dataIn, size_t size, bool wantCode)
{
    SidInfo s;
    const auto* d = (const juce::uint8*) dataIn;
    s.fileSize = (juce::int64) size;
    if (size < 0x76) return s;
    if (std::memcmp (d, "PSID", 4) == 0) s.rsid = false;
    else if (std::memcmp (d, "RSID", 4) == 0) s.rsid = true;
    else return s;

    s.version = be16 (d + 4);
    if (s.version < 1 || s.version > 4) return s;
    s.dataOffset = be16 (d + 6);
    if (s.dataOffset < 0x76 || (size_t) s.dataOffset > size) return s;
    s.loadAddr = be16 (d + 8);
    s.initAddr = be16 (d + 10);
    s.playAddr = be16 (d + 12);
    s.songs = juce::jlimit (1, 256, (int) be16 (d + 14));
    s.startSong = be16 (d + 16);
    if (s.startSong < 1 || s.startSong > s.songs) s.startSong = 1;
    s.speed = ((juce::uint32) d[18] << 24) | ((juce::uint32) d[19] << 16) | ((juce::uint32) d[20] << 8) | d[21];
    s.title = latin1 (d + 22, 32);
    s.author = latin1 (d + 54, 32);
    s.released = latin1 (d + 86, 32);

    if (s.version >= 2 && size >= 0x7C)
    {
        s.flags = be16 (d + 0x76);
        s.mus = (s.flags & 1) != 0;
        s.basicTune = s.rsid && (s.flags & 2) != 0;
        s.clock = (s.flags >> 2) & 3;
        s.model = (s.flags >> 4) & 3;
        s.model2 = (s.flags >> 6) & 3;
        s.model3 = (s.flags >> 8) & 3;
        s.startPage = d[0x78];
        s.pageLength = d[0x79];
        s.sid2 = d[0x7A];
        s.sid3 = d[0x7B];
        auto valid = [] (juce::uint8 b) { return b != 0 && ((b >= 0x42 && b <= 0x7F) || (b >= 0xE0 && b <= 0xFE)) && (b & 1) == 0; };
        s.sidCount = 1 + (valid (s.sid2) ? 1 : 0) + (valid (s.sid3) ? 1 : 0);
    }

    // Data: if the header's load address is 0, the first two data bytes (little endian) are the load address.
    size_t pos = (size_t) s.dataOffset;
    s.effLoad = s.loadAddr;
    if (s.loadAddr == 0)
    {
        if (pos + 2 > size) return s;
        s.effLoad = (juce::uint16) (d[pos] | (d[pos + 1] << 8));
        pos += 2;
    }
    if (wantCode)
    {
        const size_t n = std::min<size_t> (size - pos, 65536);
        s.code.assign (d + pos, d + pos + n);
    }
    s.md5 = juce::MD5 (d, size).toHexString();
    s.valid = true;
    return s;
}

SidInfo parseSidFile (const juce::File& file, bool wantCode)
{
    juce::MemoryBlock mb;
    if (! file.loadFileAsData (mb)) return {};
    return parseSidBytes (mb.getData(), mb.getSize(), wantCode);
}

bool hasSidExtension (const juce::String& name) { return name.endsWithIgnoreCase (".sid"); }

juce::String SidInfo::formatName() const { return juce::String (rsid ? "RSID" : "PSID") + " v" + juce::String (version); }

juce::String SidInfo::modelName() const
{
    switch (model) { case 1: return "6581"; case 2: return "8580"; case 3: return "6581/8580"; default: return {}; }
}

juce::String SidInfo::clockName() const
{
    switch (clock) { case 1: return "PAL"; case 2: return "NTSC"; case 3: return "PAL/NTSC"; default: return {}; }
}

int SidInfo::year() const
{
    const auto s = released;
    for (int i = 0; i + 3 < s.length(); ++i)
    {
        bool ok = true;
        for (int k = 0; k < 4; ++k) ok = ok && juce::CharacterFunctions::isDigit (s[i + k]);
        if (ok && (i == 0 || ! juce::CharacterFunctions::isDigit (s[i - 1])) && (i + 4 >= s.length() || ! juce::CharacterFunctions::isDigit (s[i + 4])))
        {
            const int y = s.substring (i, i + 4).getIntValue();
            if (y >= 1975 && y <= 2100) return y;
        }
    }
    return 0;
}

juce::String formatLong (double seconds)
{
    if (seconds < 3600) return formatTime (seconds);
    const int total = (int) seconds;
    return juce::String (total / 3600) + " h " + juce::String ((total / 60) % 60).paddedLeft ('0', 2) + " min";
}

juce::String formatTime (double seconds, bool withMillis)
{
    if (seconds < 0) return {};
    const int total = (int) seconds;
    juce::String t = juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
    if (withMillis) t << "." << juce::String ((int) ((seconds - total) * 1000)).paddedLeft ('0', 3);
    return t;
}
} // namespace sm
