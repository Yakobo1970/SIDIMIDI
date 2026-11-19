#include "MidiTab.h"
#include "MainComponent.h"
#include "SidTab.h"
#include "AudioPlayerEngine.h"
#include "MidiToSidCore.h"
#include "SidInfo.h"
#include "SidTrace.h"

namespace sm
{
using ui::font;

namespace
{
enum MidiCol { mPlay = 1, mName, mConvert, mResult, mTracks, mNotes, mLength, mFolder };

struct MidiColDef { int id; const char* name; int width; bool numeric; const char* tip; };
const MidiColDef kMidiCols[] = {
    { mPlay,    "▶",             38,  false, "MIDI-Datei vorhören (über den Windows-GM-Synth)" },
    { mName,    "Datei",         215, false, "Dateiname" },
    { mConvert, "Konvertieren",  272, false, "Konvertierungstiefen: Schnell (automatisch), Gezielt (Spuren und Rollen wählen), Rundlauf (nur für von SIDiMIDI erzeugte MIDIs)" },
    { mTracks,  "Spuren",        54,  true,  "Spuren mit Noten" },
    { mNotes,   "Noten",         58,  true,  "Anzahl der Noten" },
    { mLength,  "Länge",         56,  true,  "Spieldauer" },
    { mResult,  "Ergebnis",      240, false, "Die zuletzt geschriebene .sid-Datei – ▶ spielt sie ab" },
    { mFolder,  "Ordner",        180, false, "Unterordner" } };

bool hasSidmSignature (juce::MidiFile& mf)
{
    auto* t0 = mf.getTrack (0);
    if (t0 == nullptr) return false;
    for (int i = 0; i < t0->getNumEvents(); ++i)
    {
        const auto& m = t0->getEventPointer (i)->message;
        if (m.isMetaEvent() && m.getMetaEventType() == 0x7f && m.getMetaEventLength() >= 4)
        {
            auto* d = m.getMetaEventData();
            if (d[0] == 'S' && d[1] == 'I' && d[2] == 'D' && d[3] == 'M') return true;
        }
    }
    return false;
}

MidiItem readItem (const juce::File& f, const juce::File& root)
{
    MidiItem it;
    it.file = f;
    it.name = f.getFileName();
    it.folder = f.getParentDirectory() == root ? juce::String() : f.getParentDirectory().getRelativePathFrom (root).replaceCharacter ('\\', '/');
    juce::FileInputStream in (f);
    juce::MidiFile mf;
    if (! in.openedOk() || ! mf.readFrom (in)) return it;
    it.roundTrip = hasSidmSignature (mf);
    mf.convertTimestampTicksToSeconds();
    double end = 0;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        auto* tr = mf.getTrack (t);
        int n = 0;
        for (int i = 0; i < tr->getNumEvents(); ++i) n += tr->getEventPointer (i)->message.isNoteOn() ? 1 : 0;
        if (n > 0) { ++it.tracks; it.notes += n; }
        end = std::max (end, tr->getEndTime());
    }
    it.seconds = end;
    it.valid = it.notes > 0;
    return it;
}
} // namespace

struct MidiTab::Scan
{
    std::thread thread;
    std::atomic<bool> stop { false };
    ~Scan() { stop = true; if (thread.joinable()) thread.join(); }
};

// ================================================================================================ cell with buttons
class MidiCell : public juce::Component
{
public:
    MidiCell (MidiTab& t, int c) : tab (t), col (c)
    {
        for (auto* b : { &b1, &b2, &b3 }) addAndMakeVisible (b);
        b1.onClick = [this] { tab.convert (item, MidiTab::Depth::quick); };
        b2.onClick = [this] { tab.convert (item, MidiTab::Depth::targeted); };
        b3.onClick = [this] { tab.convert (item, MidiTab::Depth::roundTrip); };
        play.onClick = [this] { if (col == mPlay) tab.playMidi (item); else tab.playSidResult (item); };
        addAndMakeVisible (play);
    }

