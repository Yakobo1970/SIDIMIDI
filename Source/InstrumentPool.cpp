#include "InstrumentPool.h"
#include "Lang.h"
#include <set>
#include <algorithm>

namespace sm
{
namespace InstrumentPool
{
namespace
{
using Step = InstrumentPreset::Step;
using Set = InstrumentPreset::Set;
enum { wTri = 0x11, wSaw = 0x21, wPulse = 0x41, wNoise = 0x81 };

InstrumentPreset::ArpProg makeProg (const std::vector<std::pair<int, int>>& steps, int loopTo)
{
    InstrumentPreset::ArpProg p;
    for (auto& s : steps) p.rows.push_back ({ (juce::uint8) s.first, (juce::uint8) s.second });
    p.rows.push_back ({ 0x7f, (juce::uint8) loopTo });
    return p;
}

// pulse programs (v18 / v20: start, speed, duration, next set - "next" is an offset, 4 per set): set 0 starts the pulse, sets 1 and 2 swing
void swing (InstrumentPreset& p, int start, int speed, int dur)
{
    p.pulseRows = { 0, 1, 2 };
    p.pulseSets = { Set { (juce::uint8) start, (juce::uint8) speed, (juce::uint8) dur, 0x04 }, Set { 0xff, (juce::uint8) speed, (juce::uint8) (dur | 0x80), 0x08 }, Set { 0xff, (juce::uint8) speed, (juce::uint8) dur, 0x04 } };
}

InstrumentPreset make (const char* name, const char* cat, int ad, int sr, const std::vector<std::pair<int, int>>& steps, int loopTo, int hf = 0x80)
{
    InstrumentPreset p;
    p.name = juce::String::fromUTF8 (name); p.category = juce::String::fromUTF8 (cat); p.factory = true;
    p.ins = { (juce::uint8) ad, (juce::uint8) sr, (juce::uint8) hf, 0, 0, 0, 0, 0 };
    p.arpOn = makeProg (steps, loopTo);
    p.arpShared = true;
    return p;
}

InstrumentPreset single (const char* n, const char* c, int ad, int sr, int wave) { return make (n, c, ad, sr, { { 0, wave } }, 0); }

InstrumentPreset chord (const char* n, int ad, int sr, int wave, std::initializer_list<int> intervals, int hf = 0x80)
{
    std::vector<std::pair<int, int>> steps;
    for (int i : intervals) steps.push_back ({ i, wave });
    return make (n, "Arpeggios", ad, sr, steps, 0, hf);
}

// drums: fixed notes ($80 + hertz table index) for a few frames, then silence (the program stays on its last step)
InstrumentPreset drum (const char* n, const char* c, int ad, int sr, std::initializer_list<int> notes, int wave, int noiseFrom = -1, int noiseWave = wNoise)
{
    std::vector<std::pair<int, int>> steps;
    int i = 0;
    for (int note : notes) { steps.push_back ({ 0x80 + note, noiseFrom >= 0 && i >= noiseFrom ? noiseWave : wave }); ++i; }
    steps.push_back ({ 0x00, 0x00 });
    return make (n, c, ad, sr, steps, (int) steps.size() - 1);
}

InstrumentPreset noiseHit (const char* n, const char* c, int note, int frames)
{
    std::vector<std::pair<int, int>> steps;
    for (int i = 0; i < frames; ++i) steps.push_back ({ 0x80 + note, wNoise });
    steps.push_back ({ 0x00, 0x00 });
    return make (n, c, 0x00, 0xf0, steps, (int) steps.size() - 1);
}

InstrumentPreset noiseSweep (const char* n, const char* c, int from, int step, int frames)
{
    std::vector<std::pair<int, int>> steps;
    for (int i = 0; i < frames; ++i) steps.push_back ({ 0x80 + from - i * step, wNoise });
    steps.push_back ({ 0x00, 0x00 });
    return make (n, c, 0x00, 0xf0, steps, (int) steps.size() - 1);
}

juce::var toVar (const InstrumentPreset& p)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", p.name);
    o->setProperty ("category", p.category);
    juce::Array<juce::var> ins;
    for (auto b : p.ins) ins.add ((int) b);
    o->setProperty ("ins", ins);
    auto prog = [] (const InstrumentPreset::ArpProg& a)
    {
        auto* po = new juce::DynamicObject();
        po->setProperty ("start", a.start);
        juce::Array<juce::var> r;
        for (auto& s : a.rows) { r.add ((int) s[0]); r.add ((int) s[1]); }
        po->setProperty ("rows", r);
        return juce::var (po);
    };
    o->setProperty ("arpOn", prog (p.arpOn));
    o->setProperty ("arpShared", p.arpShared);
    if (! p.arpShared) o->setProperty ("arpOff", prog (p.arpOff));
    auto sets = [] (const std::vector<int>& rows, const std::vector<InstrumentPreset::Set>& s)
    {
        juce::Array<juce::var> a;
        for (size_t i = 0; i < s.size(); ++i) { a.add (rows[i]); for (auto b : s[i]) a.add ((int) b); }
        return juce::var (a);
    };
    o->setProperty ("pulse", sets (p.pulseRows, p.pulseSets));
    o->setProperty ("filter", sets (p.filterRows, p.filterSets));
    return juce::var (o);
}

bool fromVar (const juce::var& v, InstrumentPreset& p)
{
    if (! v.isObject()) return false;
    p = {};
    p.name = v["name"].toString();
    p.category = v["category"].toString();
    auto* ins = v["ins"].getArray();
    if (p.name.isEmpty() || ins == nullptr || ins->size() != 8) return false;
    for (int i = 0; i < 8; ++i) p.ins[(size_t) i] = (juce::uint8) (int) (*ins)[i];
    auto prog = [] (const juce::var& pv, InstrumentPreset::ArpProg& a)
    {
        a.start = (int) pv["start"];
        if (auto* r = pv["rows"].getArray())
            for (int i = 0; i + 1 < r->size(); i += 2) a.rows.push_back ({ (juce::uint8) (int) (*r)[i], (juce::uint8) (int) (*r)[i + 1] });
    };
    prog (v["arpOn"], p.arpOn);
    p.arpShared = (bool) v["arpShared"];
    if (! p.arpShared) prog (v["arpOff"], p.arpOff);
    auto sets = [] (const juce::var& sv, std::vector<int>& rows, std::vector<InstrumentPreset::Set>& s)
    {
        if (auto* a = sv.getArray())
            for (int i = 0; i + 4 < a->size(); i += 5)
            {
                rows.push_back ((int) (*a)[i]);
                s.push_back ({ (juce::uint8) (int) (*a)[i + 1], (juce::uint8) (int) (*a)[i + 2], (juce::uint8) (int) (*a)[i + 3], (juce::uint8) (int) (*a)[i + 4] });
            }
    };
    sets (v["pulse"], p.pulseRows, p.pulseSets);
    sets (v["filter"], p.filterRows, p.filterSets);
    return ! p.arpOn.rows.empty() && (p.arpShared || ! p.arpOff.rows.empty());
}

juce::File poolFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SIDIMIDI").getChildFile ("instrument-pool.json");
}

// ---- reading programs from a tune
InstrumentPreset::ArpProg walkArp (const NpTune& t, int start)
{
    InstrumentPreset::ArpProg p;
    p.start = start;
    for (int r = start; r < 256 && p.rows.size() < 64; ++r)
    {
        const auto a = t.get (NpTune::tabArpeggio, r, 0), w = t.get (NpTune::tabArpeggio, r, 1);
        p.rows.push_back ({ a, w });
        if (a == 0x7f) break;
    }
    return p;
}

void walkSets (const NpTune& t, NpTune::Table table, int ptr, std::vector<int>& rows, std::vector<Set>& sets)
{
    if (ptr <= 0 || ptr % 4 != 0) return;
    std::set<int> seen;
    int row = ptr / 4;
    while (row > 0 && row < 64 && ! seen.count (row) && seen.size() < 32)
    {
        seen.insert (row);
        const auto nxt = t.get (table, row, 3);
        if (nxt % 4 != 0 || nxt == 0 || nxt / 4 >= 64) break;
        row = nxt / 4;
    }
    // the pulse table may start at set 0 (pointer 0 means set 0 for the pulse table): handled by the caller
    for (int r : seen) { rows.push_back (r); sets.push_back ({ t.get (table, r, 0), t.get (table, r, 1), t.get (table, r, 2), t.get (table, r, 3) }); }
}

// ---- placing programs into a tune
int topRow (const NpTune& t, NpTune::Table table, int rows, int cols)
{
    for (int r = rows - 1; r >= 0; --r)
        for (int c = 0; c < cols; ++c)
            if (t.get (table, r, c) != 0) return r + 1;
    return 0;
}

int placeArp (NpTune& t, const InstrumentPreset::ArpProg& prog, bool write)
{
    const int n = (int) prog.rows.size();
    auto relocated = [&] (int s)
    {
        std::vector<Step> out = prog.rows;
        for (auto& st : out)
            if (st[0] == 0x7f && st[1] >= prog.start && st[1] < prog.start + n) st[1] = (juce::uint8) (st[1] - prog.start + s);
        return out;
    };
    for (int s = 0; s + n <= 256; ++s)            // the same program may be there already
    {
        const auto rel = relocated (s);
        bool same = true;
        for (int i = 0; i < n && same; ++i) same = t.get (NpTune::tabArpeggio, s + i, 0) == rel[(size_t) i][0] && t.get (NpTune::tabArpeggio, s + i, 1) == rel[(size_t) i][1];
        if (same) return s;
    }
    const int s = std::max (topRow (t, NpTune::tabArpeggio, 256, 2), 0);
    if (s + n > 256) return -1;
    if (write)
    {
        const auto rel = relocated (s);
        for (int i = 0; i < n; ++i) { t.set (NpTune::tabArpeggio, s + i, 0, rel[(size_t) i][0]); t.set (NpTune::tabArpeggio, s + i, 1, rel[(size_t) i][1]); }
    }
    return s;
}

int startIndex (const std::vector<int>& rows, int startRow)
{
    for (size_t j = 0; j < rows.size(); ++j) if (rows[j] == startRow) return (int) j;
    return 0;
}

// returns the new row of the first set (or -1: no room)
int placeSets (NpTune& t, NpTune::Table table, const std::vector<int>& rows, const std::vector<Set>& sets, int firstRowAllowed, int startRow, bool write)
{
    const int n = (int) sets.size();
    auto relocated = [&] (int s)
    {
        std::vector<Set> out = sets;
        for (auto& st : out)
        {
            const int nxt = st[3];
            if (nxt % 4 == 0)
                for (int j = 0; j < n; ++j) if (rows[(size_t) j] == nxt / 4) { st[3] = (juce::uint8) ((s + j) * 4); break; }
        }
        return out;
    };
    for (int s = firstRowAllowed; s + n <= 64; ++s)
    {
        const auto rel = relocated (s);
        bool same = true;
        for (int i = 0; i < n && same; ++i)
            for (int c = 0; c < 4 && same; ++c) same = t.get (table, s + i, c) == rel[(size_t) i][(size_t) c];
        if (same) return s + startIndex (rows, startRow);
    }
    const int s = std::max (topRow (t, table, 64, 4), firstRowAllowed);
    if (s + n > 64) return -1;
    if (write)
    {
        const auto rel = relocated (s);
        for (int i = 0; i < n; ++i) for (int c = 0; c < 4; ++c) t.set (table, s + i, c, rel[(size_t) i][(size_t) c]);
    }
    return s + startIndex (rows, startRow);
}

bool applyImpl (NpTune& t, int instrument, const InstrumentPreset& p, juce::String& error, bool write)
{
    auto ins = p.ins;
    const int on = placeArp (t, p.arpOn, write);
    const int off = p.arpShared ? on : placeArp (t, p.arpOff, write);
    if (on < 0 || off < 0) { error = T ("Die Arpeggio-Tabelle ist voll"); return false; }
    ins[6] = (juce::uint8) on; ins[7] = (juce::uint8) off;
    if (! p.pulseSets.empty())
    {
        const int start = placeSets (t, NpTune::tabPulse, p.pulseRows, p.pulseSets, 0, p.ins[5] / 4, write);
        if (start < 0) { error = T ("Die Puls-Tabelle ist voll"); return false; }
        ins[5] = (juce::uint8) (start * 4);
    }
    else ins[5] = 0;
    if (! p.filterSets.empty())
    {
        const int start = placeSets (t, NpTune::tabFilter, p.filterRows, p.filterSets, 1, p.ins[4] / 4, write);
        if (start < 0) { error = T ("Die Filter-Tabelle ist voll"); return false; }
        ins[4] = (juce::uint8) (start * 4);
    }
    else ins[4] = 0;
    if (write) for (int c = 0; c < 8; ++c) t.set (NpTune::tabInstrument, instrument, c, ins[(size_t) c]);
    return true;
}
} // namespace

