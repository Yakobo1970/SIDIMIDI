#include "NpTune.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <cstring>

namespace sm
{
NpTune::NpTune() = default;

// ================================================================================================ files
bool NpTune::loadPrg (const void* data, size_t size, juce::String& err)
{
    isValid = false;
    const auto* d = (const juce::uint8*) data;
    if (size < 0x300) { err = juce::String::fromUTF8 ("Die Datei ist zu klein für einen NewPlayer-Tune"); return false; }
    const int la = d[0] | (d[1] << 8);
    if (la != 0x0f00) { err = juce::String::fromUTF8 ("Kein NewPlayer-Tune (Ladeadresse $") + juce::String::toHexString (la).toUpperCase() + juce::String::fromUTF8 (" statt $0F00)"); return false; }
    if (size - 2 > 0xa000) { err = juce::String::fromUTF8 ("Die Datei ist zu groß"); return false; }
    mem.fill (0);
    std::memcpy (mem.data() + la, d + 2, size - 2);
    load = (juce::uint16) la;
    endA = (juce::uint16) (la + size - 2);
    // plausibility: the pointers of the player must lie inside the image
    for (int zp : { zpInstr, zpV1, zpV2, zpV3, zpLoByt, zpHiByt, zpSlide, zpArp1, zpArp2, zpFilter, zpPulse, zpTpoin })
    {
        const int p = pointer (zp);
        if (p < 0x1000 || p >= endA + 0x4000) { err = juce::String::fromUTF8 ("Die Zeigertabelle des Players ist nicht gültig – das ist keine Datei des NewPlayer-Editors"); return false; }
    }
    if (mem[0x1000] == 0 && mem[0x1001] == 0) { err = juce::String::fromUTF8 ("Der Player fehlt"); return false; }
    isValid = true;
    return true;
}

bool NpTune::loadFile (const juce::File& f, juce::String& err)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb)) { err = juce::String::fromUTF8 ("Die Datei lässt sich nicht lesen"); return false; }
    return loadPrg (mb.getData(), mb.getSize(), err);
}

juce::MemoryBlock NpTune::toPrg() const
{
    juce::MemoryBlock mb;
    const int n = (int) endA - (int) load;
    mb.setSize ((size_t) n + 2);
    auto* p = (juce::uint8*) mb.getData();
    p[0] = (juce::uint8) (load & 0xff);
    p[1] = (juce::uint8) (load >> 8);
    std::memcpy (p + 2, mem.data() + load, (size_t) n);
    return mb;
}

bool NpTune::saveFile (const juce::File& f, juce::String& err) const
{
    const auto mb = toPrg();
    if (! f.replaceWithData (mb.getData(), mb.getSize())) { err = juce::String::fromUTF8 ("Schreiben nicht möglich"); return false; }
    return true;
}

juce::MemoryBlock NpTune::toPsid (const juce::String& title, const juce::String& author, const juce::String& released) const
{
    juce::MemoryBlock mb;
    const auto prg = toPrg();
    mb.setSize (0x7c + prg.getSize(), true);
    auto* h = (juce::uint8*) mb.getData();
    std::memcpy (h, "PSID", 4);
    auto be16 = [&] (int off, int v) { h[off] = (juce::uint8) (v >> 8); h[off + 1] = (juce::uint8) (v & 0xff); };
    be16 (4, 2);          // version
    be16 (6, 0x7c);       // data offset
    be16 (8, 0);          // load address: the first two data bytes
    be16 (10, 0x1000);    // init
    be16 (12, 0x1003);    // play
    be16 (14, 1);         // songs
    be16 (16, 1);         // start song
    be16 (0x76, 0x0004);  // flags: PAL
    auto put = [&] (int off, const juce::String& s)
    {
        const auto b = s.toStdString();    // Latin-1 would be nicer, ASCII is enough for the tracker
        for (size_t i = 0; i < 31 && i < b.size(); ++i) h[(size_t) off + i] = (juce::uint8) b[i];
    };
    put (0x16, title); put (0x36, author); put (0x56, released);
    std::memcpy (h + 0x7c, prg.getData(), prg.getSize());
    return mb;
}