    void setItem (int idx)
    {
        item = idx;
        const auto* m = tab.item (idx);
        const bool ok = m != nullptr && m->valid;
        b1.setButtonText (T ("Schnell"));
        b2.setButtonText (T ("Gezielt …"));
        b3.setButtonText (T ("Rundlauf"));
        b1.setTooltip (T ("Automatisch: die drei notenreichsten Spuren, Klang nach GM-Programm"));
        b2.setTooltip (T ("Spuren und Rollen selbst wählen und vorhören"));
        b3.setTooltip (T ("Nur für MIDIs, die SIDiMIDI aus einer SID-Datei erzeugt hat: Spur 1–3 direkt zurückwandeln"));
        b1.setEnabled (ok); b2.setEnabled (ok); b3.setEnabled (ok && m->roundTrip);
        {
            auto* l = dynamic_cast<ui::Look*> (&getLookAndFeel());
            const auto& th = l != nullptr ? l->theme() : themes()[0];
            b1.setColour (juce::TextButton::buttonColourId, th.track[2].withMultipliedBrightness (0.55f));
            b2.setColour (juce::TextButton::buttonColourId, th.track[1].withMultipliedBrightness (0.55f));
            b3.setColour (juce::TextButton::buttonColourId, th.track[0].withMultipliedBrightness (0.55f));
            for (auto* b : { &b1, &b2, &b3 }) b->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        }
        const bool hasResult = m != nullptr && m->result != juce::File();
        play.setVisible (col == mPlay || hasResult);
        play.setButtonText (juce::String::fromUTF8 ("▶"));
        play.setEnabled (ok || hasResult);
        for (auto* b : { &b1, &b2, &b3 }) b->setVisible (col == mConvert);
        resized();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (2, 2);
        if (col == mConvert)
        {
            const int w = (r.getWidth() - 8) / 3;
            b1.setBounds (r.removeFromLeft (w)); r.removeFromLeft (4);
            b2.setBounds (r.removeFromLeft (w)); r.removeFromLeft (4);
            b3.setBounds (r);
        }
        else if (col == mPlay) play.setBounds (r.withSizeKeepingCentre (std::min (30, r.getWidth()), r.getHeight()));
        else play.setBounds (r.removeFromLeft (28));
    }

    void paint (juce::Graphics& g) override
    {
        if (col != mResult) return;
        const auto* m = tab.item (item);
        if (m == nullptr || m->result == juce::File()) return;
        auto* l = dynamic_cast<ui::Look*> (&getLookAndFeel());
        const auto& th = l != nullptr ? l->theme() : themes()[0];
        g.setColour (th.track[2]);
        g.setFont (font (13.0f));
        g.drawText (m->result.getFileName(), getLocalBounds().withTrimmedLeft (34), juce::Justification::centredLeft, true);
    }

    MidiTab& tab;
    int col, item = -1;
    juce::TextButton b1, b2, b3, play;
};

// ================================================================================================ track panel ("Gezielt")
class TrackPickPanel : public Overlay, private juce::ListBoxModel
{
public:
    using Track = MidiToSidCoreEngine::ChannelSuggestion;

    TrackPickPanel (MainComponent& m, MidiTab& t, int idx, std::vector<Track> tr, std::array<int, 3> pre)
        : Overlay (m), tab (t), itemIndex (idx), tracks (std::move (tr))
    {
        boxWidth = 880; boxHeight = 600;
        voiceOf.assign (tracks.size(), 0);
        roleOf.assign (tracks.size(), 0);
        for (int v = 0; v < 3; ++v)
            for (size_t i = 0; i < tracks.size(); ++i)
                if (tracks[i].partId() == pre[(size_t) v]) voiceOf[i] = v + 1;
        list.setModel (this);
        list.setRowHeight (34);
        addAndMakeVisible (list);
        convertBtn.setButtonText (T ("Konvertieren"));
        cancelBtn.setButtonText (T ("Abbrechen"));
        stopBtn.setButtonText (juce::String::fromUTF8 ("■ ") + T ("Vorhören stoppen"));
        convertBtn.onClick = [this] { run(); };
        cancelBtn.onClick = [this] { main.player().stop(); closeLater(); };
        stopBtn.onClick = [this] { main.player().stop(); };
        for (auto* b : { &convertBtn, &cancelBtn, &stopBtn }) addAndMakeVisible (b);
        help.setText (T ("Wähle bis zu drei Spuren für die drei SID-Stimmen (Stimme 1 ist die tiefste, 3 die höchste) und gib jeder eine Rolle. Mit ▶ lässt sich jede Spur einzeln vorhören."), juce::dontSendNotification);
        help.setFont (font (13.0f));
        help.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (help);
        updateButton();
    }

