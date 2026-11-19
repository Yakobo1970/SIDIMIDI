#include "Hvsc.h"
#include "SidInfo.h"
#include <functional>

namespace sm
{
juce::String readDocumentText (const juce::File& f)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb)) return {};
    auto* p = (const char*) mb.getData();
    size_t n = mb.getSize();
    if (n >= 3 && (juce::uint8) p[0] == 0xEF && (juce::uint8) p[1] == 0xBB && (juce::uint8) p[2] == 0xBF) { p += 3; n -= 3; }
    if (juce::CharPointer_UTF8::isValidString (p, (int) n))
        return juce::String::fromUTF8 (p, (int) n);
    return latin1String (p, n);
}

namespace
{
// iterate over the lines of a text without copying it line by line
template <typename F>
void forEachLine (const juce::String& text, F&& fn)
{
    auto p = text.getCharPointer();
    const auto end = p.findTerminatingNull();
    while (p < end)
    {
        auto lineStart = p;
        while (p < end && *p != '\n' && *p != '\r') ++p;
        if (! fn (juce::String (lineStart, p))) return;
        if (p < end && *p == '\r') ++p;
        if (p < end && *p == '\n') ++p;
    }
}
} // namespace

bool Hvsc::isRoot (const juce::File& dir)
{
    if (! dir.isDirectory()) return false;
    const auto docs = dir.getChildFile ("DOCUMENTS");
    if (! docs.isDirectory()) return false;
    const bool docFile = docs.getChildFile ("Songlengths.md5").existsAsFile() || docs.getChildFile ("STIL.txt").existsAsFile();
    return docFile && (dir.getChildFile ("MUSICIANS").isDirectory() || dir.getChildFile ("GAMES").isDirectory());
}

juce::File Hvsc::findRoot (const juce::File& dir)
{
    if (isRoot (dir)) return dir;
    auto up = dir;
    for (int i = 0; i < 6 && up.getParentDirectory() != up; ++i)
    {
        up = up.getParentDirectory();
        if (isRoot (up)) return up;
    }
    for (const auto& c : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findDirectories))
    {
        if (isRoot (c.getFile())) return c.getFile();
        for (const auto& c2 : juce::RangedDirectoryIterator (c.getFile(), false, "*", juce::File::findDirectories))
            if (isRoot (c2.getFile())) return c2.getFile();
    }
    return {};
}

int Hvsc::detectVersion (const juce::File& rootDir, juce::String* date)
{
    const auto docs = rootDir.getChildFile ("DOCUMENTS");
    int best = 0;
    for (const auto& f : juce::RangedDirectoryIterator (docs, false, "Update*.hvs", juce::File::findFiles))
    {
        const auto n = f.getFile().getFileNameWithoutExtension().fromFirstOccurrenceOf ("Update", false, true);
        if (n.containsOnly ("0123456789") && n.isNotEmpty()) best = std::max (best, n.getIntValue());
    }
    if (best == 0)
    {
        const auto t = readDocumentText (docs.getChildFile ("hv_sids.txt"));
        const auto r = t.fromFirstOccurrenceOf ("Release", false, true).trim();
        if (r.isNotEmpty() && juce::CharacterFunctions::isDigit (r[0])) best = r.getIntValue();
    }
    if (best > 0 && date != nullptr)
    {
        const auto f = docs.getChildFile ("Update" + juce::String (best).paddedLeft ('0', 2) + ".hvs");
        juce::FileInputStream in (f);
        if (in.openedOk())
            for (int i = 0; i < 8 && ! in.isExhausted(); ++i)
            {
                const auto line = in.readNextLine();
                if (line.startsWith ("# Date:")) { *date = line.fromFirstOccurrenceOf (":", false, false).trim(); break; }
            }
    }
    return best;
}

std::shared_ptr<Hvsc> Hvsc::open (const juce::File& rootDir)
{
    if (! isRoot (rootDir)) return nullptr;
    auto h = std::make_shared<Hvsc>();
    h->root = rootDir;
    h->version = detectVersion (rootDir, &h->versionDate);
    return h;
}

juce::String Hvsc::relPath (const juce::File& f) const
{
    const auto r = root.getFullPathName();
    const auto p = f.getFullPathName();
    if (! p.startsWithIgnoreCase (r)) return {};
    auto rel = p.substring (r.length()).replaceCharacter ('\\', '/');
    if (! rel.startsWithChar ('/')) rel = "/" + rel;
    return rel;
}

std::vector<float> Hvsc::parseLengthList (const juce::String& value)
{
    std::vector<float> out;
    juce::StringArray parts;
    parts.addTokens (value, " ", {});
    for (auto p : parts)
    {
        p = p.trim();
        if (p.isEmpty()) continue;
        p = p.upToFirstOccurrenceOf ("(", false, false);   // old-format attributes, just in case
        const auto min = p.upToFirstOccurrenceOf (":", false, false);
        const auto sec = p.fromFirstOccurrenceOf (":", false, false);
        if (! min.containsOnly ("0123456789") || min.isEmpty() || sec.isEmpty()) continue;
        out.push_back ((float) (min.getIntValue() * 60 + sec.getDoubleValue()));
    }
    return out;
}

void Hvsc::loadLengths (const juce::File& f)
{
    juce::FileInputStream in (f);
    if (! in.openedOk()) return;
    const auto text = in.readEntireStreamAsString();
    lengthsByMd5.reserve (60000);
    forEachLine (text, [this] (const juce::String& line)
    {
        if (line.isEmpty() || line[0] == ';' || line[0] == '[') return true;
        const int eq = line.indexOfChar ('=');
        if (eq != 32) return true;
        lengthsByMd5[line.substring (0, 32).toLowerCase().toStdString()] = parseLengthList (line.substring (33));
        return true;
    });
}