// ================================================================================================ player
juce::String NpTune::playerName() const
{
    struct Known { const char* hash; const char* name; };
    static const Known known[] = {
        { "609082cd", "05.02" }, { "38029c55", "06.00" }, { "c84b514d", "06.01" }, { "d15432c1", "08.02" }, { "e6b2e640", "08.03" },
        { "6d910e3b", "09.01" }, { "e0cf2842", "10.G1" }, { "107d3a64", "13.G0" }, { "9682f884", "14.G0" }, { "a89bff6a", "15.G0" },
        { "1a63acc4", "15.G1" }, { "e5f9b503", "15.G5" }, { "b51b3ffe", "15.G6" }, { "90f6fa30", "17.G1" }, { "f5d778ad", "17.Q0" },
        { "86e312ab", "19.G1" }, { "ed3c3346", "20.G2" }, { "9efdd1b4", "20.G4" } };
    const auto h = juce::MD5 (mem.data() + 0x1040, 0xc0).toHexString().substring (0, 8);
    for (auto& k : known) if (h == k.hash) return k.name;
    return {};
}

int NpTune::playerVersion() const { return playerName().substring (0, 2).getIntValue(); }

int NpTune::speed() const { return mem[(size_t) pointer (zpTpoin) + 6]; }
void NpTune::setSpeed (int s) { mem[(size_t) pointer (zpTpoin) + 6] = (juce::uint8) juce::jlimit (0, 9, s); }

// ================================================================================================ tracks
int NpTune::trackBase (int voice) const { return pointer (voice == 0 ? zpV1 : voice == 1 ? zpV2 : zpV3); }

std::vector<NpTrackEntry> NpTune::trackAll (int voice) const
{
    std::vector<NpTrackEntry> v;
    const int a = trackBase (voice);
    for (int i = 0; i < kMaxTrackEntries; ++i) v.push_back ({ mem[(size_t) (a + 2 * i) & 0xffff], mem[(size_t) (a + 2 * i + 1) & 0xffff] });
    return v;
}

void NpTune::setTrackAll (int voice, const std::vector<NpTrackEntry>& e)
{
    const int a = trackBase (voice);
    for (int i = 0; i < kMaxTrackEntries; ++i)
    {
        const NpTrackEntry x = i < (int) e.size() ? e[(size_t) i] : NpTrackEntry { 0xff, 0 };
        mem[(size_t) (a + 2 * i)] = x.trans;
        mem[(size_t) (a + 2 * i + 1)] = x.seq;
    }
    extendEnd (a + 2 * kMaxTrackEntries);
}

std::vector<NpTrackEntry> NpTune::trackList (int voice, bool* endsWithEndMark) const
{
    std::vector<NpTrackEntry> v;
    const int a = trackBase (voice);
    if (endsWithEndMark != nullptr) *endsWithEndMark = false;
    for (int i = 0; i < kMaxTrackEntries; ++i)
    {
        const NpTrackEntry e { mem[(size_t) (a + 2 * i)], mem[(size_t) (a + 2 * i + 1)] };
        if (e.isMark()) { if (endsWithEndMark != nullptr) *endsWithEndMark = e.trans == 0xfe; break; }
        v.push_back (e);
    }
    return v;
}

int NpTune::trackListLength (int voice) const { return (int) trackList (voice).size(); }

void NpTune::setTrackEntry (int voice, int index, NpTrackEntry e)
{
    if (index < 0 || index >= kMaxTrackEntries) return;
    const int a = trackBase (voice) + 2 * index;
    mem[(size_t) a] = e.trans;
    mem[(size_t) a + 1] = e.seq;
}

