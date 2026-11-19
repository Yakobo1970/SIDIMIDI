#include "Tracker.h"
#include "MainComponent.h"
#include "SidTab.h"
#include "MidiTab.h"
#include "AudioPlayerEngine.h"
#include "SidInfo.h"
#include "SidTrace.h"
#include "SidToMidiCore.h"
#include "NpFromMidi.h"
#include "SidView.h"
#include <BinaryData.h>

namespace sm
{
using ui::font;
using ui::mono;

namespace
{
const char* const kNoteNames[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };

juce::String hex2 (int v) { return juce::String::toHexString (v & 0xff).paddedLeft ('0', 2).toUpperCase(); }

juce::String noteText (int n)
{
    if (n == 0) return "---";
    if (n == 0x7e) return "+++";
    if (n >= 1 && n < 96) return juce::String (kNoteNames[n % 12]) + juce::String (n / 12);
    return "?" + hex2 (n);
}

juce::String cmdText (int c)
{
    if (c == 0x80) return "---";
    if (c == 0x90) return "***";
    if (c >= 0xa0 && c < 0xc0) return "I" + hex2 (c - 0xa0);
    if (c >= 0xc0) return "S" + hex2 (c - 0xc0);
    return "?" + hex2 (c);
}

int hexDigit (juce::juce_wchar c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// the keys Q2W3ER5T6Y7U I9O0P are a piano keyboard, as in the editor of JCH
int pianoKey (juce::juce_wchar c)
{
    static const char keys[] = "q2w3er5t6y7ui9o0p";
    if (c == 'z' || c == 'Z') c = 'y';
    for (int i = 0; keys[i] != 0; ++i) if (c == (juce::juce_wchar) keys[i] || c == (juce::juce_wchar) (keys[i] - 32 * (keys[i] >= 'a'))) return i;
    return -1;
}

// ---- what the bytes of the tables mean, per player version
struct TableInfo { juce::StringArray cols; juce::StringArray help; juce::String title; };

TableInfo tableInfo (NpTune::Table t, int ver)
{
    TableInfo ti;
    switch (t)
    {
        case NpTune::tabInstrument:
            ti.title = T ("Instrumente (8 Byte)");
            if (ver == 14 || ver == 13)
            {
                ti.cols = { "AD", "SR", T ("VibBr"), T ("VibTp"), T ("HF/Fi"), T ("Filter"), T ("Puls"), T ("Arp") };
                ti.help = { T ("Attack / Decay"), T ("Sustain / Release"), T ("Vibrato-Breite (oben) / Vibrato-Verzögerung (unten)"), T ("Vibrato-Tempo (oben) / Hard-Restart-Zeit (unten, 0 = aus, höchstens das Tune-Tempo, normal 2)"),
                             T ("HiFreq-Modus (oben, 1 = an) / Filter-Passband und Filter an (unten, 0 = aus)"), T ("Zeiger in die Filter-Tabelle (in Viererschritten)"), T ("Zeiger in die Puls-Tabelle (in Viererschritten)"), T ("Zeiger in die Arpeggio-Tabelle") };
            }
            else if (ver == 15 || ver == 18 || ver == 20)
            {
                const bool v20 = ver == 20;
                ti.cols = { "AD", "SR", v20 ? T ("HF/HR") : T ("HiFreq"), T ("Res"), T ("Filter"), T ("Puls"), T ("Arp +"), T ("Arp -") };
                ti.help = { T ("Attack / Decay"), T ("Sustain / Release"),
                            v20 ? T ("Oben: 4 = HiFreq-Drums, 8 = Hard-Restart (zusammen $C0); unten: Arpeggio-Tempo, 0 = am schnellsten") : T ("HiFreq-Modus (00 = aus)"),
                            T ("Filter-Resonanz (oben, normal F) / Filter-Passband und Filter an (unten, 0 = aus)"), T ("Zeiger in die Filter-Tabelle (ab $04)"), T ("Zeiger in die Puls-Tabelle"),
                            T ("Arpeggio-Zeiger bei gehaltener Note (+++)"), T ("Arpeggio-Zeiger bei neuer Note (---)") };
            }
            else if (ver == 17)
            {
                ti.cols = { "AD", "SR", T ("PulsBr"), T ("PulsTp"), T ("PulsSt"), T ("Filter"), T ("Vibr"), T ("Arp") };
                ti.help = { T ("Attack / Decay"), T ("Sustain / Release"), T ("Pulsbreite: untere Grenze (oben) / obere Grenze (unten)"), T ("Puls-Geschwindigkeit (Additionswert)"), T ("Puls-Startwert ($1F0 wird F1)"),
                            T ("Zeiger in die Filter-Tabelle, 00 = Filter aus (nur Stimme 1)"), T ("Zeiger in die Vibrato-Tabelle"), T ("Zeiger in die Arpeggio-Tabelle") };
            }
            else if (ver == 19)
            {
                ti.cols = { "AD", "SR", T ("PulsSt"), T ("PulsTp"), T ("FiltSt"), T ("FiltAd"), T ("FiltDa"), T ("Arp") };
                ti.help = { T ("Attack / Decay"), T ("Sustain / Release"), T ("Puls-Start 00–0F"), T ("Puls-Geschwindigkeit"), T ("Filter-Startfrequenz"), T ("Filter-Addition (FE zieht 2 ab)"), T ("Dauer der Filter-Addition in Frames"), T ("Zeiger in die Arpeggio-Tabelle") };
            }
            else
            {
                ti.cols = { "A", "B", "C", "D", "E", "F", "G", "H" };
                ti.help = { T ("Attack / Decay"), T ("Sustain / Release"), T ("Je nach Variante (siehe die Anleitung von JCH)"), "", "", "", "", T ("meist: Zeiger in die Arpeggio-Tabelle") };
            }
            break;
        case NpTune::tabArpeggio:
            ti.title = T ("Arpeggio-Tabelle");
            ti.cols = { T ("Note +"), T ("Welle") };
            ti.help = { T ("Notenaddition $00–$5F zur Note der Sequenz, $80–$DF feste Note, $7F = Ende der Schleife"), T ("Wellenform ($11 Dreieck, $21 Säge, $41 Puls, $81 Rauschen); bei $7F: Sprungziel (Zeile)") };
            break;
        case NpTune::tabPulse:
            ti.title = T ("Puls-Tabelle (4 Byte je Satz)");
            if (ver >= 18 && ver != 19)
            {
                ti.cols = { T ("Start"), T ("Tempo"), T ("Dauer"), T ("Weiter") };
                ti.help = { T ("Start-Puls (oben tief, unten hoch); $FF = den aktuellen weiterbenutzen"), T ("Geschwindigkeit der Pulsänderung"), T ("Dauer des Satzes in Frames; +$80 = Puls läuft abwärts"), T ("Nummer des nächsten Satzes nach Ablauf der Dauer") };
            }
            else
            {
                ti.cols = { T ("Breite"), T ("Tempo"), T ("Dauer/Bit"), T ("Start") };
                ti.help = { T ("Untere Grenze (oben) / obere Grenze (unten) der Pulsbreite"), T ("Geschwindigkeit (Additionswert, $20 normal)"), T ("Bit 7 = Start (Byte 4) lesen, Bit 6 = auf diesem Satz bleiben, Bit 0–5 = Dauer in Frames"), T ("Start-Puls (unten Hi, oben Lo; $4A0 = A4)") };
            }
            break;
        case NpTune::tabFilter:
            ti.title = T ("Filter-Tabelle (4 Byte je Satz, die ersten vier sind reserviert)");
            if (ver == 17)
            {
                ti.cols = { T ("Wert"), T ("Wert"), T ("Wert"), T ("Wert") };
                ti.help = { T ("Rohwerte für $D416, je Frame einer; $FF = Ende, danach das Sprungziel"), "", "", "" };
            }
            else if (ver >= 18)
            {
                ti.cols = { T ("Start"), T ("Tempo"), T ("Dauer"), T ("Weiter") };
                ti.help = { T ("Start-Filterfrequenz ($FF = die aktuelle weiterbenutzen)"), T ("Additionswert je Frame ($F0 zieht $10 ab)"), T ("Dauer des Satzes in Frames"), T ("Nummer des nächsten Satzes") };
            }
            else
            {
                ti.cols = { T ("Breite"), T ("Tempo"), T ("Dauer/Bit"), T ("Start/Res") };
                ti.help = { T ("Untere / obere Grenze der Filterfrequenz"), T ("Geschwindigkeit der Filterfahrt"), T ("Bit 7 = Start lesen, Bit 6 = bleiben, Bit 0–5 = Dauer"), T ("Start-Frequenz (oben) / Resonanz (unten, normal F)") };
            }
            break;
        case NpTune::tabSlide:
            ti.title = ver >= 15 && ver != 17 ? T ("Super-Tabelle (Befehle Sxx)") : T ("Slide-Tabelle (Befehle Sxx)");
            if (ver >= 15 && ver != 17 && ver != 19)
            {
                ti.cols = { T ("Befehl"), T ("Wert") };
                ti.help = { T ("Oben: 0/2 Slide auf/ab, 4/6 Vibrato, 8 Hard-Restart, 9 Sustain ändern, C0–DF Arpeggio-Wechsel, E Tempo, F Lautstärke"), T ("Zweites Byte des Befehls (Geschwindigkeit, neuer Wert …)") };
            }
            else
            {
                ti.cols = { T ("Richtung/Tempo"), T ("Tempo lo") };
                ti.help = { T ("Bit 7 = Richtung (1 = abwärts), übrige Bits = Tempo (oberes Byte)"), T ("Tempo (unteres Byte)") };
            }
            break;
    }
    return ti;
}
} // namespace

class PatternView;
class GridView;

// ================================================================================================ the panel
class TrackerPanel : public juce::Component, private juce::Timer
{
public:
    TrackerPanel (MainComponent& m, TrackerWindow& w);
    ~TrackerPanel() override;