    void paint (juce::Graphics& g) override
    {
        Overlay::paint (g);
        const auto& th = theme();
        auto b = box().reduced (18);
        g.setColour (th.text);
        g.setFont (font (20.0f, true));
        g.drawText (T ("Gezielt konvertieren: %1", tab.item (itemIndex) != nullptr ? tab.item (itemIndex)->name : juce::String()), b.removeFromTop (30), juce::Justification::centredLeft, true);
    }

    void layout() override
    {
        auto b = box().reduced (18);
        b.removeFromTop (34);
        help.setBounds (b.removeFromTop (40));
        auto bottom = b.removeFromBottom (34);
        convertBtn.setBounds (bottom.removeFromRight (150)); bottom.removeFromRight (8);
        cancelBtn.setBounds (bottom.removeFromRight (110)); bottom.removeFromRight (8);
        stopBtn.setBounds (bottom.removeFromLeft (190));
        b.removeFromBottom (8);
        list.setBounds (b);
    }

    // ---- list rows
    int getNumRows() override { return (int) tracks.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        const auto& th = theme();
        g.setColour (ui::rowColour (th, row));
        g.fillRect (0, 0, w, h);
        if (row < 0 || row >= (int) tracks.size()) return;
        const int v = voiceOf[(size_t) row];
        if (v > 0) { g.setColour (voiceColour (v - 1).withAlpha (0.22f)); g.fillRect (0, 0, w, h); }
        g.setColour (th.text);
        g.setFont (font (13.0f));
        g.drawText (tracks[(size_t) row].label, 50, 0, w - 50 - 330, h, juce::Justification::centredLeft, true);
    }

    juce::Component* refreshComponentForRow (int row, bool, juce::Component* existing) override
    {
        if (row < 0 || row >= (int) tracks.size()) { delete existing; return nullptr; }
        auto* c = dynamic_cast<Row*> (existing);
        if (c == nullptr) { delete existing; c = new Row (*this); }
        c->set (row);
        return c;
    }

    struct Row : public juce::Component
    {
        explicit Row (TrackPickPanel& p) : panel (p)
        {
            addAndMakeVisible (play); addAndMakeVisible (voice); addAndMakeVisible (role);
            play.setButtonText (juce::String::fromUTF8 ("▶"));
            voice.addItem (juce::String::fromUTF8 ("—"), 1);
            for (int i = 1; i <= 3; ++i) voice.addItem (T ("Stimme %1", i), i + 1);
            role.addItem (T ("Automatisch"), 1); role.addItem ("Lead", 2); role.addItem ("Bass", 3); role.addItem ("Pad", 4); role.addItem (T ("Schlagzeug"), 5);
            play.onClick = [this] { panel.main.player().previewMidiTrack (panel.tab.item (panel.itemIndex)->file, panel.tracks[(size_t) row].trackIndex, nullptr, panel.tracks[(size_t) row].channelFilter); };
            voice.onChange = [this]
            {
                const int v = voice.getSelectedId() - 1;
                if (v > 0) for (size_t i = 0; i < panel.voiceOf.size(); ++i) if (panel.voiceOf[i] == v) panel.voiceOf[i] = 0;
                panel.voiceOf[(size_t) row] = v;
                panel.list.updateContent();
                panel.list.repaint();
                panel.updateButton();
            };
            role.onChange = [this] { panel.roleOf[(size_t) row] = role.getSelectedId() - 1; };
        }
        void set (int r)
        {
            row = r;
            voice.setSelectedId (panel.voiceOf[(size_t) r] + 1, juce::dontSendNotification);
            role.setSelectedId (panel.roleOf[(size_t) r] + 1, juce::dontSendNotification);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (2, 3);
            play.setBounds (r.removeFromLeft (38)); r.removeFromLeft (4);
            role.setBounds (r.removeFromRight (140)); r.removeFromRight (6);
            voice.setBounds (r.removeFromRight (110));
        }
        TrackPickPanel& panel;
        int row = 0;
        juce::TextButton play;
        juce::ComboBox voice, role;
    };

    void updateButton()
    {
        int n = 0;
        for (int v : voiceOf) n += v > 0;
        convertBtn.setEnabled (n > 0);
    }