std::string keyOf (const InstrumentPreset& p) { return (p.category + "|" + p.name + (p.factory ? "|f" : "|u")).toStdString(); }

bool find (const std::string& key, InstrumentPreset& out)
{
    const bool factory = key.size() > 2 && key.compare (key.size() - 2, 2, "|f") == 0;
    for (auto& p : factory ? factoryPresets() : userPresets())
        if (keyOf (p) == key) { out = p; return true; }
    return false;
}

std::string keyOf (const DrumKit& k) { return std::string (k.factory ? "f|" : "u|") + k.name.toStdString(); }

std::vector<DrumKit> factoryKits()
{
    const auto presets = factoryPresets();
    auto key = [&] (const char* name)
    {
        for (auto& p : presets) if (p.name == juce::String::fromUTF8 (name)) return keyOf (p);
        return std::string();
    };
    auto kit = [&] (const char* name, const char* k, const char* s, const char* t, const char* c, const char* h, const char* o)
    {
        DrumKit d;
        d.name = juce::String::fromUTF8 (name); d.factory = true;
        d.keys = { key (k), key (s), key (t), key (c), key (h), key (o) };
        return d;
    };
    return {
        kit ("Punchy", "Kick", "Snare knackig", "Tom mittel", "Becken", "Hi-Hat kurz", "Hi-Hat halb offen"),
        kit ("808", "Kick 808", "Snare lang", "Tom tief", "Crash lang", "Hi-Hat zu", "Hi-Hat offen"),
        kit ("Hart", "Kick kurz", "Snare knackig", "Tom hoch", "Becken", "Hi-Hat kurz", "Hi-Hat zu"),
        kit ("Lang und weich", "Kick lang", "Snare lang", "Tom tief", "Crash lang", "Hi-Hat halb offen", "Hi-Hat offen"),
        kit ("Elektro", "Kick", "Klatsche", "Tom hoch", "Becken", "Hi-Hat zu", "Hi-Hat offen"),
        kit ("909", "Kick 909", "Clap", "Tom mittel", "Becken", "Hi-Hat zu", "Hi-Hat offen"),
        kit ("Industrial", "Kick hart", "Snare lang", "Tom tief", "Crash lang", "Hi-Hat winzig", "Hi-Hat halb offen"),
        kit ("Lo-Fi", "Kick kurz", "Klatsche", "Tom hoch", "Becken", "Shaker", "Hi-Hat zu"),
        kit ("Minimal", "Kick Click", "Rimshot", "Tom hoch", "Becken", "Hi-Hat winzig", "Hi-Hat kurz"),
        kit ("Disco", "Kick", "Snare lang", "Tom mittel", "Crash lang", "Hi-Hat zu", "Hi-Hat offen"),
    };
}