    MainComponent& main;
    TrackerWindow& window;
    NpTune tune;
    juce::File file;
    bool dirty = false;
    int octave = 4;
    std::array<bool, 3> muted { { false, false, false } };

    void edit (const std::function<void (NpTune&)>& fn, const juce::String& coalesceKey = {});
    void undo();
    void redo();
    void status (const juce::String& s);
    void tuneChanged();
    void play (bool fromCursor);
    void stop();
    bool isPlaying() const { return playing; }
    void confirmDiscard (std::function<void()> proceed);
    void newTune();
    void openFile();
    void openD64();
    void importMidi (const juce::File& midi);
    void chooseMidi();
    void saveFile (bool saveAs);
    void exportSid();
    void exportMidi();
    void updateTitle();
    void setOctave (int o);
    void changeSpeed (int d);
    bool closeRequested (std::function<void()> then);
    void stopOnClose() { stop(); }
    void loadForTest (const NpTune& t, const juce::File& f) { loadTune (t, f); }

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;

    PatternView* pattern = nullptr;
    GridView* grid = nullptr;

private:
    juce::TextButton tabButtons[5], voiceMute[3], newBtn, openBtn, d64Btn, saveAsBtn, sidBtn, midiBtn, midiInBtn, playBtn, cursorBtn, stopBtn,
                     octMinus, octPlus, spdMinus, spdPlus;
    juce::Label infoLabel, statusLabel, helpLabel, octLabel, spdLabel;
    std::unique_ptr<PatternView> patternPtr;
    std::unique_ptr<GridView> gridPtr;
    std::vector<NpTune> undoStack, redoStack;
    juce::String lastKey;
    juce::uint32 lastEditTime = 0;
    bool playing = false;
    int playStartRow = 0, playSpeed = 2;
    std::unique_ptr<juce::FileChooser> chooser;
    int currentTable = 0;

    void selectTable (int i);
    void loadTune (const NpTune& t, const juce::File& f);
    void timerCallback() override;
    const Theme& theme() const;
};

// ================================================================================================ pattern view
class PatternView : public juce::Component, private juce::ScrollBar::Listener
{
public:
    explicit PatternView (TrackerPanel& p) : panel (p), bar (true)
    {
        setWantsKeyboardFocus (true);
        addAndMakeVisible (bar);
        bar.addListener (this);
        bar.setAutoHide (false);
    }

    struct DRow { int entry = -1; int rowIn = 0; bool mark = false; bool broken = false; NpRow row; };

    void rebuild()
    {
        for (int v = 0; v < 3; ++v)
        {
            disp[(size_t) v].clear();
            bool endMark = false;
            const auto list = panel.tune.trackList (v, &endMark);
            for (int k = 0; k < (int) list.size(); ++k)
            {
                const auto rows = panel.tune.sequence (list[(size_t) k].seq);
                if (rows.empty()) { DRow d; d.entry = k; d.broken = true; disp[(size_t) v].push_back (d); continue; }
                for (int i = 0; i < (int) rows.size(); ++i) { DRow d; d.entry = k; d.rowIn = i; d.row = rows[(size_t) i]; disp[(size_t) v].push_back (d); }
            }
            DRow m; m.entry = (int) list.size(); m.mark = true;
            disp[(size_t) v].push_back (m);
        }
        total = 0;
        for (auto& d : disp) total = std::max (total, (int) d.size());
        clampCursor();
        updateBar();
        repaint();
    }

    void setCursorForTest (int voice, int row) { cv = juce::jlimit (0, 2, voice); cr = row; cf = 2; clampCursor(); repaint(); }
    int cursorVoice() const { return cv; }
    int cursorRow() const { return cr; }
    int entryAt (int voice, int row) const
    {
        const auto& d = disp[(size_t) voice];
        if (d.empty()) return 0;
        return d[(size_t) std::min (row, (int) d.size() - 1)].entry;
    }
    int entryStartRow (int voice, int entry) const
    {
        const auto& d = disp[(size_t) voice];
        for (size_t i = 0; i < d.size(); ++i) if (d[i].entry == entry && d[i].rowIn == 0) return (int) i;
        return 0;
    }
    void setPlayRow (int r)
    {
        if (r == playRow) return;
        playRow = r;
        if (r >= 0 && (r < first || r >= first + visibleRows() - 1)) { first = std::max (0, r - 2); updateBar(); }
        repaint();
    }