void NpTune::insertTrackEntry (int voice, int index, NpTrackEntry e)
{
    auto all = trackAll (voice);
    if (index < 0 || index >= kMaxTrackEntries) return;
    all.insert (all.begin() + index, e);
    all.resize (kMaxTrackEntries);
    setTrackAll (voice, all);
}

void NpTune::deleteTrackEntry (int voice, int index)
{
    auto all = trackAll (voice);
    if (index < 0 || index >= kMaxTrackEntries) return;
    all.erase (all.begin() + index);
    all.push_back ({ 0xff, 0 });
    setTrackAll (voice, all);
}

// ================================================================================================ sequences
int NpTune::seqAddr (int n) const
{
    const int lo = mem[(size_t) pointer (zpLoByt)], hi = mem[(size_t) pointer (zpHiByt)];
    return ((hi + n) << 8) + ((lo + 2) & 0xff);
}

bool NpTune::sequenceUsable (int n) const
{
    if (n < 0 || n > kMaxSequence) return false;
    const int a = seqAddr (n);
    if (a < 0x1000 || a + 2 * kMaxRows + 4 > 0xffff) return false;
    for (int i = 0; i <= kMaxRows; ++i)
        if (mem[(size_t) (a + 2 * i)] == 0x7f) return i >= 1;
    return false;
}

std::vector<NpRow> NpTune::sequence (int n) const
{
    std::vector<NpRow> rows;
    if (! sequenceUsable (n)) return rows;
    const int a = seqAddr (n);
    for (int i = 0; i < kMaxRows && mem[(size_t) (a + 2 * i)] != 0x7f; ++i) rows.push_back ({ mem[(size_t) (a + 2 * i)], mem[(size_t) (a + 2 * i + 1)] });
    return rows;
}

bool NpTune::setSequence (int n, const std::vector<NpRow>& rows)
{
    if (n < 0 || n > kMaxSequence || rows.empty() || (int) rows.size() > kMaxRows) return false;
    const int a = seqAddr (n);
    if (a < 0x1000 || a + 2 * (int) rows.size() + 4 > 0xffff) return false;
    mem[(size_t) (a - 3)] = (juce::uint8) (2 * ((int) rows.size() - 1));
    mem[(size_t) (a - 2)] = 0x80;
    mem[(size_t) (a - 1)] = 0x80;
    for (size_t i = 0; i < rows.size(); ++i) { mem[(size_t) a + 2 * i] = rows[i].cmd; mem[(size_t) a + 2 * i + 1] = rows[i].note; }
    mem[(size_t) a + 2 * rows.size()] = 0x7f;
    mem[(size_t) a + 2 * rows.size() + 1] = 0;
    extendEnd (a + 2 * (int) rows.size() + 1);
    return true;
}

void NpTune::makeBlankSequence (int n, int rows)
{
    std::vector<NpRow> r ((size_t) juce::jlimit (1, kMaxRows, rows));
    setSequence (n, r);
}

std::vector<int> NpTune::usedSequences() const
{
    std::vector<int> used;
    for (int v = 0; v < 3; ++v)
        for (auto& e : trackList (v))
            if (std::find (used.begin(), used.end(), (int) e.seq) == used.end()) used.push_back (e.seq);
    std::sort (used.begin(), used.end());
    return used;
}

int NpTune::firstFreeSequence() const
{
    const auto used = usedSequences();
    for (int n = 1; n <= kMaxSequence; ++n)
        if (std::find (used.begin(), used.end(), n) == used.end()) return n;
    return -1;
}

// ================================================================================================ instruments and tables
int NpTune::tableCols (Table t) const { return t == tabInstrument ? 8 : t == tabArpeggio ? 2 : t == tabSlide ? 2 : 4; }
int NpTune::tableRows (Table t) const { return t == tabInstrument ? 32 : t == tabArpeggio ? 256 : t == tabSlide ? 128 : 64; }