static juce::File kitFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SIDIMIDI").getChildFile ("drum-kits.json");
}

std::vector<DrumKit> userKits()
{
    std::vector<DrumKit> out;
    const auto v = juce::JSON::parse (kitFile());
    if (auto* a = v.getArray())
        for (auto& e : *a)
        {
            DrumKit k;
            k.name = e["name"].toString();
            if (auto* ks = e["keys"].getArray()) for (int i = 0; i < kNumDrumKinds && i < ks->size(); ++i) k.keys[(size_t) i] = (*ks)[i].toString().toStdString();
            if (k.name.isNotEmpty()) out.push_back (k);
        }
    return out;
}

bool saveUserKits (const std::vector<DrumKit>& list)
{
    juce::Array<juce::var> a;
    for (auto& k : list)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", k.name);
        juce::Array<juce::var> ks;
        for (auto& key : k.keys) ks.add (juce::String (key));
        o->setProperty ("keys", ks);
        a.add (juce::var (o));
    }
    const auto f = kitFile();
    f.getParentDirectory().createDirectory();
    return f.replaceWithText (juce::JSON::toString (juce::var (a)), false, false, "\n");
}

bool findKit (const std::string& key, DrumKit& out)
{
    const bool factory = key.size() > 1 && key[0] == 'f';
    for (auto& k : factory ? factoryKits() : userKits()) if (keyOf (k) == key) { out = k; return true; }
    return false;
}