    void resized() override
    {
        bar.setBounds (getLocalBounds().removeFromRight (14));
        updateBar();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& th = panel.main.theme();
        const bool light = th.bg.getBrightness() > 0.5f;
        g.fillAll (th.bg);
        const auto f = mono (14.0f);
        g.setFont (f);
        const int cw = charW();
        g.setColour (th.panel);
        g.fillRect (0, 0, getWidth(), headH);
        for (int v = 0; v < 3; ++v)
        {
            const int x = voiceX (v);
            g.setColour (voiceColour (v, light).withAlpha (panel.muted[(size_t) v] ? 0.35f : 1.0f));
            g.setFont (font (13.0f, true));
            g.drawText (T ("Stimme %1", v + 1), x, 0, voiceW(), headH, juce::Justification::centredLeft, false);
        }
        g.setFont (f);
        const int vis = visibleRows();
        for (int r = first; r < first + vis && r < total + 1; ++r)
        {
            const int y = headH + (r - first) * rowH;
            const bool playHere = r == playRow;
            g.setColour (ui::rowColour (th, r));
            g.fillRect (0, y, getWidth() - bar.getWidth(), rowH);
            if (playHere) { g.setColour (juce::Colour (0xff2f9e44).withAlpha (0.35f)); g.fillRect (0, y, getWidth() - bar.getWidth(), rowH); }
            g.setColour (th.dim);
            g.drawText (juce::String::toHexString (r).paddedLeft ('0', 3).toUpperCase(), 4, y, gutter - 8, rowH, juce::Justification::centredRight, false);
            for (int v = 0; v < 3; ++v)
            {
                const auto& d = disp[(size_t) v];
                if (r >= (int) d.size()) continue;
                const auto& row = d[(size_t) r];
                const int x = voiceX (v);
                if (row.rowIn == 0 && ! row.mark && ! row.broken) { g.setColour (th.dim.withAlpha (0.45f)); g.drawHorizontalLine (y, (float) x - 4, (float) (x + voiceW() - 8)); }
                // track word (first row of a sequence, or the wrap / end mark)
                if (row.mark || row.rowIn == 0)
                {
                    const auto all = panel.tune.trackAll (v);
                    const auto e = row.entry < (int) all.size() ? all[(size_t) row.entry] : NpTrackEntry { 0xff, 0 };
                    juce::String s = hex2 (e.trans) + hex2 (e.seq);
                    if (typing && v == cv && r == cr && cf == 0) s = typedTrackText (e);
                    g.setColour (row.mark ? th.track[3] : th.dim.interpolatedWith (th.text, 0.45f));
                    g.drawText (s, x, y, cw * 4 + 2, rowH, juce::Justification::centredLeft, false);
                }
                if (row.mark || row.broken) continue;
                g.setColour ((row.row.cmd >= 0xa0) ? th.track[3] : th.dim);
                g.drawText (typing && v == cv && r == cr && cf == 1 ? cmdBuf.paddedRight ('_', 3) : cmdText (row.row.cmd), x + cw * 5, y, cw * 3 + 2, rowH, juce::Justification::centredLeft, false);
                g.setColour (row.row.note == 0 || row.row.note == 0x7e ? th.dim : voiceColour (v, light).brighter (0.2f));
                g.drawText (noteText (row.row.note), x + cw * 9, y, cw * 3 + 2, rowH, juce::Justification::centredLeft, false);
            }
        }
        // cursor
        {
            const int y = headH + (cr - first) * rowH;
            if (cr >= first && cr < first + vis)
            {
                const int fx = fieldX (cv, cf), fw = cf == 0 ? cw * 4 : cw * 3;
                g.setColour (th.accent);
                g.drawRect (fx - 2, y, fw + 5, rowH, 2);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        if (e.y < headH) return;
        const int r = first + (e.y - headH) / rowH;
        int v = -1;
        for (int i = 0; i < 3; ++i) if (e.x >= voiceX (i) - 4 && e.x < voiceX (i) + voiceW() - 4) v = i;
        if (v < 0 || r > total) return;
        const int cw = charW();
        const int rel = e.x - voiceX (v);
        int f = rel < cw * 5 ? 0 : rel < cw * 9 ? 1 : 2;
        cancelTyping();
        cv = v; cr = r;
        cf = f;
        clampCursor();
        if (e.mods.isPopupMenu()) showMenu (e.getScreenPosition());
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        first = juce::jlimit (0, std::max (0, total + 1 - visibleRows() + 2), first - (int) std::lround (w.deltaY * 6.0f));
        updateBar();
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override;

private:
    TrackerPanel& panel;
    juce::ScrollBar bar;
    std::array<std::vector<DRow>, 3> disp;
    int total = 0, first = 0, playRow = -1;
    int cv = 0, cr = 0, cf = 1;           // cursor: voice, row, field (0 track word, 1 command, 2 note)
    bool typing = false;
    juce::String cmdBuf, trackBuf;
    char cmdMode = 0;
    static constexpr int rowH = 18, headH = 24, gutter = 40;

    int charW() const { return std::max (6, (int) std::ceil (juce::GlyphArrangement::getStringWidthInt (mono (14.0f), "0"))); }
    int voiceW() const { return std::max (60, (getWidth() - gutter - bar.getWidth()) / 3); }
    int voiceX (int v) const { return gutter + v * voiceW() + 6; }
    int fieldX (int v, int f) const { return voiceX (v) + charW() * (f == 0 ? 0 : f == 1 ? 5 : 9); }
    int visibleRows() const { return std::max (1, (getHeight() - headH) / rowH); }
    void updateBar()
    {
        bar.setRangeLimits (0.0, (double) std::max (visibleRows(), total + 2));
        bar.setCurrentRange ((double) first, (double) visibleRows());
    }
    void scrollBarMoved (juce::ScrollBar*, double start) override { first = (int) std::lround (start); repaint(); }
    // which fields exist at a place: the track word on the first row of a sequence (and on the wrap / end mark), command and note on the rows
    bool fieldValid (int v, int r, int f) const
    {
        const auto& d = disp[(size_t) v];
        if (r >= (int) d.size()) return f == 1;
        const auto& row = d[(size_t) r];
        if (f == 0) return row.mark || (row.rowIn == 0);
        return ! row.mark && ! row.broken;
    }
    void clampCursor()
    {
        cr = juce::jlimit (0, std::max (0, total), cr);
        if (! fieldValid (cv, cr, cf))
            for (int f : { 1, 2, 0 }) if (fieldValid (cv, cr, f)) { cf = f; break; }
        scrollIntoView();
    }
    void scrollIntoView()
    {
        const int vis = visibleRows();
        if (cr < first) first = cr;
        else if (cr >= first + vis - 1) first = cr - vis + 2;
        first = std::max (0, first);
        updateBar();
    }
    juce::String typedTrackText (const NpTrackEntry& e) const
    {
        const auto cur = hex2 (e.trans) + hex2 (e.seq);
        return trackBuf + cur.substring (trackBuf.length());
    }
    void cancelTyping() { typing = false; cmdBuf = {}; trackBuf = {}; cmdMode = 0; }
    const DRow* current() const
    {
        const auto& d = disp[(size_t) cv];
        return cr < (int) d.size() ? &d[(size_t) cr] : nullptr;
    }
    void move (int dv, int dr);
    void setNote (int note);
    void setCommand (int cmd);
    void commitTrack (bool partialOk);
    void insertRow();
    void deleteRow();
    void insertEntry();
    void deleteEntry();
    void newSequenceForEntry();
    void duplicateSequenceForEntry();
    void showMenu (juce::Point<int> screen);
};

// ================================================================================================ table grid
class GridView : public juce::Component, private juce::ScrollBar::Listener
{
public:
    explicit GridView (TrackerPanel& p) : panel (p), bar (true)
    {
        setWantsKeyboardFocus (true);
        addAndMakeVisible (bar);
        bar.addListener (this);
        bar.setAutoHide (false);
    }

    void setTable (NpTune::Table t)
    {
        table = t;
        row = col = 0; first = 0; half = false;
        refresh();
    }
    NpTune::Table currentTable() const { return table; }
    TableInfo info() const { return tableInfo (table, panel.tune.playerVersion()); }
    void refresh() { updateBar(); repaint(); if (onFocusChanged) onFocusChanged(); }
    int focusedColumn() const { return col; }
    std::function<void()> onFocusChanged;

    void resized() override { bar.setBounds (getLocalBounds().removeFromRight (14)); updateBar(); }
    void paint (juce::Graphics& g) override
    {
        const auto& th = panel.main.theme();
        g.fillAll (th.bg);
        const auto ti = info();
        const int cols = panel.tune.tableCols (table), rows = panel.tune.tableRows (table);
        const int cw = colW();
        g.setColour (th.panel);
        g.fillRect (0, 0, getWidth(), headH);
        g.setFont (font (12.0f, true));
        g.setColour (th.text);
        for (int c = 0; c < cols; ++c) g.drawText (c < ti.cols.size() ? ti.cols[c] : juce::String (c + 1), gutter + c * cw, 0, cw, headH, juce::Justification::centred, true);
        g.setFont (mono (14.0f));
        const int vis = visibleRows();
        for (int r = first; r < first + vis && r < rows; ++r)
        {
            const int y = headH + (r - first) * rowH;
            g.setColour (ui::rowColour (th, r));
            g.fillRect (0, y, getWidth() - bar.getWidth(), rowH);
            g.setColour (th.dim);
            g.drawText (hex2 (r * step()), 2, y, gutter - 8, rowH, juce::Justification::centredRight, false);
            for (int c = 0; c < cols; ++c)
            {
                const bool cur = r == row && c == col;
                if (cur) { g.setColour (th.accent.withAlpha (0.35f)); g.fillRect (gutter + c * cw, y, cw, rowH); }
                g.setColour (cur ? th.text : th.text.withAlpha (panel.tune.get (table, r, c) == 0 ? 0.45f : 1.0f));
                g.drawText (cur && half ? juce::String::toHexString (panel.tune.get (table, r, c) >> 4).toUpperCase() + "_" : hex2 (panel.tune.get (table, r, c)), gutter + c * cw, y, cw, rowH, juce::Justification::centred, false);
            }
        }
        if (row >= first && row < first + vis)
        {
            g.setColour (th.accent);
            g.drawRect (gutter + col * cw, headH + (row - first) * rowH, cw, rowH, 2);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        if (e.y < headH) return;
        const int r = first + (e.y - headH) / rowH, c = (e.x - gutter) / colW();
        if (r < panel.tune.tableRows (table) && c >= 0 && c < panel.tune.tableCols (table)) { row = r; col = c; half = false; repaint(); if (onFocusChanged) onFocusChanged(); }
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        first = juce::jlimit (0, std::max (0, panel.tune.tableRows (table) - visibleRows()), first - (int) std::lround (w.deltaY * 6.0f));
        updateBar(); repaint();
    }
    bool keyPressed (const juce::KeyPress& k) override
    {
        const int rows = panel.tune.tableRows (table), cols = panel.tune.tableCols (table);
        const auto code = k.getKeyCode();
        if (k.getModifiers().isCommandDown()) return false;
        if (code == juce::KeyPress::upKey) row = std::max (0, row - 1);
        else if (code == juce::KeyPress::downKey) row = std::min (rows - 1, row + 1);
        else if (code == juce::KeyPress::leftKey) col = std::max (0, col - 1);
        else if (code == juce::KeyPress::rightKey || code == juce::KeyPress::tabKey) col = std::min (cols - 1, col + 1);
        else if (code == juce::KeyPress::pageUpKey) row = std::max (0, row - visibleRows());
        else if (code == juce::KeyPress::pageDownKey) row = std::min (rows - 1, row + visibleRows());
        else if (code == juce::KeyPress::homeKey) row = 0;
        else if (code == juce::KeyPress::endKey) row = rows - 1;
        else if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) { write (0); half = false; }
        else if (const int d = hexDigit (k.getTextCharacter()); d >= 0 && ! k.getModifiers().isAltDown())
        {
            const int old = panel.tune.get (table, row, col);
            if (! half) { write ((d << 4) | (old & 0x0f)); half = true; }
            else { write ((old & 0xf0) | d); half = false; row = std::min (rows - 1, row + 1); }
        }
        else return false;
        if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey
            || code == juce::KeyPress::tabKey || code == juce::KeyPress::pageUpKey || code == juce::KeyPress::pageDownKey || code == juce::KeyPress::homeKey || code == juce::KeyPress::endKey) half = false;
        if (row < first) first = row;
        else if (row >= first + visibleRows()) first = row - visibleRows() + 1;
        updateBar();
        repaint();
        if (onFocusChanged) onFocusChanged();
        return true;
    }

private:
    TrackerPanel& panel;
    juce::ScrollBar bar;
    NpTune::Table table = NpTune::tabInstrument;
    int row = 0, col = 0, first = 0;
    bool half = false;
    static constexpr int rowH = 18, headH = 24, gutter = 34;

    int step() const { return table == NpTune::tabPulse || table == NpTune::tabFilter ? 4 : table == NpTune::tabSlide ? 2 : 1; }
    int colW() const { return std::max (30, (getWidth() - gutter - bar.getWidth()) / std::max (1, panel.tune.tableCols (table))); }
    int visibleRows() const { return std::max (1, (getHeight() - headH) / rowH); }
    void updateBar()
    {
        const int rows = panel.tune.tableRows (table);
        bar.setRangeLimits (0.0, (double) rows);
        bar.setCurrentRange ((double) first, (double) visibleRows());
    }
    void scrollBarMoved (juce::ScrollBar*, double start) override { first = (int) std::lround (start); repaint(); }
    void write (int v)
    {
        const auto t = table; const int r = row, c = col;
        panel.edit ([t, r, c, v] (NpTune& n) { n.set (t, r, c, (juce::uint8) v); }, "tbl" + juce::String ((int) t) + ":" + juce::String (r) + ":" + juce::String (c));
    }
};

// ================================================================================================ pattern view: editing
void PatternView::move (int dv, int dr)
{
    cancelTyping();
    if (dr != 0) cr = juce::jlimit (0, std::max (0, total), cr + dr);
    if (dv != 0)
    {
        // left / right: through the fields of the voice, then on to the neighbour voice
        int v = cv, f = cf;
        for (int guard = 0; guard < 12; ++guard)
        {
            f += dv;
            if (f < 0) { if (v == 0) break; --v; f = 2; }
            else if (f > 2) { if (v == 2) break; ++v; f = 0; }
            if (fieldValid (v, cr, f)) { cv = v; cf = f; break; }
        }
    }
    clampCursor();
    repaint();
}

void PatternView::setNote (int note)
{
    const auto* d = current();
    if (d == nullptr || d->mark || d->broken) return;
    const int seq = panel.tune.trackList (cv)[(size_t) d->entry].seq, ri = d->rowIn;
    panel.edit ([seq, ri, note] (NpTune& t)
    {
        auto rows = t.sequence (seq);
        if (ri < (int) rows.size()) { rows[(size_t) ri].note = (juce::uint8) note; t.setSequence (seq, rows); }
    });
}

void PatternView::setCommand (int cmd)
{
    const auto* d = current();
    if (d == nullptr || d->mark || d->broken) return;
    const int seq = panel.tune.trackList (cv)[(size_t) d->entry].seq, ri = d->rowIn;
    panel.edit ([seq, ri, cmd] (NpTune& t)
    {
        auto rows = t.sequence (seq);
        if (ri < (int) rows.size()) { rows[(size_t) ri].cmd = (juce::uint8) cmd; t.setSequence (seq, rows); }
    });
}

void PatternView::commitTrack (bool partialOk)
{
    const auto* d = current();
    if (d == nullptr || ! (d->mark || d->rowIn == 0) || trackBuf.isEmpty() || (! partialOk && trackBuf.length() < 4)) { cancelTyping(); return; }
    const auto all = panel.tune.trackAll (cv);
    const auto e = d->entry < (int) all.size() ? all[(size_t) d->entry] : NpTrackEntry { 0xff, 0 };
    const auto s = (trackBuf + (hex2 (e.trans) + hex2 (e.seq)).substring (trackBuf.length()));
    int trans = s.substring (0, 2).getHexValue32(), seq = s.substring (2, 4).getHexValue32();
    if (trans < 0xfe && trans < 0x80) trans = 0x80;
    if (trans >= 0xfe) seq = 0;
    else seq = std::min (seq, (int) NpTune::kMaxSequence);
    const int voice = cv, idx = d->entry;
    cancelTyping();
    panel.edit ([voice, idx, trans, seq] (NpTune& t)
    {
        t.setTrackEntry (voice, idx, { (juce::uint8) trans, (juce::uint8) seq });
        if (trans < 0xfe && ! t.sequenceUsable (seq)) t.makeBlankSequence (seq, 16);   // a new sequence opens with 16 rows
    });
}

void PatternView::insertRow()
{
    const auto* d = current();
    if (d == nullptr || d->mark || d->broken) return;
    const int seq = panel.tune.trackList (cv)[(size_t) d->entry].seq, ri = d->rowIn;
    if ((int) panel.tune.sequence (seq).size() >= NpTune::kMaxRows) { panel.status (T ("Eine Sequenz hat höchstens %1 Zeilen", NpTune::kMaxRows)); return; }
    panel.edit ([seq, ri] (NpTune& t) { auto rows = t.sequence (seq); rows.insert (rows.begin() + ri, NpRow()); t.setSequence (seq, rows); });
}

void PatternView::deleteRow()
{
    const auto* d = current();
    if (d == nullptr || d->mark || d->broken) return;
    const int seq = panel.tune.trackList (cv)[(size_t) d->entry].seq, ri = d->rowIn;
    if (panel.tune.sequence (seq).size() <= 1) { panel.status (T ("Eine Sequenz braucht mindestens eine Zeile")); return; }
    panel.edit ([seq, ri] (NpTune& t) { auto rows = t.sequence (seq); rows.erase (rows.begin() + ri); t.setSequence (seq, rows); });
}

void PatternView::insertEntry()
{
    const auto* d = current();
    if (d == nullptr) return;
    const int voice = cv, idx = d->entry;
    const auto all = panel.tune.trackAll (voice);
    const auto e = idx < (int) all.size() && ! all[(size_t) idx].isMark() ? all[(size_t) idx] : NpTrackEntry { 0x8c, 0 };
    panel.edit ([voice, idx, e] (NpTune& t)
    {
        t.insertTrackEntry (voice, idx, e);
        if (! t.sequenceUsable (e.seq)) t.makeBlankSequence (e.seq, 16);
    });
}

void PatternView::deleteEntry()
{
    const auto* d = current();
    if (d == nullptr || d->mark) return;
    const int voice = cv, idx = d->entry;
    panel.edit ([voice, idx] (NpTune& t) { t.deleteTrackEntry (voice, idx); });
}

void PatternView::newSequenceForEntry()
{
    const auto* d = current();
    if (d == nullptr || d->mark) return;
    const int n = panel.tune.firstFreeSequence();
    if (n < 0) { panel.status (T ("Alle Sequenzen sind belegt")); return; }
    const int voice = cv, idx = d->entry;
    panel.edit ([voice, idx, n] (NpTune& t)
    {
        auto e = t.trackAll (voice)[(size_t) idx];
        e.seq = (juce::uint8) n;
        t.makeBlankSequence (n, 16);
        t.setTrackEntry (voice, idx, e);
    });
}

void PatternView::duplicateSequenceForEntry()
{
    const auto* d = current();
    if (d == nullptr || d->mark) return;
    const int n = panel.tune.firstFreeSequence();
    if (n < 0) { panel.status (T ("Alle Sequenzen sind belegt")); return; }
    const int voice = cv, idx = d->entry;
    panel.edit ([voice, idx, n] (NpTune& t)
    {
        auto e = t.trackAll (voice)[(size_t) idx];
        const auto rows = t.sequence (e.seq);
        e.seq = (juce::uint8) n;
        t.setSequence (n, rows.empty() ? std::vector<NpRow> (16) : rows);
        t.setTrackEntry (voice, idx, e);
    });
}

void PatternView::showMenu (juce::Point<int> screen)
{
    juce::PopupMenu m;
    m.addItem (1, T ("Zeile einfügen (Einfg)"));
    m.addItem (2, T ("Zeile löschen (Entf)"));
    m.addSeparator();
    m.addItem (3, T ("Track-Eintrag davor einfügen (Strg + Einfg)"));
    m.addItem (4, T ("Track-Eintrag löschen (Strg + Entf)"));
    m.addItem (5, T ("Neue leere Sequenz für diesen Eintrag"));
    m.addItem (6, T ("Sequenz kopieren (Eintrag bekommt die Kopie)"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screen.x, screen.y, 1, 1)), [safe = juce::Component::SafePointer<PatternView> (this)] (int r)
    {
        if (safe == nullptr) return;
        switch (r) { case 1: safe->insertRow(); break; case 2: safe->deleteRow(); break; case 3: safe->insertEntry(); break; case 4: safe->deleteEntry(); break;
                     case 5: safe->newSequenceForEntry(); break; case 6: safe->duplicateSequenceForEntry(); break; default: break; }
    });
}

bool PatternView::keyPressed (const juce::KeyPress& k)
{
    const auto code = k.getKeyCode();
    const auto mods = k.getModifiers();
    const auto ch = k.getTextCharacter();
    const bool ctrl = mods.isCommandDown();
    if (ctrl && (code == juce::KeyPress::insertKey)) { insertEntry(); return true; }
    if (ctrl && (code == juce::KeyPress::deleteKey)) { deleteEntry(); return true; }
    if (ctrl) return false;
    if (code == juce::KeyPress::upKey) { if (typing && cf == 0) commitTrack (true); move (0, -1); return true; }
    if (code == juce::KeyPress::downKey) { if (typing && cf == 0) commitTrack (true); move (0, 1); return true; }
    if (code == juce::KeyPress::leftKey) { if (typing && cf == 0) commitTrack (true); move (-1, 0); return true; }
    if (code == juce::KeyPress::rightKey || code == juce::KeyPress::tabKey) { if (typing && cf == 0) commitTrack (true); move (1, 0); return true; }
    if (code == juce::KeyPress::pageUpKey) { move (0, -16); return true; }
    if (code == juce::KeyPress::pageDownKey) { move (0, 16); return true; }
    if (code == juce::KeyPress::homeKey) { cancelTyping(); cr = 0; clampCursor(); repaint(); return true; }
    if (code == juce::KeyPress::endKey) { cancelTyping(); cr = total; clampCursor(); repaint(); return true; }
    if (code == juce::KeyPress::insertKey) { insertRow(); return true; }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        if (cf == 0) { if (typing) { cancelTyping(); repaint(); } else deleteEntry(); }
        else if (cf == 1) setCommand (0x80);
        else setNote (0);
        return true;
    }
    if (code == juce::KeyPress::escapeKey) { if (typing) { cancelTyping(); repaint(); return true; } return false; }
    if (code == juce::KeyPress::F7Key) { panel.setOctave (panel.octave - 1); return true; }
    if (code == juce::KeyPress::F8Key) { panel.setOctave (panel.octave + 1); return true; }
    if (ch == '<') { panel.setOctave (panel.octave - 1); return true; }
    if (ch == '>') { panel.setOctave (panel.octave + 1); return true; }
    if (code == juce::KeyPress::returnKey)
    {
        if (cf == 0 && typing) { commitTrack (true); move (0, 1); }
        else move (0, 1);
        return true;
    }
    // ---- typing
    if (cf == 2)
    {
        if (ch == ' ') { setNote (0); move (0, 1); return true; }
        if (ch == '+') { setNote (0x7e); move (0, 1); return true; }
        if (const int p = pianoKey (ch); p >= 0)
        {
            setNote (std::min (95, panel.octave * 12 + p));
            move (0, 1);
            return true;
        }
    }
    else if (cf == 1)
    {
        const auto c = juce::CharacterFunctions::toLowerCase (ch);
        if (c == 'i' || c == 's') { typing = true; cmdMode = (char) c; cmdBuf = juce::String::charToString (juce::CharacterFunctions::toUpperCase (ch)); repaint(); return true; }
        if (ch == '*') { setCommand (0x90); move (0, 1); return true; }
        if (ch == '-' || ch == ' ') { setCommand (0x80); move (0, 1); return true; }
        if (typing && hexDigit (ch) >= 0)
        {
            cmdBuf += juce::String::charToString (juce::CharacterFunctions::toUpperCase (ch));
            if (cmdBuf.length() >= 3)
            {
                const int hi = hexDigit (cmdBuf[1]), lo = hexDigit (cmdBuf[2]);
                const int cmd = cmdMode == 'i' ? 0xa0 + ((hi & 1) << 4) + lo : 0xc0 + ((hi & 3) << 4) + lo;
                cancelTyping();
                setCommand (cmd);
                move (0, 1);
            }
            repaint();
            return true;
        }
    }
    else if (cf == 0)
    {
        if (hexDigit (ch) >= 0 && trackBuf.length() < 4)
        {
            typing = true;
            trackBuf += juce::String::charToString (juce::CharacterFunctions::toUpperCase (ch));
            if (trackBuf.length() == 4) { commitTrack (false); move (0, 1); }
            repaint();
            return true;
        }
    }
    return false;
}

// ================================================================================================ the panel
static juce::String titleBase() { return T ("Vibrants-Tracker") + " " SM_VERSION_STRING; }

const Theme& TrackerPanel::theme() const { return main.theme(); }

TrackerPanel::TrackerPanel (MainComponent& m, TrackerWindow& w) : main (m), window (w)
{
    patternPtr = std::make_unique<PatternView> (*this);
    gridPtr = std::make_unique<GridView> (*this);
    pattern = patternPtr.get();
    grid = gridPtr.get();
    addAndMakeVisible (*pattern);
    addAndMakeVisible (*grid);
    const char* names[] = { "Instrumente", "Arpeggio", "Puls", "Filter", "Slide / Super" };
    for (int i = 0; i < 5; ++i)
    {
        tabButtons[i].setButtonText (T (names[i]));
        tabButtons[i].setClickingTogglesState (true);
        tabButtons[i].setRadioGroupId (4711);
        tabButtons[i].onClick = [this, i] { selectTable (i); };
        addAndMakeVisible (tabButtons[i]);
    }
    tabButtons[0].setToggleState (true, juce::dontSendNotification);
    struct B { juce::TextButton* b; const char* text; std::function<void()> fn; const char* tip; };
    const B list[] = {
        { &newBtn, "Neu", [this] { confirmDiscard ([this] { newTune(); }); }, "Neuer Tune mit der Variante 20.G4 (Strg + N)" },
        { &openBtn, "Öffnen (.np)", [this] { confirmDiscard ([this] { openFile(); }); }, "Einen Tune laden (.np, Strg + O)" },
        { &midiInBtn, "Von MIDI …", [this] { confirmDiscard ([this] { chooseMidi(); }); }, "Eine MIDI-Datei in einen Tune umwandeln: Akkorde werden Arpeggios, Schlagzeug bekommt eigene Instrumente" },
        { &d64Btn, "Von D64 …", [this] { confirmDiscard ([this] { openD64(); }); }, "Einen Tune aus einem Diskettenabbild (.d64) laden, z. B. WORK1.D64 aus dem JCH-Paket" },
        { &saveAsBtn, "Speichern (.np)", [this] { saveFile (true); }, "Tune als .np-Datei speichern (mit Dateiname); Strg + S speichert in die geöffnete Datei" },
        { &sidBtn, "Speichern (.sid)", [this] { exportSid(); }, "Als .sid-Datei speichern (spielbar in jedem SID-Abspielprogramm und im SID-Archiv)" },
        { &midiBtn, "Speichern (.mid)", [this] { exportMidi(); }, "Als MIDI-Datei speichern (über die SID-nach-MIDI-Umwandlung)" },
        { &playBtn, "", [this] { play (false); }, "Von Anfang spielen (F1)" },
        { &cursorBtn, "", [this] { play (true); }, "Ab der Cursor-Position spielen (F2)" },
        { &stopBtn, "", [this] { stop(); }, "Stopp (Esc)" },
    };
    for (auto& e : list)
    {
        e.b->setButtonText (T (e.text));
        e.b->setTooltip (T (e.tip));
        e.b->onClick = e.fn;
        addAndMakeVisible (*e.b);
    }
    stopBtn.setButtonText (juce::String::fromUTF8 ("■"));
    playBtn.setButtonText (juce::String::fromUTF8 ("▶ ") + T ("Von vorn"));
    cursorBtn.setButtonText (juce::String::fromUTF8 ("▶ ") + T ("Ab Cursor"));
    for (auto* b : { &playBtn, &cursorBtn, &stopBtn })
    {
        const auto c = b == &stopBtn ? juce::Colour (0xffd0393a) : juce::Colour (0xff2f9e44);
        b->setColour (juce::TextButton::buttonColourId, c);
        b->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    }
    for (int v = 0; v < 3; ++v)
    {
        voiceMute[v].setButtonText (T ("Stimme %1 stumm", v + 1));
        voiceMute[v].setClickingTogglesState (true);
        voiceMute[v].onClick = [this, v]
        {
            muted[(size_t) v] = voiceMute[v].getToggleState();
            main.player().setVoiceMuted (v, muted[(size_t) v]);
            pattern->repaint();
        };
        addAndMakeVisible (voiceMute[v]);
    }
    for (auto* b : { &octMinus, &octPlus, &spdMinus, &spdPlus }) addAndMakeVisible (*b);
    octMinus.setButtonText ("-"); octPlus.setButtonText ("+"); spdMinus.setButtonText ("-"); spdPlus.setButtonText ("+");
    octMinus.onClick = [this] { setOctave (octave - 1); };
    octPlus.onClick = [this] { setOctave (octave + 1); };
    spdMinus.onClick = [this] { changeSpeed (-1); };
    spdPlus.onClick = [this] { changeSpeed (1); };
    octMinus.setTooltip (T ("Oktave tiefer (F7)")); octPlus.setTooltip (T ("Oktave höher (F8)"));
    spdMinus.setTooltip (T ("Tune-Tempo: Frames je Zeile, kleiner = schneller")); spdPlus.setTooltip (T ("Tune-Tempo: Frames je Zeile, größer = langsamer"));
    for (auto* l : { &infoLabel, &statusLabel, &helpLabel, &octLabel, &spdLabel }) addAndMakeVisible (*l);
    infoLabel.setFont (font (13.0f, true));
    statusLabel.setFont (font (12.5f));
    helpLabel.setFont (font (12.5f));
    helpLabel.setJustificationType (juce::Justification::topLeft);
    helpLabel.setMinimumHorizontalScale (1.0f);
    octLabel.setJustificationType (juce::Justification::centred);
    spdLabel.setJustificationType (juce::Justification::centred);
    grid->onFocusChanged = [this]
    {
        const auto ti = grid->info();
        const int c = grid->focusedColumn();
        helpLabel.setText (ti.title + "\n" + (c < ti.help.size() ? ti.cols[c] + ": " + ti.help[c] : juce::String()), juce::dontSendNotification);
    };
    // a new tune from the template
    NpTune t;
    juce::String err;
    t.loadPrg (BinaryData::np_v20g4_prg, (size_t) BinaryData::np_v20g4_prgSize, err);
    t.makeStarter();
    loadTune (t, {});
    selectTable (0);
    startTimerHz (30);
}

TrackerPanel::~TrackerPanel()
{
    stopTimer();
    stop();
}

void TrackerPanel::loadTune (const NpTune& t, const juce::File& f)
{
    tune = t;
    file = f;
    dirty = false;
    undoStack.clear(); redoStack.clear();
    tuneChanged();
    updateTitle();
}

void TrackerPanel::tuneChanged()
{
    pattern->rebuild();
    grid->refresh();
    spdLabel.setText (T ("Tempo %1", tune.speed()), juce::dontSendNotification);
    octLabel.setText (T ("Oktave %1", octave), juce::dontSendNotification);
    const auto pn = tune.playerName();
    infoLabel.setText (T ("Variante %1", pn.isNotEmpty() ? pn : T ("unbekannt")) + "   ·   " + T ("%1 Sequenzen", (int) tune.usedSequences().size())
                           + "   ·   " + T ("Länge etwa %1 s", juce::String (tune.estimateSeconds(), 1)), juce::dontSendNotification);
    repaint();
}

void TrackerPanel::edit (const std::function<void (NpTune&)>& fn, const juce::String& key)
{
    const auto now = juce::Time::getMillisecondCounter();
    const bool coalesce = key.isNotEmpty() && key == lastKey && now - lastEditTime < 1500 && ! undoStack.empty();
    if (! coalesce) { undoStack.push_back (tune); if (undoStack.size() > 150) undoStack.erase (undoStack.begin()); }
    redoStack.clear();
    lastKey = key; lastEditTime = now;
    fn (tune);
    dirty = true;
    tuneChanged();
    updateTitle();
}

void TrackerPanel::undo()
{
    if (undoStack.empty()) { status (T ("Nichts zum Rückgängigmachen")); return; }
    redoStack.push_back (tune);
    tune = undoStack.back();
    undoStack.pop_back();
    lastKey = {};
    dirty = true;
    tuneChanged();
    updateTitle();
}

void TrackerPanel::redo()
{
    if (redoStack.empty()) return;
    undoStack.push_back (tune);
    tune = redoStack.back();
    redoStack.pop_back();
    lastKey = {};
    dirty = true;
    tuneChanged();
    updateTitle();
}

void TrackerPanel::status (const juce::String& s) { statusLabel.setText (s, juce::dontSendNotification); }

void TrackerPanel::updateTitle()
{
    window.setName (titleBase() + " – " + (file != juce::File() ? file.getFileName() : T ("Neuer Tune")) + (dirty ? " *" : ""));
}

void TrackerPanel::setOctave (int o)
{
    octave = juce::jlimit (0, 7, o);
    octLabel.setText (T ("Oktave %1", octave), juce::dontSendNotification);
}

void TrackerPanel::changeSpeed (int d)
{
    const int s = juce::jlimit (1, 9, tune.speed() + d);
    edit ([s] (NpTune& t) { t.setSpeed (s); }, "speed");
}

void TrackerPanel::selectTable (int i)
{
    currentTable = i;
    static const NpTune::Table tabs[] = { NpTune::tabInstrument, NpTune::tabArpeggio, NpTune::tabPulse, NpTune::tabFilter, NpTune::tabSlide };
    grid->setTable (tabs[i]);
}

// ---- play
void TrackerPanel::play (bool fromCursor)
{
    main.sidTab().stopPlayback();
    main.midiTab().stopPreview();
    main.player().stop();
    NpTune t = tune;
    playStartRow = 0;
    if (fromCursor)
    {
        std::array<int, 3> idx { 0, 0, 0 };
        for (int v = 0; v < 3; ++v) idx[(size_t) v] = pattern->entryAt (v, pattern->cursorRow());
        t = tune.withStartIndex (idx);
        playStartRow = pattern->entryStartRow (pattern->cursorVoice(), idx[(size_t) pattern->cursorVoice()]);
    }
    const auto psid = t.toPsid ("Tracker", "SIDiMIDI", "");
    const auto si = parseSidBytes (psid.getData(), psid.getSize(), true);
    const double secs = juce::jlimit (3.0, 600.0, t.estimateSeconds() + 1.0);
    auto tr = computeTrace (si, 1, secs);
    if (tr == nullptr || tr->empty()) { status (T ("Der Tune lässt sich nicht abspielen")); return; }
    for (int v = 0; v < 3; ++v) main.player().setVoiceMuted (v, muted[(size_t) v]);
    main.player().playTrace (*tr->events, tr->fps, tr->frames, 0, si.ntsc());
    playing = true;
    playSpeed = std::max (1, t.speed()) + 1;      // frames per row
    status ({});
}

void TrackerPanel::stop()
{
    if (playing) main.player().stop();
    playing = false;
    if (pattern != nullptr) pattern->setPlayRow (-1);
}

void TrackerPanel::timerCallback()
{
    if (! playing) return;
    auto& pl = main.player();
    if (! pl.hasSidTrace() || ! pl.isSidTunePlaying())
    {
        playing = false;
        pl.takeSidFinished();
        pattern->setPlayRow (-1);
        return;
    }
    pattern->setPlayRow (playStartRow + (int) (pl.getPlayFrame() / (juce::uint32) playSpeed));
}

// ---- files
void TrackerPanel::confirmDiscard (std::function<void()> proceed)
{
    if (! dirty) { proceed(); return; }
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, T ("Ungespeicherte Änderungen"),
        T ("Der Tune wurde geändert und nicht gespeichert. Trotzdem fortfahren?"), T ("Verwerfen"), T ("Abbrechen"), this,
        juce::ModalCallbackFunction::create ([proceed] (int r) { if (r != 0) proceed(); }));
}

bool TrackerPanel::closeRequested (std::function<void()> then)
{
    confirmDiscard (std::move (then));
    return true;
}

void TrackerPanel::newTune()
{
    stop();
    NpTune t;
    juce::String err;
    t.loadPrg (BinaryData::np_v20g4_prg, (size_t) BinaryData::np_v20g4_prgSize, err);
    t.makeStarter();
    loadTune (t, {});
    status (T ("Neuer Tune (Variante 20.G4) mit fünf Start-Instrumenten: 1 Säge, 2 Puls, 3 Bass, 4 Schlagzeug, 5 Akkord"));
}

void TrackerPanel::openFile()
{
    const juce::File start (main.archive().settings.getProperty ("trackerDir").toString());
    main.chooseOpenFile (T ("Tune öffnen (.np)"), start.isDirectory() ? start : juce::File(), "*.prg;*.np;*.wrk;*",
                         [safe = juce::Component::SafePointer<TrackerPanel> (this)] (const juce::File& f)
    {
        if (safe == nullptr) return;
        NpTune t; juce::String err;
        if (! t.loadFile (f, err)) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SIDiMIDI", err); return; }
        safe->stop();
        safe->loadTune (t, f);
        safe->main.archive().settings.setProperty ("trackerDir", f.getParentDirectory().getFullPathName(), nullptr);
        safe->main.notifySettings();
        safe->status (T ("Geladen: %1", f.getFileName()));
    });
}

void TrackerPanel::openD64()
{
    const juce::File start (main.archive().settings.getProperty ("trackerDir").toString());
    main.chooseOpenFile (T ("Diskettenabbild (.d64) öffnen"), start.isDirectory() ? start : juce::File(), "*.d64",
                         [safe = juce::Component::SafePointer<TrackerPanel> (this)] (const juce::File& img)
    {
        if (safe == nullptr) return;
        auto files = std::make_shared<std::vector<NpD64File>> ();
        if (! npD64List (img, *files)) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SIDiMIDI", T ("Das ist kein gültiges Diskettenabbild")); return; }
        safe->main.archive().settings.setProperty ("trackerDir", img.getParentDirectory().getFullPathName(), nullptr);
        safe->main.notifySettings();
        juce::PopupMenu m;
        std::vector<int> map;
        for (size_t i = 0; i < files->size(); ++i)
        {
            if ((*files)[i].blocks < 8) continue;
            m.addItem ((int) map.size() + 1, (*files)[i].name + "   (" + juce::String ((*files)[i].blocks) + " " + T ("Blöcke") + ")");
            map.push_back ((int) i);
        }
        if (map.empty()) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "SIDiMIDI", T ("Auf dem Abbild liegen keine Tunes")); return; }
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (safe.getComponent()), [safe, files, map, img] (int r)
        {
            if (safe == nullptr || r <= 0) return;
            const auto& f = (*files)[(size_t) map[(size_t) r - 1]];
            juce::MemoryBlock mb;
            NpTune t; juce::String err;
            if (! npD64Extract (img, f, mb) || ! t.loadPrg (mb.getData(), mb.getSize(), err))
            { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SIDiMIDI", err.isNotEmpty() ? err : T ("Die Datei lässt sich nicht lesen")); return; }
            safe->stop();
            safe->loadTune (t, {});
            safe->window.setName (titleBase() + " – " + f.name);
            safe->status (T ("Geladen: %1 (Variante %2)", f.name, t.playerName()));
        });
    });
}