juce::uint8 NpTune::get (Table t, int row, int col) const
{
    if (row < 0 || row >= tableRows (t) || col < 0 || col >= tableCols (t)) return 0;
    switch (t)
    {
        case tabInstrument: return mem[(size_t) pointer (zpInstr) + (size_t) row * 8 + (size_t) col];
        case tabArpeggio: return mem[(size_t) pointer (col == 0 ? zpArp1 : zpArp2) + (size_t) row];
        case tabPulse: return mem[(size_t) pointer (zpPulse) + (size_t) row * 4 + (size_t) col];
        case tabFilter: return mem[(size_t) pointer (zpFilter) + (size_t) row * 4 + (size_t) col];
        case tabSlide: return mem[(size_t) pointer (zpSlide) + (size_t) row * 2 + (size_t) col];
    }
    return 0;
}

void NpTune::set (Table t, int row, int col, juce::uint8 v)
{
    if (row < 0 || row >= tableRows (t) || col < 0 || col >= tableCols (t)) return;
    switch (t)
    {
        case tabInstrument: mem[(size_t) pointer (zpInstr) + (size_t) row * 8 + (size_t) col] = v; break;
        case tabArpeggio: mem[(size_t) pointer (col == 0 ? zpArp1 : zpArp2) + (size_t) row] = v; break;
        case tabPulse: mem[(size_t) pointer (zpPulse) + (size_t) row * 4 + (size_t) col] = v; break;
        case tabFilter: mem[(size_t) pointer (zpFilter) + (size_t) row * 4 + (size_t) col] = v; break;
        case tabSlide: mem[(size_t) pointer (zpSlide) + (size_t) row * 2 + (size_t) col] = v; break;
    }
}

void NpTune::makeStarter()
{
    auto arp = [this] (int row, int add, int wave) { set (tabArpeggio, row, 0, (juce::uint8) add); set (tabArpeggio, row, 1, (juce::uint8) wave); };
    arp (4, 0x00, 0x21); arp (5, 0x7f, 0x04);                                  // saw, held
    arp (6, 0x00, 0x41); arp (7, 0x7f, 0x06);                                  // pulse, held
    arp (10, 0x00, 0x81); arp (11, 0x00, 0x81); arp (12, 0x00, 0x00); arp (13, 0x7f, 0x0c);   // noise for two frames, then silence
    arp (14, 0x00, 0x21); arp (15, 0x04, 0x21); arp (16, 0x07, 0x21); arp (17, 0x7f, 0x0e);   // major chord, fast arpeggio
    // pulse programs (player v18 / v20: start, speed, duration, next set): a pulse that swings between two sets
    const juce::uint8 pulse[3][4] = { { 0x0c, 0x40, 0x08, 0x04 }, { 0xff, 0x40, 0x84, 0x08 }, { 0xff, 0x40, 0x04, 0x04 } };
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) set (tabPulse, r, c, pulse[r][c]);
    // instruments: AD, SR, hard restart, resonance/filter, filter pointer, pulse pointer, arpeggio on, arpeggio off
    const juce::uint8 ins[5][8] = {
        { 0x08, 0xa8, 0x80, 0x00, 0x00, 0x00, 0x04, 0x04 },    // 1 saw lead
        { 0x09, 0xa9, 0x80, 0x00, 0x00, 0x00, 0x06, 0x06 },    // 2 pulse lead
        { 0x05, 0xa5, 0x80, 0x00, 0x00, 0x00, 0x06, 0x06 },    // 3 bass
        { 0x04, 0x00, 0x80, 0x00, 0x00, 0x00, 0x0a, 0x0a },    // 4 drum
        { 0x08, 0xa8, 0x80, 0x00, 0x00, 0x00, 0x0e, 0x0e } };  // 5 chord
    for (int i = 0; i < 5; ++i) for (int c = 0; c < 8; ++c) set (tabInstrument, i + 1, c, ins[i][c]);
    for (int v = 0; v < 3; ++v)
    {
        makeBlankSequence (v + 1, 16);
        std::vector<NpTrackEntry> all (kMaxTrackEntries, NpTrackEntry { 0xff, 0 });
        all[0] = { 0x8c, (juce::uint8) (v + 1) };
        setTrackAll (v, all);
    }
    setSpeed (4);
}