bool supported (const NpTune& t) { const int v = t.playerVersion(); return v == 18 || v == 20; }

std::vector<InstrumentPreset> factoryPresets()
{
    std::vector<InstrumentPreset> l;
    // ---- leads
    l.push_back (single ("Säge", "Leads", 0x08, 0xa8, wSaw));
    { auto p = single ("Puls (PWM)", "Leads", 0x09, 0xa9, wPulse); swing (p, 0x0c, 0x40, 0x08); l.push_back (p); }
    { auto p = single ("Puls schnell (PWM)", "Leads", 0x08, 0xa8, wPulse); swing (p, 0x08, 0x90, 0x04); l.push_back (p); }
    { auto p = single ("Puls hohl", "Leads", 0x08, 0xa9, wPulse); swing (p, 0x04, 0x10, 0x10); l.push_back (p); }
    l.push_back (single ("Dreieck", "Leads", 0x08, 0xa6, wTri));
    l.push_back (single ("Weiche Säge", "Leads", 0x3a, 0xa9, wSaw));
    { auto p = make ("Oktav-Zittern", "Leads", 0x08, 0xa8, { { 0, wPulse }, { 12, wPulse } }, 0); swing (p, 0x0c, 0x40, 0x08); l.push_back (p); }
    // ---- basses
    { auto p = single ("Puls-Bass", "Bässe", 0x05, 0xa5, wPulse); swing (p, 0x0c, 0x40, 0x08); l.push_back (p); }
    l.push_back (single ("Säge-Bass", "Bässe", 0x05, 0xa5, wSaw));
    l.push_back (single ("Sub-Bass (Dreieck)", "Bässe", 0x06, 0xf6, wTri));
    { auto p = single ("Zupf-Bass", "Bässe", 0x02, 0x00, wPulse); swing (p, 0x08, 0x30, 0x06); l.push_back (p); }
    l.push_back (make ("Säge-Bass mit Klick", "Bässe", 0x04, 0xa4, { { 0, wNoise }, { 0, wSaw } }, 1));
    // ---- arpeggios
    l.push_back (chord ("Dur", 0x08, 0xa8, wSaw, { 0, 4, 7 }));
    l.push_back (chord ("Moll", 0x08, 0xa8, wSaw, { 0, 3, 7 }));
    l.push_back (chord ("Oktave", 0x08, 0xa8, wSaw, { 0, 12 }));
    l.push_back (chord ("Quinte und Oktave", 0x08, 0xa8, wSaw, { 0, 7, 12 }));
    l.push_back (chord ("Dur 7", 0x08, 0xa8, wSaw, { 0, 4, 7, 11 }));
    l.push_back (chord ("Moll 7", 0x08, 0xa8, wSaw, { 0, 3, 7, 10 }));
    l.push_back (chord ("Sus 4", 0x08, 0xa8, wSaw, { 0, 5, 7 }));
    { auto p = chord ("Dur auf und ab (Puls)", 0x09, 0xa9, wPulse, { 0, 4, 7, 12, 7, 4 }); swing (p, 0x0c, 0x40, 0x08); p.category = "Arpeggios"; l.push_back (p); }
    // ---- pads
    l.push_back (single ("Säge-Pad", "Pads", 0x9a, 0xf7, wSaw));
    { auto p = single ("Puls-Pad", "Pads", 0x8a, 0xf8, wPulse); swing (p, 0x0c, 0x20, 0x10); l.push_back (p); }
    l.push_back (single ("Dreieck-Pad", "Pads", 0x78, 0xf9, wTri));
    l.push_back (single ("Streicher", "Pads", 0xa9, 0xf8, wSaw));
    // ---- kicks
    l.push_back (drum ("Kick", "Kicks", 0x00, 0xf0, { 36, 30, 24, 20, 16, 12 }, wTri));
    l.push_back (drum ("Kick kurz", "Kicks", 0x00, 0xf0, { 34, 22, 12 }, wTri));
    l.push_back (drum ("Kick lang", "Kicks", 0x00, 0xf0, { 40, 34, 28, 24, 20, 16, 12, 10 }, wTri));
    l.push_back (drum ("Kick 808", "Kicks", 0x00, 0xf0, { 44, 36, 30, 26, 24, 22, 20, 19, 18, 17, 16, 15, 14, 13, 12, 12 }, wTri));
    // ---- snares
    l.push_back (drum ("Snare", "Snares", 0x00, 0xf0, { 44, 66, 64, 62, 60, 58 }, wTri, 1));
    l.push_back (drum ("Snare knackig", "Snares", 0x00, 0xf0, { 44, 70, 70, 64, 60 }, wTri, 1));
    l.push_back (drum ("Snare lang", "Snares", 0x00, 0xf0, { 44, 66, 64, 62, 60, 58, 56, 54, 52 }, wTri, 1));
    l.push_back (noiseSweep ("Klatsche", "Snares", 70, 2, 8));
    // ---- hi-hats
    l.push_back (noiseHit ("Hi-Hat zu", "Hi-Hats", 90, 2));
    l.push_back (noiseHit ("Hi-Hat kurz", "Hi-Hats", 92, 1));
    l.push_back (noiseHit ("Hi-Hat halb offen", "Hi-Hats", 88, 3));
    l.push_back (noiseHit ("Hi-Hat offen", "Hi-Hats", 88, 6));
    // ---- toms and cymbals
    l.push_back (drum ("Tom hoch", "Toms & Becken", 0x00, 0xf0, { 40, 38, 36, 34, 32, 30 }, wTri));
    l.push_back (drum ("Tom mittel", "Toms & Becken", 0x00, 0xf0, { 34, 32, 30, 28, 26, 24 }, wTri));
    l.push_back (drum ("Tom tief", "Toms & Becken", 0x00, 0xf0, { 28, 26, 24, 22, 20, 18 }, wTri));
    l.push_back (noiseSweep ("Becken", "Toms & Becken", 84, 1, 10));
    l.push_back (noiseSweep ("Crash lang", "Toms & Becken", 88, 1, 18));
    // ---- effects
    l.push_back (drum ("Zap abwärts", "Effekte", 0x00, 0xa8, { 60, 54, 48, 42, 36, 30, 24 }, wSaw));
    { auto p = drum ("Laser", "Effekte", 0x00, 0xa8, { 20, 26, 32, 38, 44, 50, 56, 62 }, wPulse); swing (p, 0x08, 0x40, 0x08); l.push_back (p); }
    l.push_back (make ("Sirene", "Effekte", 0x08, 0xf8, { { 0, wSaw }, { 0, wSaw }, { 7, wSaw }, { 7, wSaw } }, 0));
    // ---- more synths and leads
    l.push_back (single ("Säge-Orgel", "Leads", 0x00, 0xf8, wSaw));
    l.push_back (single ("Säge-Pluck", "Leads", 0x02, 0x00, wSaw));
    { auto p = single ("Puls-Pluck", "Leads", 0x03, 0x00, wPulse); swing (p, 0x08, 0x30, 0x04); l.push_back (p); }
    { auto p = single ("Puls schmal", "Leads", 0x08, 0xa8, wPulse); swing (p, 0x02, 0x20, 0x10); l.push_back (p); }
    { auto p = single ("Puls breit", "Leads", 0x08, 0xa8, wPulse); swing (p, 0x0a, 0x10, 0x20); l.push_back (p); }
    { auto p = single ("Puls-PWM langsam", "Leads", 0x09, 0xa9, wPulse); swing (p, 0x08, 0x10, 0x30); l.push_back (p); }
    l.push_back (single ("Dreieck-Flöte", "Leads", 0x38, 0xa8, wTri));
    { auto p = make ("Quinten-Lead", "Leads", 0x08, 0xa8, { { 0, wPulse }, { 7, wPulse } }, 0); swing (p, 0x0c, 0x40, 0x08); l.push_back (p); }
    { auto p = make ("Terz-Lead", "Leads", 0x08, 0xa8, { { 0, wPulse }, { 4, wPulse } }, 0); swing (p, 0x0c, 0x40, 0x08); l.push_back (p); }
    l.push_back (make ("Gate-Lead schnell", "Leads", 0x08, 0xa8, { { 0, wSaw }, { 0, wSaw }, { 0, 0 }, { 0, 0 } }, 0));
    l.push_back (make ("Gate-Lead langsam", "Leads", 0x08, 0xa8, { { 0, wSaw }, { 0, wSaw }, { 0, 0 }, { 0, 0 } }, 0, 0x81));
    l.push_back (make ("Zirp-Lead", "Leads", 0x08, 0xa8, { { 0, wSaw }, { 12, wSaw }, { 7, wSaw }, { 12, wSaw } }, 0));
    // ---- more basses
    { auto p = single ("Puls-Bass hart", "Bässe", 0x04, 0xa4, wPulse); swing (p, 0x06, 0x40, 0x06); l.push_back (p); }
    { auto p = single ("Puls-Bass breit", "Bässe", 0x05, 0xa5, wPulse); swing (p, 0x0a, 0x20, 0x10); l.push_back (p); }
    { auto p = single ("Puls-Bass Wobble", "Bässe", 0x05, 0xa5, wPulse); swing (p, 0x08, 0x90, 0x04); l.push_back (p); }
    l.push_back (single ("Säge-Bass Pluck", "Bässe", 0x02, 0x00, wSaw));
    l.push_back (single ("Sub-Bass kurz", "Bässe", 0x03, 0x00, wTri));
    { auto p = make ("Oktav-Bass", "Bässe", 0x05, 0xa5, { { 0, wPulse }, { 12, wPulse } }, 0); swing (p, 0x0c, 0x40, 0x08); l.push_back (p); }
    l.push_back (make ("Rollender Bass", "Bässe", 0x04, 0xa4, { { 0, wSaw }, { 0, wSaw }, { 12, wSaw }, { 0, wSaw } }, 0));
    l.push_back (make ("Gate-Bass", "Bässe", 0x05, 0xa5, { { 0, wSaw }, { 0, wSaw }, { 0, 0 }, { 0, 0 } }, 0));
    l.push_back (make ("Klick-Bass (Dreieck)", "Bässe", 0x05, 0xa6, { { 0, wNoise }, { 0, wTri } }, 1));
    // ---- more arpeggios
    l.push_back (chord ("Dur 9", 0x08, 0xa8, wSaw, { 0, 4, 7, 14 }));
    l.push_back (chord ("Moll 9", 0x08, 0xa8, wSaw, { 0, 3, 7, 14 }));
    l.push_back (chord ("Dur 6", 0x08, 0xa8, wSaw, { 0, 4, 7, 9 }));
    l.push_back (chord ("Moll 6", 0x08, 0xa8, wSaw, { 0, 3, 7, 9 }));
    l.push_back (chord ("Dominant 7", 0x08, 0xa8, wSaw, { 0, 4, 7, 10 }));
    l.push_back (chord ("Sus 4 mit 7", 0x08, 0xa8, wSaw, { 0, 5, 7, 10 }));
    l.push_back (chord ("Sus 2", 0x08, 0xa8, wSaw, { 0, 2, 7 }));
    l.push_back (chord ("Vermindert", 0x08, 0xa8, wSaw, { 0, 3, 6 }));
    l.push_back (chord ("Übermäßig", 0x08, 0xa8, wSaw, { 0, 4, 8 }));
    l.push_back (chord ("Power (Quinte)", 0x08, 0xa8, wSaw, { 0, 7 }));
    l.push_back (chord ("Quinte und Oktave 2", 0x08, 0xa8, wSaw, { 0, 7, 12, 19 }));
    l.push_back (chord ("Dur weit", 0x08, 0xa8, wSaw, { 0, 4, 7, 12, 16, 19 }));
    l.push_back (chord ("Moll weit", 0x08, 0xa8, wSaw, { 0, 3, 7, 12, 15, 19 }));
    l.push_back (chord ("Dur langsam", 0x08, 0xa8, wSaw, { 0, 4, 7 }, 0x82));
    l.push_back (chord ("Moll langsam", 0x08, 0xa8, wSaw, { 0, 3, 7 }, 0x82));
    l.push_back (chord ("Oktave langsam", 0x08, 0xa8, wSaw, { 0, 12 }, 0x81));
    l.push_back (chord ("Dur (Dreieck)", 0x08, 0xa6, wTri, { 0, 4, 7 }));
    l.push_back (chord ("Moll (Dreieck)", 0x08, 0xa6, wTri, { 0, 3, 7 }));
    { auto p = chord ("Moll auf und ab (Puls)", 0x09, 0xa9, wPulse, { 0, 3, 7, 12, 7, 3 }); swing (p, 0x0c, 0x40, 0x08); l.push_back (p); }
    { auto p = chord ("Dur Pluck (Puls)", 0x03, 0x00, wPulse, { 0, 4, 7 }); swing (p, 0x08, 0x30, 0x04); l.push_back (p); }
    // ---- more pads
    l.push_back (single ("Säge-Pad langsam", "Pads", 0xaa, 0xfa, wSaw));
    { auto p = single ("Puls-Pad breit", "Pads", 0x9a, 0xf9, wPulse); swing (p, 0x0a, 0x10, 0x30); l.push_back (p); }
    l.push_back (single ("Dreieck-Pad weich", "Pads", 0x98, 0xf8, wTri));
    { auto p = make ("Chor", "Pads", 0x9a, 0xf8, { { 0, wPulse }, { 0, wPulse }, { 12, wPulse } }, 0, 0x82); swing (p, 0x08, 0x10, 0x30); l.push_back (p); }
    l.push_back (single ("Dunkles Pad", "Pads", 0xa8, 0xf6, wSaw));
    l.push_back (make ("Glas", "Pads", 0x88, 0xf8, { { 0, wTri }, { 12, wTri } }, 0, 0x81));
    // ---- more drums
    l.push_back (drum ("Kick hart", "Kicks", 0x00, 0xf0, { 48, 36, 24, 16, 12 }, wTri));
    l.push_back (drum ("Kick 909", "Kicks", 0x00, 0xf0, { 44, 38, 32, 28, 24, 20, 16 }, wTri));
    l.push_back (make ("Kick Click", "Kicks", 0x00, 0xf0, { { 0x80 + 60, wNoise }, { 0x80 + 36, wTri }, { 0x80 + 28, wTri }, { 0x80 + 20, wTri }, { 0x80 + 14, wTri }, { 0, 0 } }, 5));
    l.push_back (make ("Rimshot", "Snares", 0x00, 0xf0, { { 0x80 + 52, wTri }, { 0x80 + 72, wNoise }, { 0, 0 } }, 2));
    l.push_back (make ("Clap", "Snares", 0x00, 0xf0, { { 0x80 + 62, wNoise }, { 0, 0 }, { 0x80 + 62, wNoise }, { 0, 0 }, { 0x80 + 60, wNoise }, { 0x80 + 58, wNoise }, { 0x80 + 56, wNoise }, { 0, 0 } }, 7));
    l.push_back (noiseHit ("Hi-Hat winzig", "Hi-Hats", 94, 1));
    l.push_back (noiseHit ("Shaker", "Hi-Hats", 86, 3));
    for (auto& p : l) p.factory = true;
    return l;
}