    void run()
    {
        std::array<int, 3> trackIdx { -1, -1, -1 }, roles { 0, 0, 0 };
        for (size_t i = 0; i < tracks.size(); ++i)
            if (voiceOf[i] > 0) { trackIdx[(size_t) voiceOf[i] - 1] = tracks[i].partId(); roles[(size_t) voiceOf[i] - 1] = roleOf[i]; }
        main.player().stop();
        tab.convertTracks (itemIndex, trackIdx, roles, false);
        closeLater();
    }

private:
    MidiTab& tab;
    int itemIndex;
    std::vector<Track> tracks;
    std::vector<int> voiceOf, roleOf;
    juce::ListBox list;
    juce::TextButton convertBtn, cancelBtn, stopBtn;
    juce::Label help;
};

// ================================================================================================ MidiTab
MidiTab::MidiTab (MainComponent& m) : main (m), table ("midis", this)
{
    addAndMakeVisible (table);
    table.setRowHeight (30);
    table.setHeaderHeight (28);
    table.setMultipleSelectionEnabled (false);
    int flags = juce::TableHeaderComponent::resizable | juce::TableHeaderComponent::sortable | juce::TableHeaderComponent::visible;
    for (auto& c : kMidiCols)
        table.getHeader().addColumn (T (c.name), c.id, c.width, 24, 700, c.id == mConvert || c.id == mPlay || c.id == mResult ? (flags & ~juce::TableHeaderComponent::sortable) : flags);
    table.getHeader().setSortColumnId (sortCol, sortForward);
    table.getHeader().setStretchToFitActive (false);
    for (auto* b : { &openBtn, &recentBtn, &rescanBtn, &stopBtn, &fundusBtn }) addAndMakeVisible (b);
    addAndMakeVisible (subfolders);
    addAndMakeVisible (search);
    addAndMakeVisible (description);
    addAndMakeVisible (pathLabel);
    addAndMakeVisible (fundusText);
    description.setMultiLine (true);
    description.setReadOnly (true);
    description.setScrollbarsShown (true);
    description.setCaretVisible (false);
    description.setFont (font (13.5f));
    description.setReturnKeyStartsNewLine (true);
    fundusText.setFont (font (13.0f));
    fundusText.setMinimumHorizontalScale (1.0f);
    fundusText.setJustificationType (juce::Justification::topLeft);
    pathLabel.setFont (font (12.5f));
    pathLabel.setJustificationType (juce::Justification::centredLeft);

    openBtn.onClick = [this]
    {
        main.chooseFolder (T ("Ordner mit MIDI-Dateien öffnen"), [this] (const juce::File& f) { openFolder (f); }, "*.mid;*.midi;*.kar;*.rmi", "lastMidiFolder");
    };
    recentBtn.onClick = [this] { showRecentMenu(); };
    rescanBtn.onClick = [this] { startScan(); };
    stopBtn.onClick = [this] { stopPreview(); };
    fundusBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3b6fd6));
    fundusBtn.onClick = [] { juce::URL (AboutPanel::kMidiFundusUrl).launchInDefaultBrowser(); };
    subfolders.setToggleState ((bool) main.archive().settings.getProperty ("midiSubfolders", true), juce::dontSendNotification);
    subfolders.onClick = [this] { main.archive().settings.setProperty ("midiSubfolders", subfolders.getToggleState(), nullptr); main.notifySettings(); if (folder.isDirectory()) startScan(); };
    search.onTextChange = [this] { rebuildShown(); };
    search.setIndents (6, 4);
    applyLanguage();

    const juce::File last (main.archive().settings.getProperty ("midiFolder").toString());
    if (last.isDirectory()) openFolder (last);
    startTimerHz (10);
}

MidiTab::~MidiTab()
{
    stopTimer();
    stopScan();
    main.player().stop();
}