void TrackerPanel::chooseMidi()
{
    const juce::File start (main.archive().settings.getProperty ("lastMidiFolder").toString());
    main.chooseOpenFile (T ("MIDI-Datei in einen Tune umwandeln"), start.isDirectory() ? start : juce::File(), "*.mid;*.midi;*.kar;*.rmi",
                         [safe = juce::Component::SafePointer<TrackerPanel> (this)] (const juce::File& f) { if (safe != nullptr) safe->importMidi (f); });
}

void TrackerPanel::importMidi (const juce::File& midi)
{
    stop();
    auto r = npImportMidi (midi, BinaryData::np_v20g4_prg, (size_t) BinaryData::np_v20g4_prgSize);
    if (! r.ok) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SIDiMIDI", r.report); return; }
    loadTune (r.tune, {});
    dirty = true;                                   // not saved yet
    window.setName (titleBase() + " – " + midi.getFileNameWithoutExtension() + " *");
    status (T ("MIDI umgewandelt: %1", midi.getFileName()));
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, T ("MIDI → Vibrants-Tracker"), r.report + "\n\n" + T ("Der Tune ist noch nicht gespeichert. Zum Anhören F1 drücken; Klänge und Tabellen lassen sich rechts nachbearbeiten."));
}

void TrackerPanel::saveFile (bool saveAs)
{
    auto doSave = [safe = juce::Component::SafePointer<TrackerPanel> (this)] (const juce::File& f)
    {
        if (safe == nullptr) return;
        juce::String err;
        if (! safe->tune.saveFile (f, err)) { juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SIDiMIDI", err); return; }
        safe->file = f; safe->dirty = false;
        safe->main.archive().settings.setProperty ("trackerDir", f.getParentDirectory().getFullPathName(), nullptr);
        safe->main.notifySettings();
        safe->updateTitle();
        safe->status (T ("Gespeichert: %1", f.getFullPathName()));
    };
    if (! saveAs && file != juce::File()) { doSave (file); return; }
    const juce::File start (main.archive().settings.getProperty ("trackerDir").toString());
    main.chooseSaveFile (T ("Tune speichern unter"), (start.isDirectory() ? start : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)).getChildFile (file != juce::File() ? file.getFileName() : "Neuer Tune.np"),
                         "*.np;*.prg", [doSave] (const juce::File& f) { doSave (f.hasFileExtension ("np;prg") ? f : f.withFileExtension ("np")); });
}