std::vector<InstrumentPreset> userPresets()
{
    std::vector<InstrumentPreset> out;
    const auto v = juce::JSON::parse (poolFile());
    if (auto* a = v.getArray())
        for (auto& e : *a) { InstrumentPreset p; if (fromVar (e, p)) out.push_back (p); }
    return out;
}

bool saveUserPresets (const std::vector<InstrumentPreset>& list)
{
    juce::Array<juce::var> a;
    for (auto& p : list) a.add (toVar (p));
    const auto f = poolFile();
    f.getParentDirectory().createDirectory();
    return f.replaceWithText (juce::JSON::toString (juce::var (a)), false, false, "\n");
}

InstrumentPreset capture (const NpTune& t, int instrument, const juce::String& name, const juce::String& category)
{
    InstrumentPreset p;
    p.name = name; p.category = category;
    for (int c = 0; c < 8; ++c) p.ins[(size_t) c] = t.get (NpTune::tabInstrument, instrument, c);
    p.arpOn = walkArp (t, p.ins[6]);
    p.arpShared = p.ins[7] == p.ins[6];
    if (! p.arpShared) p.arpOff = walkArp (t, p.ins[7]);
    // pulse pointer 0 means set 0 when that set holds something
    std::vector<int> rows; std::vector<Set> sets;
    {
        const int ptr = p.ins[5];
        if (ptr % 4 == 0)
        {
            std::set<int> seen;
            int row = ptr / 4;
            while (row >= 0 && row < 64 && ! seen.count (row) && seen.size() < 32)
            {
                seen.insert (row);
                const auto nxt = t.get (NpTune::tabPulse, row, 3);
                if (nxt % 4 != 0 || nxt / 4 >= 64) break;
                row = nxt / 4;
            }
            for (int r : seen) { rows.push_back (r); sets.push_back ({ t.get (NpTune::tabPulse, r, 0), t.get (NpTune::tabPulse, r, 1), t.get (NpTune::tabPulse, r, 2), t.get (NpTune::tabPulse, r, 3) }); }
        }
        // pointer 0 is also "nothing": only instruments that play the pulse waveform carry a pulse program
        bool usesPulse = false;
        for (auto& st : p.arpOn.rows) if (st[0] != 0x7f && (st[1] & 0x40)) usesPulse = true;
        for (auto& st : p.arpOff.rows) if (! p.arpShared && st[0] != 0x7f && (st[1] & 0x40)) usesPulse = true;
        bool allZero = true;
        for (auto& s : sets) for (auto b : s) if (b != 0) allZero = false;
        if (usesPulse && ! allZero) { p.pulseRows = rows; p.pulseSets = sets; }
    }
    walkSets (t, NpTune::tabFilter, p.ins[4], p.filterRows, p.filterSets);
    return p;
}

bool apply (NpTune& t, int instrument, const InstrumentPreset& p, juce::String& error)
{
    NpTune probe = t;                                                      // dry run on a copy: no room means no change at all
    if (! applyImpl (probe, instrument, p, error, true)) return false;
    return applyImpl (t, instrument, p, error, true);
}

void clear (NpTune& t, int instrument)
{
    for (int c = 0; c < 8; ++c) t.set (NpTune::tabInstrument, instrument, c, 0);
}
} // namespace InstrumentPool
} // namespace sm