// ================================================================================================ helpers
int NpTune::rowsOfEntry (const NpTrackEntry& e) const { return e.isMark() ? 0 : (int) sequence (e.seq).size(); }

double NpTune::estimateSeconds() const
{
    double best = 0;
    const int sp = std::max (1, speed());
    for (int v = 0; v < 3; ++v)
    {
        int rows = 0;
        for (auto& e : trackList (v)) rows += rowsOfEntry (e);
        best = std::max (best, rows * (double) sp / 50.1245);
    }
    return best;
}

NpTune NpTune::withStartIndex (const std::array<int, 3>& idx) const
{
    NpTune t (*this);
    for (int v = 0; v < 3; ++v)
    {
        auto all = trackAll (v);
        const int s = juce::jlimit (0, kMaxTrackEntries - 1, idx[(size_t) v]);
        std::vector<NpTrackEntry> shifted (all.begin() + s, all.end());
        t.setTrackAll (v, shifted);
    }
    return t;
}

// ================================================================================================ D64 images
namespace
{
const int kSectorsPerTrack[] = { 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 19, 19, 19, 19, 19, 19, 19, 18, 18, 18, 18, 18, 18, 17, 17, 17, 17, 17 };

int d64Offset (int track, int sector)
{
    if (track < 1 || track > 35) return -1;
    int s = 0;
    for (int t = 1; t < track; ++t) s += kSectorsPerTrack[t - 1];
    return (s + sector) * 256;
}

juce::String petscii (const juce::uint8* p, int n)
{
    juce::String s;
    for (int i = 0; i < n && p[i] != 0xa0; ++i)
        s += (p[i] >= 32 && p[i] < 127) ? juce::String::charToString ((juce::juce_wchar) p[i]) : juce::String ("_");
    return s.trim();
}
} // namespace

bool npD64List (const juce::File& image, std::vector<NpD64File>& out)
{
    juce::MemoryBlock mb;
    if (! image.loadFileAsData (mb) || mb.getSize() < 174848) return false;
    const auto* d = (const juce::uint8*) mb.getData();
    int t = 18, s = 1;
    std::vector<int> seen;
    while (t != 0 && seen.size() < 40)
    {
        const int o = d64Offset (t, s);
        if (o < 0 || o + 256 > (int) mb.getSize()) break;
        seen.push_back (o);
        for (int i = 0; i < 8; ++i)
        {
            const auto* e = d + o + i * 32;
            if ((e[2] & 0x8f) != 0x82) continue;       // closed PRG files only
            NpD64File f;
            f.name = petscii (e + 5, 16);
            f.track = e[3]; f.sector = e[4];
            f.blocks = e[30] | (e[31] << 8);
            out.push_back (f);
        }
        const int nt = d[o], ns = d[o + 1];
        if (nt == 0) break;
        t = nt; s = ns;
    }
    return true;
}

bool npD64Extract (const juce::File& image, const NpD64File& f, juce::MemoryBlock& data)
{
    juce::MemoryBlock mb;
    if (! image.loadFileAsData (mb) || mb.getSize() < 174848) return false;
    const auto* d = (const juce::uint8*) mb.getData();
    data.reset();
    int t = f.track, s = f.sector;
    for (int guard = 0; guard < 700 && t != 0; ++guard)
    {
        const int o = d64Offset (t, s);
        if (o < 0 || o + 256 > (int) mb.getSize()) return false;
        const int nt = d[o], ns = d[o + 1];
        if (nt == 0) { data.append (d + o + 2, (size_t) std::max (0, ns - 1)); return true; }
        data.append (d + o + 2, 254);
        t = nt; s = ns;
    }
    return false;
}
} // namespace sm