void TrackerPanel::exportSid()
{
    const juce::File start (main.archive().settings.getProperty ("trackerDir").toString());
    const auto name = (file != juce::File() ? file.getFileNameWithoutExtension() : juce::String ("Neuer Tune"));
    main.chooseSaveFile (T ("Als SID-Datei speichern"), (start.isDirectory() ? start : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)).getChildFile (name + ".sid"),
                         "*.sid", [safe = juce::Component::SafePointer<TrackerPanel> (this), name] (const juce::File& f)
    {
        if (safe == nullptr) return;
        const auto psid = safe->tune.toPsid (name, "SIDiMIDI Tracker", juce::String (juce::Time::getCurrentTime().getYear()));
        const auto target = f.hasFileExtension ("sid") ? f : f.withFileExtension ("sid");
        if (target.replaceWithData (psid.getData(), psid.getSize())) safe->status (T ("SID geschrieben: %1", target.getFullPathName()));
        else safe->status (T ("Schreiben nicht möglich"));
    });
}

void TrackerPanel::exportMidi()
{
    const juce::File start (main.archive().settings.getProperty ("trackerDir").toString());
    const auto name = (file != juce::File() ? file.getFileNameWithoutExtension() : juce::String ("Neuer Tune"));
    main.chooseSaveFile (T ("Als MIDI-Datei speichern"), (start.isDirectory() ? start : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)).getChildFile (name + ".mid"),
                         "*.mid", [safe = juce::Component::SafePointer<TrackerPanel> (this), name] (const juce::File& f)
    {
        if (safe == nullptr) return;
        const auto psid = safe->tune.toPsid (name, "SIDiMIDI Tracker", "");
        const auto tmp = juce::File::createTempFile (".sid");
        tmp.replaceWithData (psid.getData(), psid.getSize());
        const auto target = f.hasFileExtension ("mid") ? f : f.withFileExtension ("mid");
        juce::String log;
        const bool ok = SidToMidiCoreEngine::convertSidToMidiFile (tmp, target, log, 1);
        tmp.deleteFile();
        safe->status (ok ? T ("MIDI geschrieben: %1", target.getFullPathName()) : T ("Konvertierung fehlgeschlagen: %1", log.trim().fromLastOccurrenceOf ("\n", false, false)));
    });
}