void MidiTab::applyLanguage()
{
    openBtn.setButtonText (T ("Ordner öffnen …"));
    recentBtn.setButtonText (juce::String::fromUTF8 ("▾"));
    rescanBtn.setButtonText (T ("Neu lesen"));
    stopBtn.setButtonText (juce::String::fromUTF8 ("■"));
    subfolders.setButtonText (T ("Unterordner"));
    fundusBtn.setButtonText (T ("Midi-Fundus auf undabtanzbar.de"));
    openBtn.setTooltip (T ("Ordner mit MIDI-Dateien öffnen (auch per Ziehen ins Fenster)"));
    recentBtn.setTooltip (T ("Zuletzt geöffnete Ordner"));
    rescanBtn.setTooltip (T ("Den Ordner neu einlesen"));
    stopBtn.setTooltip (T ("Vorhören stoppen"));
    subfolders.setTooltip (T ("Auch die Unterordner nach MIDI-Dateien durchsuchen"));
    fundusBtn.setTooltip (AboutPanel::kMidiFundusUrl);
    search.setTextToShowWhenEmpty (T ("Filter nach Dateiname oder Ordner …"), juce::Colours::grey);
    fundusText.setText (T ("Viele MIDI-Dateien? Midi-Fundus verwaltet ganze MIDI-Sammlungen: Tonart, Skala, Dubletten-Suche, Vorhören mit GM-Klang, über 20 Formate und Konvertierung nach Ableton, MusicXML und mehr. Kostenlos, ebenfalls von Un(d)AbtanzBAR."), juce::dontSendNotification);
    description.setText (T ("@midi"), false);
    description.moveCaretToTop (false);
    auto& hdr = table.getHeader();
    for (auto& c : kMidiCols) hdr.setColumnName (c.id, T (c.name));
    resized();
    table.updateContent();
}

void MidiTab::applyTheme() { repaint(); }

void MidiTab::saveState() { main.archive().settings.setProperty ("midiFolder", folder.getFullPathName(), nullptr); main.notifySettings(); }

juce::String MidiTab::statusSummary() const
{
    if (folder == juce::File()) return T ("Noch kein Ordner geöffnet");
    juce::String s = T ("%1 MIDI-Dateien  ·  %2 angezeigt", (int) items.size(), (int) shown.size());
    if (! scanFinished) s << U ("  ·  ") << T ("liest … %1", scanTotal.load());
    if (lastMessage.isNotEmpty()) s << U ("  ·  ") << lastMessage;
    return s;
}

// ---- layout
void MidiTab::resized()
{
    auto r = getLocalBounds();
    toolbarArea = r.removeFromTop (46);
    auto h = toolbarArea.reduced (10, 7);
    openBtn.setBounds (h.removeFromLeft (130)); h.removeFromLeft (2);
    recentBtn.setBounds (h.removeFromLeft (30)); h.removeFromLeft (6);
    rescanBtn.setBounds (h.removeFromLeft (92)); h.removeFromLeft (10);
    subfolders.setBounds (h.removeFromLeft (120)); h.removeFromLeft (10);
    stopBtn.setBounds (h.removeFromRight (34)); h.removeFromRight (6);
    search.setBounds (h.removeFromRight (std::min (320, h.getWidth() / 2))); h.removeFromRight (10);
    pathLabel.setBounds (h);

    // the table uses the whole width (the result column needs the room); the explanation sits below it
    const int bottomH = juce::jlimit (170, 300, r.getHeight() * 32 / 100);
    auto bottom = r.removeFromBottom (bottomH).reduced (8, 8);
    table.setBounds (r);
    infoBox = bottom.removeFromRight (juce::jlimit (300, 420, bottom.getWidth() / 3));
    bottom.removeFromRight (8);
    description.setBounds (bottom);
    auto inner = infoBox.reduced (12, 10);
    fundusBtn.setBounds (inner.removeFromBottom (30));
    inner.removeFromBottom (6);
    fundusText.setBounds (inner);
}

void MidiTab::paint (juce::Graphics& g)
{
    const auto& th = main.theme();
    g.fillAll (th.bg);
    g.setColour (th.panel);
    g.fillRect (toolbarArea);
    g.setColour (th.panel);
    g.fillRoundedRectangle (infoBox.toFloat(), 8.0f);
    g.setColour (th.track[1].withAlpha (0.8f));
    g.drawRoundedRectangle (infoBox.toFloat().reduced (0.5f), 8.0f, 1.5f);
    if (items.empty())
    {
        g.setColour (th.dim);
        g.setFont (font (15.0f));
        auto area = table.getBounds().reduced (30, 60).withHeight (120);
        g.drawFittedText (folder == juce::File() ? T ("Noch kein Ordner geöffnet. „Ordner öffnen …“ klicken oder einen Ordner mit MIDI-Dateien ins Fenster ziehen.")
                                                  : (scanFinished ? T ("In diesem Ordner liegen keine MIDI-Dateien.") : T ("Ordner wird gelesen …")),
                          area, juce::Justification::centred, 4);
    }
}