void Hvsc::loadStil (const juce::File& f)
{
    const auto text = readDocumentText (f);
    juce::String key, body;
    auto flush = [&]
    {
        if (key.isNotEmpty() && body.trim().isNotEmpty()) stilByPath[key] = body.trimEnd();
        key = {}; body = {};
    };
    forEachLine (text, [&] (const juce::String& line)
    {
        if (line.startsWithChar ('/') && (line.endsWithChar ('/') || line.endsWithIgnoreCase (".sid")))
        {
            flush();
            key = line.trim();
        }
        else if (line.startsWithChar ('#'))
        {
            if (key.isNotEmpty() && body.isEmpty()) key = {};   // a banner inside an entry's place
        }
        else if (key.isNotEmpty())
        {
            if (line.trim().isEmpty()) flush();
            else body << line << "\n";
        }
        return true;
    });
    flush();
}

void Hvsc::loadBugs (const juce::File& f)
{
    const auto text = readDocumentText (f);
    juce::String key, body;
    auto flush = [&] { if (key.isNotEmpty()) bugByPath[key] = body.trimEnd(); key = {}; body = {}; };
    forEachLine (text, [&] (const juce::String& line)
    {
        if (line.startsWithChar ('/') && line.endsWithIgnoreCase (".sid")) { flush(); key = line.trim(); }
        else if (line.startsWithChar ('#')) { flush(); }
        else if (key.isNotEmpty())
        {
            if (line.trim().isEmpty()) flush();
            else body << line.trim() << "\n";
        }
        return true;
    });
    flush();
}

juce::String Hvsc::normaliseName (const juce::String& s)
{
    juce::String out;
    for (auto c : s.toLowerCase())
        out << (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c) : juce::String (" "));
    auto words = juce::StringArray::fromTokens (out, " ", {});
    words.removeEmptyStrings();
    return words.joinIntoString (" ");
}

void Hvsc::loadMusicians (const juce::File& f)
{
    const auto text = readDocumentText (f);
    forEachLine (text, [this] (const juce::String& line)
    {
        const int dash = line.lastIndexOf (" - ");
        if (dash <= 0 || line.startsWithChar ('*') || line.contains ("....")) return true;
        Musician m;
        m.country = line.substring (dash + 3).trim();
        auto left = line.substring (0, dash).trim();
        const int slash = left.indexOf (" / ");
        if (slash >= 0) { m.group = left.substring (slash + 3).trim(); left = left.substring (0, slash).trim(); }
        const int lp = left.indexOfChar ('(');
        juce::String handle = left;
        if (lp > 0 && left.endsWithChar (')'))
        {
            handle = left.substring (0, lp).trim();
            m.name = left.substring (lp + 1, left.length() - 1).replace ("{", "(").replace ("}", ")").trim();
        }
        m.handle = handle;
        if (handle.isEmpty() || m.country.isEmpty() || m.country.length() > 40) return true;
        const auto k = normaliseName (handle.replace (",", " "));
        if (k.isNotEmpty() && musicians.count (k) == 0) musicians[k] = m;
        // "Surname, Firstname (Handle)" style names are also reachable by the folder naming scheme
        if (m.name.isNotEmpty())
        {
            const auto n = normaliseName (m.name.replace (",", " ").upToFirstOccurrenceOf ("(", false, false));
            if (n.isNotEmpty() && musicians.count (n) == 0) musicians[n] = m;
        }
        return true;
    });
}

void Hvsc::loadDocs (const std::function<bool()>& cancelled)
{
    const auto docs = root.getChildFile ("DOCUMENTS");
    loadLengths (docs.getChildFile ("Songlengths.md5"));
    if (cancelled && cancelled()) return;
    loadStil (docs.getChildFile ("STIL.txt"));
    if (cancelled && cancelled()) return;
    loadBugs (docs.getChildFile ("BUGlist.txt"));
    loadMusicians (docs.getChildFile ("Musicians.txt"));
    loaded = true;
}

const std::vector<float>* Hvsc::lengths (const juce::String& md5) const
{
    auto it = lengthsByMd5.find (md5.toLowerCase().toStdString());
    return it == lengthsByMd5.end() ? nullptr : &it->second;
}

juce::String Hvsc::stil (const juce::String& rel) const
{
    auto it = stilByPath.find (rel);
    return it == stilByPath.end() ? juce::String() : it->second;
}

juce::String Hvsc::stilDirectory (const juce::String& rel) const
{
    juce::String out;
    auto dir = rel.upToLastOccurrenceOf ("/", true, false);   // "/MUSICIANS/H/Hubbard_Rob/"
    while (dir.length() > 1)
    {
        auto it = stilByPath.find (dir);
        if (it != stilByPath.end()) out = it->second + (out.isEmpty() ? "" : "\n\n") + out;
        dir = dir.dropLastCharacters (1).upToLastOccurrenceOf ("/", true, false);
    }
    return out;
}

juce::String Hvsc::bug (const juce::String& rel) const
{
    auto it = bugByPath.find (rel);
    return it == bugByPath.end() ? juce::String() : it->second;
}

const Musician* Hvsc::musicianFor (const juce::String& rel) const
{
    // /MUSICIANS/H/Hubbard_Rob/Commando.sid -> "hubbard rob"
    auto parts = juce::StringArray::fromTokens (rel, "/", {});
    parts.removeEmptyStrings();
    if (parts.size() < 4 || parts[0] != "MUSICIANS") return nullptr;
    auto it = musicians.find (normaliseName (parts[2].replace ("_", " ")));
    return it == musicians.end() ? nullptr : &it->second;
}
} // namespace sm
