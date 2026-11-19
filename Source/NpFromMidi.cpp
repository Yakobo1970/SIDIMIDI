#include "NpFromMidi.h"
#include "MidiToSidCore.h"
#include "Lang.h"
#include <map>
#include <set>
#include <algorithm>
#include <cmath>

namespace sm
{
namespace
{
constexpr double kFps = 50.1245;

struct InNote { double start = 0, end = 0; int pitch = 60; };

struct InVoice
{
    std::vector<InNote> notes;
    int program = -1;
    int channel = -1;
    double avgPitch = 60;
    juce::String label;
    bool drums = false;
};

enum { wTri = 0x11, wSaw = 0x21, wPulse = 0x41, wNoise = 0x81 };

struct Timbre { int wave; int ad; int sr; bool pulse; const char* name; };
const Timbre kTri { wTri, 0x08, 0xa6, false, "tri" }, kSaw { wSaw, 0x06, 0xc5, false, "saw" }, kLead { wPulse, 0x09, 0xa9, true, "lead" },
             kBass { wPulse, 0x05, 0xa5, true, "bass" }, kPad { wSaw, 0x9a, 0xf7, false, "pad" };

Timbre pickTimbre (const InVoice& v)
{
    const int p = v.program;
    if (p >= 32 && p <= 39) return kBass;
    if ((p >= 80 && p <= 87) || (p >= 96 && p <= 103)) return kLead;
    if ((p >= 88 && p <= 95) || (p >= 48 && p <= 55)) return kPad;
    if (p >= 16 && p < 80) return kSaw;
    if (p < 0) { if (v.avgPitch < 50) return kBass; if (v.avgPitch > 76) return kLead; }
    return kTri;
}

// ---- drum kit: GM drum notes -> five sounds made of noise and short pitch sweeps (all as arpeggio programs, notes "locked" = $80 + hertz table index)
enum DrumKind { dKick = 0, dSnare, dTom, dCymbal, dHat, dOpenHat, dCount };

int drumKindOf (int pitch)
{
    if (pitch == 35 || pitch == 36) return dKick;
    if (pitch >= 37 && pitch <= 40) return dSnare;
    if (pitch == 41 || pitch == 43 || pitch == 45 || pitch == 47 || pitch == 48 || pitch == 50) return dTom;
    if (pitch == 46) return dOpenHat;
    if (pitch == 49 || pitch == 51 || pitch == 52 || pitch == 53 || pitch == 55 || pitch == 57 || pitch == 59) return dCymbal;
    return dHat;
}
int drumPriority (int kind) { static const int pr[] = { 5, 4, 3, 2, 1, 1 }; return pr[kind]; }

struct Builder
{
    NpTune& t;
    int nextArp = 0, nextIns = 1;
    std::map<juce::String, int> insMap;
    int chordShapes = 0;
    bool overflowIns = false, overflowArp = false;

    int addArp (const std::vector<std::pair<int, int>>& steps, int loopTo)
    {
        if (nextArp + (int) steps.size() + 1 > 256) { overflowArp = true; return 0; }
        const int start = nextArp;
        for (size_t i = 0; i < steps.size(); ++i) { t.set (NpTune::tabArpeggio, start + (int) i, 0, (juce::uint8) steps[i].first); t.set (NpTune::tabArpeggio, start + (int) i, 1, (juce::uint8) steps[i].second); }
        t.set (NpTune::tabArpeggio, start + (int) steps.size(), 0, 0x7f);
        t.set (NpTune::tabArpeggio, start + (int) steps.size(), 1, (juce::uint8) (start + loopTo));
        nextArp += (int) steps.size() + 1;
        return start;
    }

    // ADSR, hard restart on, no filter, pulse program 0, arpeggio program for gate on and off
    int instrument (const juce::String& key, int ad, int sr, int arpStart, int pulsePtr)
    {
        if (auto it = insMap.find (key); it != insMap.end()) return it->second;
        if (nextIns > 31) { overflowIns = true; return 1; }
        const int id = nextIns++;
        const juce::uint8 b[8] = { (juce::uint8) ad, (juce::uint8) sr, 0x80, 0x00, 0x00, (juce::uint8) pulsePtr, (juce::uint8) arpStart, (juce::uint8) arpStart };
        for (int c = 0; c < 8; ++c) t.set (NpTune::tabInstrument, id, c, b[c]);
        insMap[key] = id;
        return id;
    }

    int single (const Timbre& tb)
    {
        const juce::String key = juce::String ("s:") + tb.name;
        if (insMap.count (key)) return insMap[key];
        if (nextIns > 31) { overflowIns = true; return 1; }
        const int arp = addArp ({ { 0, tb.wave } }, 0);
        return instrument (key, tb.ad, tb.sr, arp, 0);
    }

