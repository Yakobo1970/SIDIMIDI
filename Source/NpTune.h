// SIDiMIDI – a tune of JCH's NewPlayer editor ("worktune"): the memory image of the editor's music block
// ($0F00 and up). It holds the player code (init $1000, play $1003), the three track lists, the sequences,
// the instruments and the tables. The tracker edits this image; playing it is done by the 6502 emulation.
//
// Layout (see ED_TEXTS of the JCH package and the editor source):
//   $0F00 + zero-page address ($A0 … $EC) = pointer to the player's data of that kind (voicon, tpoin, instr, v1 …)
//   track list of a voice: 2 bytes per entry [$80 + transposition][sequence number]; $FF = wrap, $FE = end of the tune
//   sequence n: page hibyt[0] + n, offset lobyt[0] + 2;  [len][$80][$80] before it, then rows of 2 bytes [command][note],
//               closed by $7F.  command: $80 ---, $90 ***, $A0-$BF Ixx (instrument), $C0-$FF Sxx (slide / super command)
//               note: 0 rest, $7E +++ (hold), else the note number (12 per octave, 48 = C-4)
#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <vector>

namespace sm
{
struct NpRow
{
    juce::uint8 cmd = 0x80, note = 0;
    bool operator== (const NpRow& o) const { return cmd == o.cmd && note == o.note; }
};

struct NpTrackEntry
{
    juce::uint8 trans = 0x8c, seq = 0;   // $FF / $FE in trans = wrap / end
    bool isMark() const { return trans >= 0xfe; }
    bool operator== (const NpTrackEntry& o) const { return trans == o.trans && seq == o.seq; }
};

class NpTune
{
public:
    static constexpr int kMaxTrackEntries = 512;   // per voice (1 KB)
    static constexpr int kMaxSequence = 0x70;      // numbers 0 … $70 (0 is better kept as the "empty" model)
    static constexpr int kMaxRows = 96;

    enum Table { tabInstrument, tabArpeggio, tabPulse, tabFilter, tabSlide };

    NpTune();

    // ---- files
    bool loadPrg (const void* data, size_t size, juce::String& err);        // 2 bytes load address ($0F00) + data
    bool loadFile (const juce::File& f, juce::String& err);
    juce::MemoryBlock toPrg() const;
    bool saveFile (const juce::File& f, juce::String& err) const;
    juce::MemoryBlock toPsid (const juce::String& title, const juce::String& author, const juce::String& released) const;
    bool valid() const { return isValid; }

    // ---- player
    juce::String playerName() const;       // "17.G1", "20.G4" … or empty when unknown
    int playerVersion() const;             // 17, 20 … (0 = unknown)
    int speed() const;                     // tune speed (frames per row)
    void setSpeed (int s);

    // ---- tracks (per voice: the first tune; the rest of the table stays untouched)
    std::vector<NpTrackEntry> trackAll (int voice) const;                    // all 512 entries
    void setTrackAll (int voice, const std::vector<NpTrackEntry>& entries);
    std::vector<NpTrackEntry> trackList (int voice, bool* endsWithEndMark = nullptr) const;   // up to the first mark (not included)
    void setTrackEntry (int voice, int index, NpTrackEntry e);
    void insertTrackEntry (int voice, int index, NpTrackEntry e);            // shifts the rest down
    void deleteTrackEntry (int voice, int index);                            // shifts the rest up
    int trackListLength (int voice) const;                                   // entries before the first mark

    // ---- sequences
    bool sequenceUsable (int n) const;     // the page holds a well-formed sequence (rows and a $7F)
    std::vector<NpRow> sequence (int n) const;
    bool setSequence (int n, const std::vector<NpRow>& rows);               // false: empty, too long or out of range
    void makeBlankSequence (int n, int rows = 16);
    int firstFreeSequence() const;         // lowest number > 0 that no track list uses (and that is blank or unusable)
    std::vector<int> usedSequences() const;

    // ---- instruments and tables
    int tableCols (Table t) const;         // 8, 2, 4, 4, 2
    int tableRows (Table t) const;         // 32, 256, 64, 64, 128
    juce::uint8 get (Table t, int row, int col) const;
    void set (Table t, int row, int col, juce::uint8 v);

    // a new tune for player v20: five instruments (saw lead, pulse lead, bass, drum, chord), their arpeggio / pulse programs
    // and three empty sequences, one on each voice
    void makeStarter();

    // ---- helpers
    double estimateSeconds() const;        // length of the longest voice at the tune speed
    NpTune withStartIndex (const std::array<int, 3>& trackIndex) const;      // copy that begins at the given track entries
    int rowsOfEntry (const NpTrackEntry& e) const;                           // rows of the sequence the entry points to

    juce::uint16 pointer (int zpOffset) const { return (juce::uint16) (mem[0x0f00 + zpOffset] | (mem[0x0f01 + zpOffset] << 8)); }
    juce::uint8 peek (int addr) const { return mem[(size_t) addr & 0xffff]; }
    juce::uint16 loadAddr() const { return load; }
    juce::uint16 endAddr() const { return endA; }

    static const int zpTpoin = 0xa6, zpCredits = 0xa4, zpArp1 = 0xbc, zpArp2 = 0xbe, zpFilter = 0xc0, zpPulse = 0xc2,
                     zpInstr = 0xc4, zpV1 = 0xc6, zpV2 = 0xc8, zpV3 = 0xca, zpLoByt = 0xcc, zpHiByt = 0xce, zpSlide = 0xd0;

private:
    std::array<juce::uint8, 65536> mem {};
    juce::uint16 load = 0x0f00, endA = 0x0f00;
    bool isValid = false;

    int seqAddr (int n) const;
    int trackBase (int voice) const;
    void extendEnd (int addr) { if (addr + 1 > endA) endA = (juce::uint16) std::min (addr + 1, 0xffff); }
};

// "Directory" of a C64 disk image (.d64) – the worktunes of the JCH package come on such disks
struct NpD64File { juce::String name; int track = 0, sector = 0, blocks = 0; };
bool npD64List (const juce::File& image, std::vector<NpD64File>& out);
bool npD64Extract (const juce::File& image, const NpD64File& f, juce::MemoryBlock& data);
} // namespace sm