// ---- layout
void TrackerPanel::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto row1 = r.removeFromTop (28);
    for (auto* b : { &newBtn, &openBtn, &d64Btn, &midiInBtn, &saveAsBtn, &sidBtn, &midiBtn })
    {
        const int w = juce::jlimit (60, 150, juce::GlyphArrangement::getStringWidthInt (font (14.0f), b->getButtonText()) + 26);
        b->setBounds (row1.removeFromLeft (w));
        row1.removeFromLeft (4);
    }
    r.removeFromTop (6);
    auto row2 = r.removeFromTop (28);
    playBtn.setBounds (row2.removeFromLeft (96)); row2.removeFromLeft (4);
    cursorBtn.setBounds (row2.removeFromLeft (110)); row2.removeFromLeft (4);
    stopBtn.setBounds (row2.removeFromLeft (44)); row2.removeFromLeft (14);
    for (int v = 0; v < 3; ++v) { voiceMute[v].setBounds (row2.removeFromLeft (118)); row2.removeFromLeft (4); }
    row2.removeFromLeft (10);
    spdMinus.setBounds (row2.removeFromLeft (28)); spdLabel.setBounds (row2.removeFromLeft (80)); spdPlus.setBounds (row2.removeFromLeft (28)); row2.removeFromLeft (12);
    octMinus.setBounds (row2.removeFromLeft (28)); octLabel.setBounds (row2.removeFromLeft (90)); octPlus.setBounds (row2.removeFromLeft (28));
    r.removeFromTop (4);
    infoLabel.setBounds (r.removeFromTop (22));
    statusLabel.setBounds (r.removeFromBottom (22));
    r.removeFromBottom (4);
    const int rightW = juce::jlimit (330, 520, r.getWidth() * 36 / 100);
    auto right = r.removeFromRight (rightW);
    r.removeFromRight (8);
    pattern->setBounds (r);
    auto tabs = right.removeFromTop (26);
    const int tw = tabs.getWidth() / 5;
    for (int i = 0; i < 5; ++i) tabButtons[i].setBounds (tabs.removeFromLeft (i == 4 ? tabs.getWidth() : tw).reduced (1, 0));
    right.removeFromTop (4);
    helpLabel.setBounds (right.removeFromBottom (76));
    grid->setBounds (right);
}