bool MidiTab::handleKey (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { stopPreview(); return true; }
    if (k == juce::KeyPress::spaceKey && ! search.hasKeyboardFocus (true))
    {
        const int row = table.getSelectedRow();
        if (const int i = itemOfRow (row); i >= 0) { playMidi (i); return true; }
    }
    if (k == juce::KeyPress ('f', juce::ModifierKeys::ctrlModifier, 0)) { search.grabKeyboardFocus(); return true; }
    return false;
}

// ---- folder and scanning
void MidiTab::openFolder (const juce::File& dir)
{
    if (! dir.isDirectory()) return;
    folder = dir;
    pathLabel.setText (dir.getFullPathName(), juce::dontSendNotification);
    auto recent = juce::StringArray::fromLines (main.archive().settings.getProperty ("midiRecent").toString());
    recent.removeString (dir.getFullPathName());
    recent.insert (0, dir.getFullPathName());
    while (recent.size() > 10) recent.remove (recent.size() - 1);
    main.archive().settings.setProperty ("midiRecent", recent.joinIntoString ("\n"), nullptr);
    saveState();
    startScan();
}

void MidiTab::stopScan()
{
    if (scan != nullptr) { scan->stop = true; scan.reset(); }
}

void MidiTab::startScan()
{
    stopScan();
    items.clear();
    shown.clear();
    { const juce::ScopedLock sl (pendingLock); pending.clear(); }
    table.updateContent();
    scanFinished = false;
    scanTotal = 0;
    lastMessage = {};
    scan = std::make_unique<Scan>();
    auto* s = scan.get();
    const auto root = folder;
    const bool recursive = subfolders.getToggleState();
    s->thread = std::thread ([this, s, root, recursive]
    {
        std::vector<MidiItem> batch;
        for (const auto& de : juce::RangedDirectoryIterator (root, recursive, "*.mid;*.midi;*.kar;*.rmi", juce::File::findFiles))
        {
            if (s->stop) return;
            batch.push_back (readItem (de.getFile(), root));
            ++scanTotal;
            if (batch.size() >= 25)
            {
                const juce::ScopedLock sl (pendingLock);
                for (auto& b : batch) pending.push_back (std::move (b));
                batch.clear();
            }
        }
        if (s->stop) return;
        {
            const juce::ScopedLock sl (pendingLock);
            for (auto& b : batch) pending.push_back (std::move (b));
        }
        scanFinished = true;
    });
    repaint();
}

void MidiTab::timerCallback()
{
    std::vector<MidiItem> got;
    {
        const juce::ScopedLock sl (pendingLock);
        got.swap (pending);
    }
    if (! got.empty())
    {
        for (auto& g : got) items.push_back (std::move (g));
        rebuildShown();
    }
    else if (scanFinished && scan != nullptr)
    {
        scan.reset();
        rebuildShown();
        repaint();
    }
    // end of a preview
    if (playingItem >= 0 && ! main.player().isPlaying()) { playingItem = -1; table.repaint(); }
}

void MidiTab::rebuildShown()
{
    const auto f = search.getText().trim();
    const int keep = itemOfRow (table.getSelectedRow());
    shown.clear();
    for (size_t i = 0; i < items.size(); ++i)
        if (f.isEmpty() || items[i].name.containsIgnoreCase (f) || items[i].folder.containsIgnoreCase (f)) shown.push_back ((int) i);
    const int col = sortCol;
    const bool fwd = sortForward;
    std::stable_sort (shown.begin(), shown.end(), [&] (int a, int b)
    {
        const auto& x = items[(size_t) a];
        const auto& y = items[(size_t) b];
        int c = 0;
        switch (col)
        {
            case mTracks: c = x.tracks - y.tracks; break;
            case mNotes: c = x.notes - y.notes; break;
            case mLength: c = x.seconds < y.seconds ? -1 : (x.seconds > y.seconds ? 1 : 0); break;
            case mFolder: c = x.folder.compareNatural (y.folder, false); break;
            default: c = (x.folder + "/" + x.name).compareNatural (y.folder + "/" + y.name, false); break;
        }
        return fwd ? c < 0 : c > 0;
    });
    table.updateContent();
    for (size_t r = 0; r < shown.size(); ++r) if (shown[r] == keep) table.selectRow ((int) r);
    table.repaint();
    repaint();
}