    int chord (const Timbre& tb, const std::vector<int>& intervals)
    {
        juce::String key = juce::String ("c:") + tb.name + ":";
        for (int i : intervals) key << i << ",";
        if (insMap.count (key)) return insMap[key];
        // no room left (instruments or arpeggio rows): the chord is played as its lowest note
        if (nextIns > 31 || nextArp + (int) intervals.size() + 2 > 256) { overflowIns = overflowIns || nextIns > 31; overflowArp = overflowArp || nextIns <= 31; return single (tb); }
        std::vector<std::pair<int, int>> steps { { 0, tb.wave } };
        for (int i : intervals) steps.push_back ({ i, tb.wave });
        ++chordShapes;
        const int arp = addArp (steps, 0);
        return instrument (key, tb.ad, tb.sr, arp, 0);
    }

    int drum (int kind)
    {
        const juce::String key = "d:" + juce::String (kind);
        if (insMap.count (key)) return insMap[key];
        std::vector<std::pair<int, int>> s;
        auto lockedNoise = [&] (int n, int w) { s.push_back ({ 0x80 + n, w }); };
        switch (kind)
        {
            case dKick:    for (int n : { 36, 30, 24, 20, 16, 12 }) lockedNoise (n, wTri); break;
            case dSnare:   lockedNoise (44, wTri); for (int n : { 66, 64, 62, 60, 58 }) lockedNoise (n, wNoise); break;
            case dTom:     for (int n : { 40, 38, 36, 34, 32, 30 }) lockedNoise (n, wTri); break;
            case dCymbal:  for (int i = 0; i < 10; ++i) lockedNoise (84 - i, wNoise); break;
            case dOpenHat: for (int i = 0; i < 6; ++i) lockedNoise (88, wNoise); break;
            default:       lockedNoise (90, wNoise); lockedNoise (90, wNoise); break;
        }
        s.push_back ({ 0x00, 0x00 });      // silence; the program stays on this last step
        const int arp = addArp (s, (int) s.size() - 1);
        return instrument (key, 0x00, 0xf0, arp, 0);
    }
};

int noteOf (int pitch)
{
    int n = pitch - 12;     // transposition $80: note + 12 = MIDI pitch
    while (n < 1) n += 12;
    while (n > 95) n -= 12;
    return n;
}

// ---- one melodic voice -> rows
std::vector<NpRow> buildMelodic (const InVoice& v, double rowSec, int totalRows, Builder& b, int& chordCount)
{
    std::vector<NpRow> rows ((size_t) totalRows);
    const Timbre tb = pickTimbre (v);
    struct Slot { double start, end; std::vector<int> pitches; };
    auto notes = v.notes;
    std::sort (notes.begin(), notes.end(), [] (const InNote& a, const InNote& c) { return a.start < c.start; });
    std::vector<Slot> slots;
    for (auto& n : notes)
    {
        if (! slots.empty() && n.start - slots.back().start < rowSec * 0.6) { slots.back().pitches.push_back (n.pitch); slots.back().end = std::max (slots.back().end, n.end); }
        else slots.push_back ({ n.start, n.end, { n.pitch } });
    }
    for (size_t i = 0; i < slots.size(); ++i)
    {
        const int sr = (int) std::lround (slots[i].start / rowSec);
        int nextSr = totalRows;
        if (i + 1 < slots.size()) nextSr = (int) std::lround (slots[i + 1].start / rowSec);
        if (nextSr <= sr || sr < 0 || sr >= totalRows) continue;                       // two slots on one row: the later one wins
        const int er = std::min (nextSr, std::max (sr + 1, (int) std::lround (slots[i].end / rowSec)));
        auto p = slots[i].pitches;
        std::sort (p.begin(), p.end());
        p.erase (std::unique (p.begin(), p.end()), p.end());
        const int base = p.front();
        // a chord is described by its pitch classes above the lowest note (octaves do not matter for a fast arpeggio)
        std::set<int> classes;
        for (int q : p) if ((q - base) % 12 != 0) classes.insert ((q - base) % 12);
        int id;
        if (classes.empty()) id = b.single (tb);
        else
        {
            std::vector<int> iv (classes.begin(), classes.end());
            if (iv.size() > 3) iv = { iv[0], iv[iv.size() / 2], iv.back() };
            id = b.chord (tb, iv);
            ++chordCount;
        }
        rows[(size_t) sr] = { (juce::uint8) (0xa0 + id), (juce::uint8) noteOf (base) };
        for (int r = sr + 1; r < er; ++r) rows[(size_t) r] = { 0x80, 0x7e };
    }
    return rows;
}

std::vector<NpRow> buildDrums (const InVoice& v, double rowSec, int totalRows, Builder& b, int& hits)
{
    std::vector<NpRow> rows ((size_t) totalRows);
    std::vector<int> prio ((size_t) totalRows, 0);
    for (auto& n : v.notes)
    {
        const int r = (int) std::lround (n.start / rowSec);
        if (r < 0 || r >= totalRows) continue;
        const int kind = drumKindOf (n.pitch);
        if (drumPriority (kind) < prio[(size_t) r]) continue;
        prio[(size_t) r] = drumPriority (kind);
        rows[(size_t) r] = { (juce::uint8) (0xa0 + b.drum (kind)), 48 };
    }
    for (int r = 0; r < totalRows; ++r) if (prio[(size_t) r] > 0) ++hits;
    return rows;
}
} // namespace

NpImport npImportMidi (const juce::File& midi, const void* templateData, size_t templateSize, std::array<int, 3> parts)
{
    NpImport out;
    juce::String err;
    if (! out.tune.loadPrg (templateData, templateSize, err)) { out.report = err; return out; }

    // ---- which tracks play the three voices
    juce::String log;
    bool roundTrip = false;
    std::array<MidiToSidCoreEngine::ChannelSuggestion, 3> sug;
    if (! MidiToSidCoreEngine::analyzeMidiFile (midi, roundTrip, sug, log)) { out.report = T ("Die MIDI-Datei lässt sich nicht analysieren"); return out; }
    for (int v = 0; v < 3; ++v)
        if (parts[(size_t) v] == -2) parts[(size_t) v] = sug[(size_t) v].channel > 0 || sug[(size_t) v].trackIndex >= 0 ? sug[(size_t) v].partId() : -1;

    juce::FileInputStream in (midi);
    juce::MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in)) { out.report = T ("Die MIDI-Datei lässt sich nicht analysieren"); return out; }
    mf.convertTimestampTicksToSeconds();

    // tempo: the first tempo event decides the grid
    double secPerQuarter = 0.5;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        bool found = false;
        auto* tr = mf.getTrack (t);
        for (int i = 0; i < tr->getNumEvents() && ! found; ++i)
            if (tr->getEventPointer (i)->message.isTempoMetaEvent()) { secPerQuarter = tr->getEventPointer (i)->message.getTempoSecondsPerQuarterNote(); found = true; }
        if (found) break;
    }
    const double sixteenth = secPerQuarter / 4.0;
    const int speed = juce::jlimit (2, 6, (int) std::lround (sixteenth * kFps));
    const double rowSec = speed / kFps;

    std::array<InVoice, 3> voices;
    for (int v = 0; v < 3; ++v)
    {
        auto& iv = voices[(size_t) v];
        const int part = parts[(size_t) v];
        iv.label = part >= 0 ? sug[(size_t) v].label : juce::String();
        if (part < 0) continue;
        const int idx = part & 0xffff, chan = (part >> 16) & 0x1f;
        if (idx >= mf.getNumTracks()) continue;
        juce::MidiMessageSequence seq (*mf.getTrack (idx));
        seq.updateMatchedPairs();
        double sum = 0;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            auto* ev = seq.getEventPointer (i);
            const auto& m = ev->message;
            if (chan > 0 && m.getChannel() != chan) continue;
            if (m.isProgramChange() && iv.program < 0) iv.program = m.getProgramChangeNumber();
            else if (m.isNoteOn() && m.getVelocity() > 0)
            {
                if (iv.channel < 0) iv.channel = m.getChannel();
                InNote n;
                n.start = m.getTimeStamp();
                n.end = ev->noteOffObject != nullptr ? ev->noteOffObject->message.getTimeStamp() : n.start + 0.25;
                n.pitch = m.getNoteNumber();
                iv.notes.push_back (n);
                sum += n.pitch;
            }
        }
        if (! iv.notes.empty()) iv.avgPitch = sum / (double) iv.notes.size();
        iv.drums = iv.channel == 10;
    }

    double endTime = 0, firstStart = 1e9;
    int noteCount = 0;
    for (auto& v : voices) for (auto& n : v.notes) { endTime = std::max (endTime, n.end); firstStart = std::min (firstStart, n.start); ++noteCount; }
    if (noteCount == 0) { out.report = T ("Keine Spuren mit Noten gefunden"); return out; }
    // the tune begins with the first note (no silence in front)
    for (auto& v : voices) for (auto& n : v.notes) { n.start -= firstStart; n.end -= firstStart; }
    endTime -= firstStart;
    const int totalRows = (int) std::ceil (endTime / rowSec) + 2;

    // ---- instruments and rows
    NpTune& t = out.tune;
    t.makeStarter();                                    // pulse programs, track lists, speed
    for (int i = 0; i < 32; ++i) for (int c = 0; c < 8; ++c) t.set (NpTune::tabInstrument, i, c, 0);
    for (int i = 0; i < 256; ++i) { t.set (NpTune::tabArpeggio, i, 0, 0); t.set (NpTune::tabArpeggio, i, 1, 0); }
    Builder b { t };
    std::array<std::vector<NpRow>, 3> rows;
    int chords = 0, hits = 0;
    // sounds that every tune needs come first (drum kit, one instrument per timbre): chord shapes use the slots that are left
    for (auto& v : voices)
    {
        if (v.notes.empty()) continue;
        if (v.drums) for (auto& n : v.notes) b.drum (drumKindOf (n.pitch));
        else b.single (pickTimbre (v));
    }
    for (int v = 0; v < 3; ++v)
    {
        if (voices[(size_t) v].notes.empty()) { rows[(size_t) v].assign ((size_t) totalRows, NpRow()); continue; }
        rows[(size_t) v] = voices[(size_t) v].drums ? buildDrums (voices[(size_t) v], rowSec, totalRows, b, hits)
                                                    : buildMelodic (voices[(size_t) v], rowSec, totalRows, b, chords);
    }

    // ---- sequences: chunks of equal rows share a sequence; the chunk size grows until everything fits
    int chunk = 32, used = 0, rowsKept = totalRows;
    std::array<std::vector<int>, 3> trackSeq;
    for (int C : { 32, 48, 64, 96 })
    {
        chunk = C;
        std::map<std::string, int> known;
        std::vector<std::vector<NpRow>> seqs;
        std::array<std::vector<int>, 3> ts;
        bool fits = true;
        const int chunks = (totalRows + C - 1) / C;
        rowsKept = totalRows;
        for (int ci = 0; ci < chunks && fits; ++ci)
        {
            std::array<int, 3> ids {};
            std::vector<std::vector<NpRow>> pieces;
            int fresh = 0;
            std::array<std::string, 3> keys;
            for (int v = 0; v < 3; ++v)
            {
                std::vector<NpRow> piece (rows[(size_t) v].begin() + ci * C, rows[(size_t) v].begin() + std::min (totalRows, (ci + 1) * C));
                std::string key;
                for (auto& r : piece) { key.push_back ((char) r.cmd); key.push_back ((char) r.note); }
                keys[(size_t) v] = key;
                pieces.push_back (std::move (piece));
            }
            std::set<std::string> newKeys;
            for (auto& k : keys) if (known.find (k) == known.end()) newKeys.insert (k);
            fresh = (int) newKeys.size();
            if ((int) seqs.size() + fresh > NpTune::kMaxSequence)
            {
                if (C == 96) { rowsKept = ci * C; fits = true; break; }        // last resort: the tune is cut here
                fits = false;
                break;
            }
            for (int v = 0; v < 3; ++v)
            {
                auto it = known.find (keys[(size_t) v]);
                if (it == known.end()) { seqs.push_back (pieces[(size_t) v]); it = known.emplace (keys[(size_t) v], (int) seqs.size()).first; }
                ts[(size_t) v].push_back (it->second);
            }
        }
        if (! fits) continue;
        // write
        for (size_t i = 0; i < seqs.size(); ++i) t.setSequence ((int) i + 1, seqs[i]);
        trackSeq = ts;
        used = (int) seqs.size();
        break;
    }
    for (int v = 0; v < 3; ++v)
    {
        std::vector<NpTrackEntry> all ((size_t) NpTune::kMaxTrackEntries, NpTrackEntry { 0xff, 0 });
        for (size_t i = 0; i < trackSeq[(size_t) v].size() && i + 1 < all.size(); ++i) all[i] = { 0x80, (juce::uint8) trackSeq[(size_t) v][i] };
        t.setTrackAll (v, all);
    }
    t.setSpeed (speed);

    out.ok = true;
    juce::String r;
    r << T ("MIDI → NewPlayer: Tempo %1 BPM, Tune-Tempo %2 (eine Zeile = ein Sechzehntel)", juce::String (60.0 / secPerQuarter, 0), speed) << "\n";
    for (int v = 0; v < 3; ++v)
        r << T ("Stimme %1: %2", v + 1, voices[(size_t) v].label.isNotEmpty() ? voices[(size_t) v].label : T ("leer"))
          << (voices[(size_t) v].drums ? " – " + T ("Schlagzeug") : juce::String()) << "\n";
    r << T ("%1 Akkorde als Arpeggio, %2 Schlagzeug-Anschläge, %3 Instrumente, %4 Sequenzen", chords, hits, b.nextIns - 1, used);
    if (rowsKept < totalRows) r << "\n" << T ("Der Tune war zu lang für 112 Sequenzen und wurde nach %1 Zeilen gekürzt", rowsKept);
    if (b.overflowIns) r << "\n" << T ("Es gibt mehr als 31 Klangvarianten: die übrigen bekamen das erste Instrument");
    if (b.overflowArp) r << "\n" << T ("Die Arpeggio-Tabelle ist voll: manche Akkorde fehlen");
    out.report = r;
    (void) chunk;
    return out;
}
} // namespace sm