void TrackerPanel::paint (juce::Graphics& g) { g.fillAll (theme().panel); }

bool TrackerPanel::keyPressed (const juce::KeyPress& k)
{
    const auto code = k.getKeyCode();
    if (code == juce::KeyPress::F1Key) { play (false); return true; }
    if (code == juce::KeyPress::F2Key) { play (true); return true; }
    if (code == juce::KeyPress::escapeKey) { stop(); return true; }
    const auto ctrlKey = [&k] (char c) { return k == juce::KeyPress (c, juce::ModifierKeys::ctrlModifier, 0); };
    if (ctrlKey ('z')) { undo(); return true; }
    if (ctrlKey ('y')) { redo(); return true; }
    if (ctrlKey ('s')) { saveFile (false); return true; }
    if (ctrlKey ('o')) { confirmDiscard ([this] { openFile(); }); return true; }
    if (ctrlKey ('n')) { confirmDiscard ([this] { newTune(); }); return true; }
    return false;
}

// ================================================================================================ the window
TrackerWindow::TrackerWindow (MainComponent& m)
    : juce::DocumentWindow (T ("Vibrants-Tracker") + " " SM_VERSION_STRING, m.theme().panel, juce::DocumentWindow::allButtons), main (m)
{
    setUsingNativeTitleBar (true);
    content = new TrackerPanel (m, *this);
    setContentOwned (content, true);
    setResizable (true, true);
    setResizeLimits (900, 560, 3000, 2000);
    centreWithSize (1240, 760);
    setVisible (true);
    content->pattern->grabKeyboardFocus();
}