void MidiTab::showRecentMenu()
{
    juce::PopupMenu m;
    const auto recent = juce::StringArray::fromLines (main.archive().settings.getProperty ("midiRecent").toString());
    for (int i = 0; i < recent.size(); ++i) if (recent[i].isNotEmpty()) m.addItem (1 + i, recent[i]);
    if (m.getNumItems() == 0) m.addItem (-1, T ("Noch keine Ordner geöffnet"), false);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&recentBtn), [this, recent] (int r) { if (r > 0) openFolder (juce::File (recent[r - 1])); });
}

// ---- table
void MidiTab::paintRowBackground (juce::Graphics& g, int row, int w, int h, bool selected)
{
    const auto& th = main.theme();
    auto c = ui::rowColour (th, row);
    if (selected) c = th.accent.withAlpha (0.38f);
    g.setColour (c);
    g.fillRect (0, 0, w, h);
    if (itemOfRow (row) == playingItem && playingItem >= 0)
    {
        g.setColour (th.track[2].withAlpha (0.16f));
        g.fillRect (0, 0, w, h);
    }
}

void MidiTab::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool)
{
    const auto* m = item (itemOfRow (row));
    if (m == nullptr) return;
    const auto& th = main.theme();
    juce::String text;
    switch (col)
    {
        case mName: text = m->name; break;
        case mTracks: text = m->valid ? juce::String (m->tracks) : juce::String(); break;
        case mNotes: text = m->valid ? juce::String (m->notes) : juce::String(); break;
        case mLength: text = m->valid ? formatTime (m->seconds) : juce::String(); break;
        case mFolder: text = m->folder; break;
        default: return;
    }
    g.setColour (m->valid ? th.text : th.dim);
    g.setFont (font (13.0f));
    if (! m->valid && col == mName) text << U ("   (") << T ("keine Noten oder nicht lesbar") << ")";
    g.drawText (text, juce::Rectangle<int> (0, 0, w, h).reduced (5, 0), (col == mTracks || col == mNotes || col == mLength) ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
    if (m->roundTrip && col == mName)
    {
        g.setColour (th.track[6]);
        g.setFont (font (11.0f, true));
        g.drawText ("SIDM", w - 50, 0, 46, h, juce::Justification::centredRight, false);
    }
}

juce::Component* MidiTab::refreshComponentForCell (int row, int col, bool, juce::Component* existing)
{
    if (col != mConvert && col != mPlay && col != mResult) { delete existing; return nullptr; }
    auto* c = dynamic_cast<MidiCell*> (existing);
    if (c == nullptr || c->col != col) { delete existing; c = new MidiCell (*this, col); }
    c->setItem (itemOfRow (row));
    return c;
}

void MidiTab::cellDoubleClicked (int row, int col, const juce::MouseEvent&)
{
    if (col == mName) convert (itemOfRow (row), Depth::quick);
}

// ---- preview
void MidiTab::stopPreview()
{
    main.player().stop();
    playingItem = -1;
    table.repaint();
}

void MidiTab::playMidi (int idx)
{
    const auto* m = item (idx);
    if (m == nullptr) return;
    if (playingItem == idx && main.player().isPlaying()) { stopPreview(); return; }
    main.sidTab().stopPlayback();
    juce::String log;
    if (main.player().playMidiFile (m->file, &log)) playingItem = idx;
    else main.setStatus (T ("MIDI-Datei lässt sich nicht abspielen"));
    table.repaint();
}

void MidiTab::playSidResult (int idx)
{
    const auto* m = item (idx);
    if (m == nullptr || m->result == juce::File() || ! m->result.existsAsFile()) return;
    main.sidTab().stopPlayback();
    const auto path = m->result.getFullPathName();
    juce::Component::SafePointer<MidiTab> safe (this);
    main.jobs().addJob ([safe, path, idx]
    {
        const auto si = parseSidFile (juce::File (path), true);
        auto t = computeTrace (si, 1, 600.0);
        juce::MessageManager::callAsync ([safe, t, si, idx]
        {
            if (safe == nullptr) return;
            if (t == nullptr || t->empty()) { safe->main.setStatus (T ("Die SID-Datei lässt sich nicht abspielen")); return; }
            safe->main.player().playTrace (*t->events, t->fps, t->frames, 0, si.ntsc());
            safe->playingItem = idx;
        });
    });
}

// ---- conversion
void MidiTab::convert (int idx, Depth depth)
{
    const auto* m = item (idx);
    if (m == nullptr || ! m->valid) return;
    main.player().stop();
    playingItem = -1;
    juce::String log;
    bool round = false;
    std::array<MidiToSidCoreEngine::ChannelSuggestion, 3> sug;
    if (! MidiToSidCoreEngine::analyzeMidiFile (m->file, round, sug, log))
    {
        main.setStatus (T ("Die MIDI-Datei lässt sich nicht analysieren"));
        return;
    }
    if (depth == Depth::quick)
    {
        convertTracks (idx, { sug[0].partId(), sug[1].partId(), sug[2].partId() }, { 0, 0, 0 }, round);
    }
    else if (depth == Depth::roundTrip)
    {
        if (! m->roundTrip) { main.setStatus (T ("Rundlauf geht nur mit MIDI-Dateien, die SIDiMIDI aus einer SID-Datei erzeugt hat")); return; }
        convertTracks (idx, { 1, 2, 3 }, { 0, 0, 0 }, true);
    }
    else
    {
        std::vector<MidiToSidCoreEngine::ChannelSuggestion> all;
        if (! MidiToSidCoreEngine::listAllTracks (m->file, all, log) || all.empty())
        {
            main.setStatus (T ("Keine Spuren mit Noten gefunden"));
            return;
        }
        main.showOverlay (std::make_unique<TrackPickPanel> (main, *this, idx, std::move (all), std::array<int, 3> { sug[0].partId(), sug[1].partId(), sug[2].partId() }));
    }
}

void MidiTab::convertTracks (int idx, const std::array<int, 3>& tracks, const std::array<int, 3>& roles, bool roundTrip)
{
    // pressing a conversion depth opens the save dialog (suggestion: the opened MIDI folder); the conversion then runs at once
    const auto* m = item (idx);
    if (m == nullptr) return;
    const auto suggested = (folder.isDirectory() ? folder : m->file.getParentDirectory())
                               .getChildFile (juce::File::createLegalFileName (m->file.getFileNameWithoutExtension()) + ".sid");
    juce::Component::SafePointer<MidiTab> safe (this);
    main.chooseSaveFile (T ("SID-Datei speichern unter"), suggested, "*.sid", [safe, idx, tracks, roles, roundTrip] (const juce::File& chosen)
    {
        if (safe == nullptr) return;
        safe->runConversion (idx, tracks, roles, roundTrip, chosen.hasFileExtension ("sid") ? chosen : chosen.withFileExtension ("sid"));
    });
}

void MidiTab::runConversion (int idx, const std::array<int, 3>& tracks, const std::array<int, 3>& roles, bool roundTrip, const juce::File& target)
{
    const auto* m = item (idx);
    if (m == nullptr) return;
    if (main.archive().isProtected (target.getParentDirectory().getFullPathName()))
    {
        main.setStatus (T ("Der Zielordner ist schreibgeschützt: %1", target.getParentDirectory().getFullPathName()));
        return;
    }
    const auto src = m->file;
    main.setStatus (T ("Konvertiere %1 …", src.getFileName()));
    std::array<MidiToSidCoreEngine::VoiceRole, 3> vr;
    for (size_t i = 0; i < 3; ++i) vr[i] = (MidiToSidCoreEngine::VoiceRole) juce::jlimit (0, 4, roles[i]);
    juce::Component::SafePointer<MidiTab> safe (this);
    main.jobs().addJob ([safe, src, target, tracks, roundTrip, vr, idx]
    {
        juce::String log;
        target.getParentDirectory().createDirectory();
        const bool ok = MidiToSidCoreEngine::convertMidiToSidFile (src, target, tracks, roundTrip, log, vr);
        juce::MessageManager::callAsync ([safe, ok, target, log, idx]
        {
            if (safe == nullptr) return;
            if (ok)
            {
                if (idx >= 0 && idx < (int) safe->items.size()) safe->items[(size_t) idx].result = target;
                safe->lastMessage = T ("SID geschrieben: %1", target.getFileName());
                safe->main.setStatus (T ("SID geschrieben: %1", target.getFullPathName()));
                auto& a = safe->main.archive();
                if (a.isInsideFolders (target)) a.addFile (target);
                safe->table.updateContent();
                safe->table.repaint();
            }
            else
                safe->main.setStatus (T ("Konvertierung fehlgeschlagen: %1", log.trim().fromLastOccurrenceOf ("\n", false, false)));
        });
    });
}
} // namespace sm