void TrackerWindow::importMidiFile (const juce::File& midi)
{
    content->confirmDiscard ([safe = juce::Component::SafePointer<TrackerWindow> (this), midi] { if (safe != nullptr) safe->content->importMidi (midi); });
}

void TrackerWindow::setupForTest (const juce::File& tune, int row, int voice, bool play, const juce::String& keys, const juce::File& exportSid)
{
    if (tune.existsAsFile() && tune.hasFileExtension ("mid;midi"))
    {
        auto r = npImportMidi (tune, BinaryData::np_v20g4_prg, (size_t) BinaryData::np_v20g4_prgSize);
        if (r.ok) content->loadForTest (r.tune, {});
    }
    else if (tune.existsAsFile())
    {
        NpTune t; juce::String err;
        if (t.loadFile (tune, err)) content->loadForTest (t, tune);
    }
    content->pattern->setCursorForTest (voice, row);
    // keys: typed into the pattern like at the keyboard; "_" = cursor down, "^" = cursor up, ">" = right, "<" = left, "#" = Return, "~" = Delete
    for (auto c : keys)
    {
        int code = c;
        if (c == '_') code = juce::KeyPress::downKey; else if (c == '^') code = juce::KeyPress::upKey;
        else if (c == '>') code = juce::KeyPress::rightKey; else if (c == '<') code = juce::KeyPress::leftKey;
        else if (c == '#') code = juce::KeyPress::returnKey; else if (c == '~') code = juce::KeyPress::deleteKey;
        content->pattern->keyPressed (juce::KeyPress (code, juce::ModifierKeys(), (juce::juce_wchar) (code == (int) c ? c : 0)));
    }
    if (exportSid != juce::File())
    {
        const auto psid = content->tune.toPsid ("Test", "SIDiMIDI", "");
        exportSid.replaceWithData (psid.getData(), psid.getSize());
    }
    if (play) content->play (false);
}

TrackerWindow::~TrackerWindow()
{
    if (content != nullptr) content->stopOnClose();
}

void TrackerWindow::closeButtonPressed()
{
    content->closeRequested ([this]
    {
        content->stopOnClose();
        setVisible (false);
        if (onClosed) juce::MessageManager::callAsync ([cb = onClosed] { cb(); });
    });
}

} // namespace sm
