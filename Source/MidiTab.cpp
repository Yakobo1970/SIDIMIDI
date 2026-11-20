#include "MidiTab.h"
#include "MainComponent.h"
#include "SidTab.h"
#include "AudioPlayerEngine.h"
#include "MidiToSidCore.h"
#include "SidInfo.h"
#include "InstrumentPool.h"
#include "NpFromMidi.h"
#include <BinaryData.h>
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
    { mConvert, "Konvertieren",  360, false, "Konvertierungstiefen: Schnell (automatisch), Gezielt (Spuren und Rollen wählen), Rundlauf (nur für von SIDiMIDI erzeugte MIDIs), Tracker (Tune mit Akkord-Arpeggios und Schlagzeug, im Vibrants-Tracker bearbeitbar)" },
    { mTracks,  "Spuren",        54,  true,  "Spuren mit Noten" },
    { mNotes,   "Noten",         58,  true,  "Anzahl der Noten" },
    { mLength,  "Länge",         56,  true,  "Spieldauer" },
    { mResult,  "Ergebnis",      240, false, "Die zuletzt geschriebene .sid-Datei – ▶ spielt sie ab" } };

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
    // an .sid with the same name next to the MIDI file is an earlier result of ours: offer it for playing
    if (const auto sid = f.withFileExtension ("sid"); sid.existsAsFile()) it.result = sid;
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
        for (auto* b : { &b1, &b2, &b3, &b4 }) addAndMakeVisible (b);
        b1.onClick = [this] { tab.convert (item, MidiTab::Depth::quick); };
        b2.onClick = [this] { tab.convert (item, MidiTab::Depth::targeted); };
        b3.onClick = [this] { tab.convert (item, MidiTab::Depth::roundTrip); };
        b4.onClick = [this] { tab.convert (item, MidiTab::Depth::tracker); };
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
        b4.setButtonText (T ("Tracker"));
        b4.setTooltip (T ("Umwandlung in einen Tune des Vibrants-Trackers: Akkorde als Arpeggio, Schlagzeug mit eigenen Klängen, Puls und Hard-Restart. Der Tune öffnet sich im Tracker (dort Anhören, Bearbeiten, Export als SID oder MIDI)"));
        b1.setTooltip (T ("Automatisch: die drei notenreichsten Spuren, Klang nach GM-Programm"));
        b2.setTooltip (T ("Spuren und Rollen selbst wählen und vorhören"));
        b3.setTooltip (T ("Nur für MIDIs, die SIDiMIDI aus einer SID-Datei erzeugt hat: Spur 1–3 direkt zurückwandeln"));
        b1.setEnabled (ok); b2.setEnabled (ok); b3.setEnabled (ok && m->roundTrip); b4.setEnabled (ok);
        {
            auto* l = dynamic_cast<ui::Look*> (&getLookAndFeel());
            const auto& th = l != nullptr ? l->theme() : themes()[0];
            b1.setColour (juce::TextButton::buttonColourId, th.track[2].withMultipliedBrightness (0.55f));
            b2.setColour (juce::TextButton::buttonColourId, th.track[1].withMultipliedBrightness (0.55f));
            b3.setColour (juce::TextButton::buttonColourId, th.track[0].withMultipliedBrightness (0.55f));
            b4.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f9e44));
            for (auto* b : { &b1, &b2, &b3, &b4 }) b->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        }
        const bool hasResult = m != nullptr && m->result != juce::File();
        play.setVisible (col == mPlay || hasResult);
        play.setButtonText (juce::String::fromUTF8 ("▶"));
        play.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f9e44));
        play.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        play.setEnabled (ok || hasResult);
        for (auto* b : { &b1, &b2, &b3, &b4 }) b->setVisible (col == mConvert);
        resized();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (2, 2);
        if (col == mConvert)
        {
            const int w = (r.getWidth() - 12) / 4;
            b1.setBounds (r.removeFromLeft (w)); r.removeFromLeft (4);
            b2.setBounds (r.removeFromLeft (w)); r.removeFromLeft (4);
            b3.setBounds (r.removeFromLeft (w)); r.removeFromLeft (4);
            b4.setBounds (r);
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
    juce::TextButton b1, b2, b3, b4, play;
};

// ================================================================================================ track panel ("Gezielt")
// choose the MIDI tracks for the three voices (target: a SID file or the Vibrants-Tracker); every track can be listened to,
// several at once, with the oscillators and a small picture of its notes over the whole length
class TrackPickPanel : public Overlay, private juce::ListBoxModel, private juce::Timer
{
public:
    using Track = MidiToSidCoreEngine::ChannelSuggestion;
    static constexpr int kMiniW = 190, kMiniH = 26;

    TrackPickPanel (MainComponent& m, MidiTab& t, int idx, std::vector<Track> tr, std::array<int, 3> pre, bool tracker = false)
        : Overlay (m), tab (t), itemIndex (idx), tracks (std::move (tr)), toTracker (tracker)
    {
        boxWidth = 960; boxHeight = 660;
        voiceOf.assign (tracks.size(), 0);
        roleOf.assign (tracks.size(), 0);
        prioOf.assign (tracks.size(), 0);
        presetOf.assign (tracks.size(), -1);
        kitOf.assign (tracks.size(), -1);
        if (toTracker)
        {
            kits = InstrumentPool::factoryKits();
            const auto myKits = InstrumentPool::userKits();
            kits.insert (kits.end(), myKits.begin(), myKits.end());
            presets = InstrumentPool::factoryPresets();
            const auto mine = InstrumentPool::userPresets();
            presets.insert (presets.end(), mine.begin(), mine.end());
        }
        active.assign (tracks.size(), 0);
        for (int v = 0; v < 3; ++v)
            for (size_t i = 0; i < tracks.size(); ++i)
                if (tracks[i].partId() == pre[(size_t) v]) voiceOf[i] = v + 1;
        if (const char* test = std::getenv ("SIDI_PICK_TEST"))       // test only: "1,1,2,0,1" = the OSC of every track
        {
            const auto parts = juce::StringArray::fromTokens (test, ",", "");
            for (int i = 0; i < parts.size() && i < (int) voiceOf.size(); ++i) voiceOf[(size_t) i] = parts[i].getIntValue();
            for (size_t i = 0; i < prioOf.size(); ++i) prioOf[i] = (int) i;
        }
        if (const char* test = std::getenv ("SIDI_PICK_PRESET"); test != nullptr && ! presets.empty())      // test only: preset index for track 0
            presetOf[0] = juce::String (test).getIntValue();
        normalizePrio();
        for (size_t i = 0; i < tracks.size(); ++i)         // drums that were recognised: role drums, in the tracker the first kit
            if (voiceOf[i] > 0 && tracks[i].drums) { roleOf[i] = 4; if (toTracker && ! kits.empty()) kitOf[i] = 0; }
        buildMinis();
        list.setModel (this);
        list.setRowHeight (34);
        addAndMakeVisible (list);
        convertBtn.setButtonText (toTracker ? T ("In den Tracker laden") : T ("Konvertieren"));
        cancelBtn.setButtonText (T ("Abbrechen"));
        playBtn.setButtonText (juce::String::fromUTF8 ("▶"));
        playBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f9e44));
        playBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        playBtn.setTooltip (T ("Spielt die Spuren, die einem OSC zugeordnet sind (ohne Zuordnung: alle Spuren)"));
        stopBtn.setButtonText (juce::String::fromUTF8 ("■"));
        stopBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffd0393a));
        stopBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        stopBtn.setTooltip (T ("Vorhören stoppen"));
        convertBtn.onClick = [this] { run(); };
        cancelBtn.onClick = [this] { stopAll(); closeLater(); };
        stopBtn.onClick = [this] { stopAll(); };
        playBtn.onClick = [this] { playAssigned(); };
        for (auto* b : { &convertBtn, &cancelBtn, &stopBtn, &playBtn }) addAndMakeVisible (b);
        for (int v = 0; v < 3; ++v) { scopes[v] = std::make_unique<ScopeBox> (main.player(), v); addAndMakeVisible (*scopes[v]); }
        if (toTracker)
        {
            sidSound.setButtonText (T ("Klang: SID"));
            sidSound.setTooltip (T ("Vorhören mit dem Klang der Konvertierung: Rollen und Presets werden mit der SID-Emulation gespielt. Aus: der GM-Synth des Computers spielt die Spuren"));
            sidSound.setToggleState ((bool) main.archive().settings.getProperty ("pickSidSound", true), juce::dontSendNotification);
            sidSound.onClick = [this]
            {
                main.archive().settings.setProperty ("pickSidSound", sidSound.getToggleState(), nullptr);
                main.notifySettings();
                if (anyActive()) { const bool was = main.player().isPlaying(); sidPlaying = false; applyPreview (was); }
            };
            addAndMakeVisible (sidSound);
        }
        timeLabel.setFont (font (15.0f, true));
        timeLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (timeLabel);
        help.setText (toTracker ? T ("Wähle bis zu drei Spuren für die drei Tracker-Oszillatoren (OSC1 ist der tiefste, OSC3 der höchste) und gib jeder eine Rolle. Mit ▶ schaltest du Spuren zum Hören ein und aus, auch mehrere zugleich – grün heißt hörbar.")
                                : T ("Wähle bis zu drei Spuren für die drei SID-Oszillatoren (OSC1 ist der tiefste, OSC3 der höchste) und gib jeder eine Rolle. Mit ▶ schaltest du Spuren zum Hören ein und aus, auch mehrere zugleich – grün heißt hörbar."),
                      juce::dontSendNotification);
        help.setFont (font (13.0f));
        help.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (help);
        updateButton();
        startTimerHz (10);
        updateTime();
    }

    ~TrackPickPanel() override
    {
        stopTimer();
        if (anyActive()) main.player().stop();
    }

    void paint (juce::Graphics& g) override
    {
        Overlay::paint (g);
        const auto& th = theme();
        auto b = box().reduced (18);
        g.setColour (th.text);
        g.setFont (font (20.0f, true));
        const auto name = tab.item (itemIndex) != nullptr ? tab.item (itemIndex)->name : juce::String();
        g.drawText (toTracker ? T ("Spuren für den Tracker wählen: %1", name) : T ("Gezielt konvertieren: %1", name), b.removeFromTop (30), juce::Justification::centredLeft, true);
    }

    void layout() override
    {
        auto b = box().reduced (18);
        b.removeFromTop (34);
        auto top = b.removeFromTop (50);
        playBtn.setBounds (top.removeFromLeft (46)); top.removeFromLeft (4);
        stopBtn.setBounds (top.removeFromLeft (46)); top.removeFromLeft (10);
        timeLabel.setBounds (top.removeFromLeft (120)); top.removeFromLeft (10);
        if (toTracker) { sidSound.setBounds (top.removeFromLeft (104).withSizeKeepingCentre (104, 24)); top.removeFromLeft (10); }
        const int sw = (top.getWidth() - 12) / 3;
        for (int v = 0; v < 3; ++v) { scopes[v]->setBounds (top.removeFromLeft (sw)); top.removeFromLeft (6); }
        b.removeFromTop (6);
        help.setBounds (b.removeFromTop (40));
        auto bottom = b.removeFromBottom (34);
        convertBtn.setBounds (bottom.removeFromRight (190)); bottom.removeFromRight (8);
        cancelBtn.setBounds (bottom.removeFromRight (110));
        b.removeFromBottom (8);
        list.setBounds (b);
    }

    // ---- list rows
    int getNumRows() override { return (int) tracks.size(); }
    int roleWidth() const { return 146 + 106; }
    int groupSize (int v) const { int n = 0; for (int x : voiceOf) n += x == v && v > 0; return n; }
    // priorities are ranks inside a voice: 0 .. n-1 without gaps, ties by row
    void normalizePrio()
    {
        for (int v = 0; v <= 3; ++v)
        {
            std::vector<size_t> idx;
            for (size_t i = 0; i < voiceOf.size(); ++i) if (voiceOf[i] == v) idx.push_back (i);
            std::stable_sort (idx.begin(), idx.end(), [this] (size_t a, size_t b) { return prioOf[a] < prioOf[b]; });
            for (size_t k = 0; k < idx.size(); ++k) prioOf[idx[k]] = v == 0 ? 0 : (int) k;
        }
    }
    void setRank (size_t row, int want)       // this track takes rank `want`; the others close up around it
    {
        std::vector<size_t> idx;
        for (size_t i = 0; i < voiceOf.size(); ++i) if (i != row && voiceOf[i] == voiceOf[row]) idx.push_back (i);
        std::stable_sort (idx.begin(), idx.end(), [this] (size_t a, size_t b) { return prioOf[a] < prioOf[b]; });
        idx.insert (idx.begin() + std::min ((size_t) std::max (want, 0), idx.size()), row);
        for (size_t k = 0; k < idx.size(); ++k) prioOf[idx[k]] = (int) k;
        list.updateContent();
        list.repaint();
    }
    // does another track play the same voice? Then the priority matters
    bool shared (size_t row) const
    {
        if (voiceOf[row] <= 0) return false;
        for (size_t i = 0; i < voiceOf.size(); ++i) if (i != row && voiceOf[i] == voiceOf[row]) return true;
        return false;
    }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        const auto& th = theme();
        g.setColour (ui::rowColour (th, row));
        g.fillRect (0, 0, w, h);
        if (row < 0 || row >= (int) tracks.size()) return;
        const int v = voiceOf[(size_t) row];
        if (v > 0) { g.setColour (voiceColour (v - 1).withAlpha (0.22f)); g.fillRect (0, 0, w, h); }
        const int mapX = w - roleWidth() - 116 - kMiniW - 8;
        g.setColour (th.text);
        g.setFont (font (13.0f));
        g.drawText (tracks[(size_t) row].label, 50, 0, mapX - 56, h, juce::Justification::centredLeft, true);
        // the notes of the track over the whole length of the piece; a line follows the playback
        const juce::Rectangle<int> map (mapX, (h - kMiniH) / 2, kMiniW, kMiniH);
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillRect (map);
        if ((size_t) row < minis.size() && minis[(size_t) row].isValid())
        {
            g.setColour (v > 0 ? voiceColour (v - 1) : th.dim.interpolatedWith (th.text, 0.55f));
            g.drawImageAt (minis[(size_t) row], map.getX(), map.getY(), true);
        }
        if (active[(size_t) row] != 0 && total > 0)
        {
            const double at = elapsedNow();
            g.setColour (juce::Colour (0xff2f9e44));
            g.fillRect (map.getX() + (int) std::lround (juce::jlimit (0.0, 1.0, at / total) * (kMiniW - 1)), map.getY(), 2, kMiniH);
        }
    }

    juce::Component* refreshComponentForRow (int row, bool, juce::Component* existing) override
    {
        if (row < 0 || row >= (int) tracks.size()) { delete existing; return nullptr; }
        auto* c = dynamic_cast<Row*> (existing);
        if (c == nullptr || c->version != menuVersion) { delete existing; c = new Row (*this); }
        c->set (row);
        return c;
    }

    struct Row : public juce::Component
    {
        explicit Row (TrackPickPanel& p) : panel (p), version (p.menuVersion)
        {
            addAndMakeVisible (play); addAndMakeVisible (voice); addAndMakeVisible (role); addAndMakeVisible (prio);
            prio.setTextWhenNothingSelected (T ("Priorität"));
            prio.setTooltip (T ("Spielen mehrere Spuren denselben OSC, gewinnt bei gleichzeitigen Noten die Spur mit der höheren Priorität; die anderen füllen nur deren Pausen. Die Klänge der Spuren bleiben unterschiedlich (nur im Tracker)"));
            prio.onChange = [this] { if (prio.getSelectedId() > 0) panel.setRank ((size_t) row, prio.getSelectedId() - 1); };
            play.setButtonText (juce::String::fromUTF8 ("▶"));
            play.setTooltip (T ("Diese Spur ein- oder ausschalten (hörbar = grün); mehrere Spuren gleichzeitig sind möglich"));
            voice.addItem (juce::String::fromUTF8 ("—"), 1);
            for (int i = 1; i <= 3; ++i) voice.addItem ("OSC" + juce::String (i), i + 1);
            role.addItem (T ("Automatisch"), 1); role.addItem ("Lead", 2); role.addItem ("Bass", 3); role.addItem ("Pad", 4); role.addItem (T ("Schlagzeug"), 5);
            play.onClick = [this] { panel.toggleTrack (row); };
            voice.onChange = [this]
            {
                const int v = voice.getSelectedId() - 1;
                panel.voiceOf[(size_t) row] = v;
                panel.prioOf[(size_t) row] = 1000;      // a track that joins a voice comes after the others
                panel.normalizePrio();
                panel.list.updateContent();
                panel.list.repaint();
                panel.updateButton();
            };
            if (panel.toTracker && ! panel.presets.empty())      // presets of the instrument pool in place of a role
            {
                auto* root = role.getRootMenu();
                root->addSeparator();
                juce::PopupMenu all;
                for (int c = 0; c < InstrumentPool::kNumCategories; ++c)
                {
                    juce::PopupMenu sub;
                    for (size_t i = 0; i < panel.presets.size(); ++i)
                        if (panel.presets[i].category == juce::String::fromUTF8 (InstrumentPool::kCategories[c]) || (c == InstrumentPool::kNumCategories - 1 && ! panel.presets[i].factory))
                            sub.addItem (100 + (int) i, panel.presets[i].name);
                    if (sub.getNumItems() > 0) all.addSubMenu (juce::String::fromUTF8 (InstrumentPool::kCategories[c]), sub);
                }
                root->addSubMenu (T ("Preset aus dem Instrumentenpool"), all);
                juce::PopupMenu kitMenu;
                for (size_t i = 0; i < panel.kits.size(); ++i) kitMenu.addItem (300 + (int) i, "Kit: " + panel.kits[i].name);
                kitMenu.addSeparator();
                kitMenu.addItem (299, T ("Kit zusammenstellen …"));
                root->addSubMenu (T ("Drum-Kit aus Presets"), kitMenu);
            }
            role.onChange = [this]
            {
                const int id = role.getSelectedId();
                if (id == 299) { const int keep = row; juce::MessageManager::callAsync ([p = &panel, keep] { p->composeKit ((size_t) keep); }); set (row); return; }
                if (id >= 300) { panel.kitOf[(size_t) row] = id - 300; panel.presetOf[(size_t) row] = -1; panel.roleOf[(size_t) row] = 4; }
                else if (id >= 100) { panel.presetOf[(size_t) row] = id - 100; panel.kitOf[(size_t) row] = -1; panel.roleOf[(size_t) row] = 0; }
                else if (id > 0) { panel.presetOf[(size_t) row] = -1; panel.kitOf[(size_t) row] = -1; panel.roleOf[(size_t) row] = id - 1; }
            };
        }
        void set (int r)
        {
            row = r;
            voice.setSelectedId (panel.voiceOf[(size_t) r] + 1, juce::dontSendNotification);
            role.setSelectedId (panel.kitOf[(size_t) r] >= 0 ? 300 + panel.kitOf[(size_t) r] : panel.presetOf[(size_t) r] >= 0 ? 100 + panel.presetOf[(size_t) r] : panel.roleOf[(size_t) r] + 1, juce::dontSendNotification);
            const bool sh = panel.shared ((size_t) r);
            prio.clear (juce::dontSendNotification);
            const int n = panel.groupSize (panel.voiceOf[(size_t) r]);
            for (int k = 1; k <= n && sh; ++k) prio.addItem (T ("Priorität %1", k), k);
            prio.setEnabled (sh);
            prio.setSelectedId (sh ? panel.prioOf[(size_t) r] + 1 : 0, juce::dontSendNotification);
            if (sh)         // the priority field wears the colour of the OSC it belongs to
            {
                prio.setColour (juce::ComboBox::backgroundColourId, voiceColour (panel.voiceOf[(size_t) r] - 1).withMultipliedBrightness (0.55f));
                prio.setColour (juce::ComboBox::textColourId, juce::Colours::white);
                prio.setColour (juce::ComboBox::arrowColourId, juce::Colours::white);
            }
            else
            {
                prio.removeColour (juce::ComboBox::backgroundColourId);
                prio.removeColour (juce::ComboBox::textColourId);
                prio.removeColour (juce::ComboBox::arrowColourId);
            }
            if (panel.active[(size_t) r] != 0)      // listened to: the key is green
            {
                play.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f9e44));
                play.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            }
            else
            {
                play.removeColour (juce::TextButton::buttonColourId);
                play.removeColour (juce::TextButton::textColourOffId);
            }
            play.repaint();
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (2, 3);
            play.setBounds (r.removeFromLeft (38)); r.removeFromLeft (4);
            role.setBounds (r.removeFromRight (140)); r.removeFromRight (6);
            prio.setBounds (r.removeFromRight (100)); r.removeFromRight (6);
            voice.setBounds (r.removeFromRight (110));
        }
        TrackPickPanel& panel;
        int version = 0;
        int row = 0;
        juce::TextButton play;
        juce::ComboBox voice, role, prio;
    };

    // a drum kit of your own: one preset for every kind of drum
    void composeKit (size_t row)
    {
        auto* w = new juce::AlertWindow (T ("Drum-Kit zusammenstellen"), T ("Für jede Trommel ein Preset wählen. „Standard“ nimmt den eingebauten Klang"), juce::MessageBoxIconType::NoIcon, this);
        w->addTextEditor ("name", T ("Mein Kit"), T ("Name"));
        static const char* const wanted[kNumDrumKinds] = { "Kicks", "Snares", "Toms & Becken", "Toms & Becken", "Hi-Hats", "Hi-Hats" };
        auto keys = std::make_shared<std::array<std::vector<std::string>, kNumDrumKinds>>();
        for (int k = 0; k < kNumDrumKinds; ++k)
        {
            juce::StringArray items;
            items.add (T ("Standard"));
            (*keys)[(size_t) k].push_back ({});
            for (auto& p : presets)
                if (p.category == juce::String::fromUTF8 (wanted[k]) || ! p.factory) { items.add (p.name); (*keys)[(size_t) k].push_back (InstrumentPool::keyOf (p)); }
            w->addComboBox ("k" + juce::String (k), items, T (InstrumentPool::kDrumKinds[k]));
            w->getComboBoxComponent ("k" + juce::String (k))->setSelectedItemIndex (std::min (1, items.size() - 1));
        }
        w->addButton (T ("Speichern"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton (T ("Abbrechen"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<TrackPickPanel> (this), row, w, keys] (int r)
        {
            if (r != 1 || safe == nullptr) return;
            const auto name = w->getTextEditorContents ("name").trim();
            if (name.isEmpty()) return;
            DrumKit kit;
            kit.name = name;
            for (int k = 0; k < kNumDrumKinds; ++k)
            {
                const int idx = w->getComboBoxComponent ("k" + juce::String (k))->getSelectedItemIndex();
                kit.keys[(size_t) k] = idx >= 0 && idx < (int) (*keys)[(size_t) k].size() ? (*keys)[(size_t) k][(size_t) idx] : std::string();
            }
            auto mine = InstrumentPool::userKits();
            mine.erase (std::remove_if (mine.begin(), mine.end(), [&] (const DrumKit& d) { return d.name == name; }), mine.end());
            mine.push_back (kit);
            InstrumentPool::saveUserKits (mine);
            safe->kits = InstrumentPool::factoryKits();
            safe->kits.insert (safe->kits.end(), mine.begin(), mine.end());
            safe->kitOf[row] = (int) safe->kits.size() - 1;
            safe->presetOf[row] = -1;
            safe->roleOf[row] = 4;
            ++safe->menuVersion;
            safe->list.updateContent();
            safe->main.setStatus (T ("Drum-Kit „%1“ gespeichert", name));
        }), true);
    }

    // ---- preview: tracks are switched on and off one by one; they sound together
    bool anyActive() const { for (auto a : active) if (a != 0) return true; return false; }
    double elapsedNow() const { return sidPlaying ? (double) main.player().getPlayFrame() / sidFps + sidOffset : main.player().getMidiElapsedSeconds(); }

    // the sound of the conversion: the tune is built from the tracks that are switched on (with their roles, presets and priorities),
    // played by the emulation, and it goes on at the point where the music was
    void applySidPreview (bool wasAny)
    {
        VoiceSources src;
        int rank = 0;
        bool any = false;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (active[i] != 0)
            {
                const bool assigned = voiceOf[i] > 0;
                const size_t v = assigned ? (size_t) voiceOf[i] - 1 : (size_t) (rank++ % 3);
                src[v].push_back ({ tracks[i].partId(), roleOf[i], assigned && shared (i) ? prioOf[i] : 100 + (int) i,
                                    kitOf[i] >= 0 ? InstrumentPool::keyOf (kits[(size_t) kitOf[i]]) : std::string(),
                                    presetOf[i] >= 0 ? InstrumentPool::keyOf (presets[(size_t) presetOf[i]]) : std::string() });
                any = true;
            }
        if (! any) { main.player().stop(); sidPlaying = false; return; }
        const double elapsed = wasAny && main.player().isPlaying() ? elapsedNow() : 0.0;
        const int gen = ++generation;
        ++pending;
        const auto file = tab.item (itemIndex)->file;
        juce::Component::SafePointer<TrackPickPanel> safe (this);
        main.jobs().addJob ([safe, file, src, gen, elapsed]
        {
            auto r = npImportMidi (file, BinaryData::np_v20g4_prg, (size_t) BinaryData::np_v20g4_prgSize, src);
            juce::MemoryBlock psid;
            if (r.ok) psid = r.tune.toPsid ("Vorschau", "SIDiMIDI", "");
            auto si = parseSidBytes (psid.getData(), psid.getSize(), true);
            auto t = r.ok ? computeTrace (si, 1, juce::jlimit (3.0, 900.0, r.tune.estimateSeconds() + 1.0)) : nullptr;
            juce::MessageManager::callAsync ([safe, t, si, gen, elapsed, off = r.startOffset, ok = r.ok]
            {
                if (safe == nullptr) return;
                --safe->pending;
                if (gen != safe->generation) return;               // a newer choice (or stop) has come in meanwhile
                if (! ok || t == nullptr || t->empty())
                {
                    std::fill (safe->active.begin(), safe->active.end(), (char) 0);
                    safe->list.updateContent();
                    safe->main.setStatus (T ("Die Spur lässt sich nicht vorhören"));
                    return;
                }
                safe->tab.stopPreview();
                safe->main.sidTab().stopPlayback();
                safe->main.player().playTrace (*t->events, t->fps, t->frames, (juce::uint32) std::max (0.0, (elapsed - off) * t->fps), si.ntsc());
                safe->sidPlaying = true;
                safe->sidOffset = off;
                safe->sidFps = t->fps;
            });
        });
    }

    void applyPreview (bool wasAny)
    {
        if (toTracker && sidSound.getToggleState()) { applySidPreview (wasAny); return; }
        sidPlaying = false;
        std::vector<AudioPlayerEngine::PreviewTrack> sel;
        int rank = 0;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (active[i] != 0)
            {
                const int slot = voiceOf[i] > 0 ? voiceOf[i] - 1 : rank % 3;
                ++rank;
                sel.push_back ({ tracks[i].trackIndex, tracks[i].channelFilter, slot });
            }
        if (sel.empty()) { main.player().stop(); return; }
        if (! wasAny) { tab.stopPreview(); main.sidTab().stopPlayback(); }
        if (! main.player().setPreviewTracks (tab.item (itemIndex)->file, sel))
        {
            std::fill (active.begin(), active.end(), (char) 0);
            main.setStatus (T ("Die Spur lässt sich nicht vorhören"));
        }
    }

    void toggleTrack (int row)
    {
        const bool was = anyActive();
        active[(size_t) row] = active[(size_t) row] != 0 ? 0 : 1;
        applyPreview (was && main.player().isPlaying());
        list.updateContent();
        list.repaint();
    }

    void playAssigned()
    {
        const bool was = anyActive();
        bool any = false;
        for (size_t i = 0; i < tracks.size(); ++i) { active[i] = voiceOf[i] > 0 ? 1 : 0; any = any || voiceOf[i] > 0; }
        if (! any) std::fill (active.begin(), active.end(), (char) 1);
        main.player().stop();                                // from the beginning
        sidPlaying = false;
        applyPreview (false);
        (void) was;
        list.updateContent();
        list.repaint();
    }

    void stopAll()
    {
        ++generation;
        sidPlaying = false;
        std::fill (active.begin(), active.end(), (char) 0);
        main.player().stop();
        list.updateContent();
        list.repaint();
        updateTime();
    }

    void updateTime()
    {
        const bool playing = anyActive() && main.player().isPlaying();
        timeLabel.setText (formatTime (playing ? elapsedNow() : 0.0) + " / " + formatTime (total), juce::dontSendNotification);
    }

    void timerCallback() override
    {
        if (anyActive() && pending == 0 && ! main.player().isPlaying())      // the music is over
        {
            sidPlaying = false;
            std::fill (active.begin(), active.end(), (char) 0);
            list.updateContent();
        }
        updateTime();
        if (anyActive()) list.repaint();
    }

    void updateButton()
    {
        int n = 0;
        for (int v : voiceOf) n += v > 0;
        convertBtn.setEnabled (n > 0);
    }

    void run()
    {
        VoiceSources sources;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (voiceOf[i] > 0)
                sources[(size_t) voiceOf[i] - 1].push_back ({ tracks[i].partId(), roleOf[i], shared (i) ? prioOf[i] : 0,
                                                              kitOf[i] >= 0 ? InstrumentPool::keyOf (kits[(size_t) kitOf[i]]) : std::string(),
                                                              presetOf[i] >= 0 ? InstrumentPool::keyOf (presets[(size_t) presetOf[i]]) : std::string() });
        stopAll();
        if (toTracker) tab.convertToTracker (itemIndex, sources);
        else tab.convertTracks (itemIndex, sources, false);
        closeLater();
    }

private:
    // a small picture of the notes of every track: time to the right over the whole piece, pitch upwards
    void buildMinis()
    {
        minis.assign (tracks.size(), juce::Image());
        const auto* item = tab.item (itemIndex);
        if (item == nullptr) return;
        juce::FileInputStream in (item->file);
        juce::MidiFile mf;
        if (! in.openedOk() || ! mf.readFrom (in)) return;
        mf.convertTimestampTicksToSeconds();
        struct N { double a, b; int p; };
        std::vector<std::vector<N>> notes (tracks.size());
        double end = 0.0;
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            const int ti = tracks[i].trackIndex, ch = tracks[i].channelFilter;
            if (ti < 0 || ti >= mf.getNumTracks()) continue;
            juce::MidiMessageSequence seq (*mf.getTrack (ti));
            seq.updateMatchedPairs();
            for (int k = 0; k < seq.getNumEvents(); ++k)
            {
                auto* ev = seq.getEventPointer (k);
                const auto& m = ev->message;
                if (! m.isNoteOn() || (ch > 0 && m.getChannel() != ch)) continue;
                const double a = m.getTimeStamp(), b = ev->noteOffObject != nullptr ? ev->noteOffObject->message.getTimeStamp() : a + 0.25;
                notes[i].push_back ({ a, b, m.getNoteNumber() });
                end = std::max (end, b);
            }
        }
        total = std::max (end, item->seconds);
        if (total <= 0.0) total = 1.0;
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            if (notes[i].empty()) continue;
            int lo = 127, hi = 0;
            for (auto& n : notes[i]) { lo = std::min (lo, n.p); hi = std::max (hi, n.p); }
            if (hi - lo < 12) { const int mid = (hi + lo) / 2; lo = mid - 6; hi = mid + 6; }
            juce::Image img (juce::Image::SingleChannel, kMiniW, kMiniH, true);
            juce::Graphics g (img);
            g.setColour (juce::Colours::white);
            for (auto& n : notes[i])
            {
                const float x = (float) (n.a / total * kMiniW), w = std::max (1.2f, (float) ((n.b - n.a) / total * kMiniW));
                const float y = (float) (kMiniH - 3) - (float) (n.p - lo) / (float) (hi - lo) * (float) (kMiniH - 5);
                g.fillRect (x, y, w, 2.0f);
            }
            minis[i] = img;
        }
    }

    MidiTab& tab;
    int itemIndex;
    std::vector<Track> tracks;
    bool toTracker = false;
    std::vector<int> voiceOf, roleOf, prioOf;      // prioOf: 0 = highest
    std::vector<int> presetOf, kitOf;               // index into presets / kits, -1 = none (the role decides)
    std::vector<InstrumentPreset> presets;
    std::vector<DrumKit> kits;
    int menuVersion = 0;                            // grows when a kit is added: the role boxes are built again
    std::vector<char> active;
    std::vector<juce::Image> minis;
    double total = 1.0;
    juce::ListBox list;
    juce::TextButton convertBtn, cancelBtn, stopBtn, playBtn;
    juce::Label help, timeLabel;
    juce::ToggleButton sidSound;                   // tracker: listen with the sound of the conversion (SID emulation) instead of the GM synth
    bool sidPlaying = false;
    double sidOffset = 0, sidFps = 50.1245;
    int generation = 0, pending = 0;
    std::unique_ptr<ScopeBox> scopes[3];
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
    stopBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffd0393a));
    stopBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    fundusBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3b6fd6));
    fundusBtn.onClick = [] { juce::URL (AboutPanel::kMidiFundusUrl).launchInDefaultBrowser(); };
    subfolders.setToggleState ((bool) main.archive().settings.getProperty ("midiSubfolders", true), juce::dontSendNotification);
    subfolders.onClick = [this] { main.archive().settings.setProperty ("midiSubfolders", subfolders.getToggleState(), nullptr); main.notifySettings(); if (folder.isDirectory()) startScan(); };
    search.onTextChange = [this] { rebuildShown(); };
    search.setIndents (6, 4);
    applyLanguage();

    for (const auto& line : juce::StringArray::fromLines (main.archive().settings.getProperty ("sidResults").toString()))
        if (const int tab = line.indexOfChar ('\t'); tab > 0) knownResults[line.substring (0, tab)] = line.substring (tab + 1);
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
        for (auto& g : got)
        {
            // results written to another folder: known from the list of earlier conversions
            if (g.result == juce::File())
                if (auto it = knownResults.find (g.file.getFullPathName()); it != knownResults.end() && juce::File (it->second).existsAsFile())
                    g.result = juce::File (it->second);
            items.push_back (std::move (g));
        }
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
        case mName: text = (m->folder.isNotEmpty() ? m->folder + "/" : juce::String()) + m->name; break;   // files of subfolders show their folder in front
        case mTracks: text = m->valid ? juce::String (m->tracks) : juce::String(); break;
        case mNotes: text = m->valid ? juce::String (m->notes) : juce::String(); break;
        case mLength: text = m->valid ? formatTime (m->seconds) : juce::String(); break;
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
    else    // targeted, or the tracker: the tracks are chosen first
    {
        std::vector<MidiToSidCoreEngine::ChannelSuggestion> all;
        if (! MidiToSidCoreEngine::listAllTracks (m->file, all, log) || all.empty())
        {
            main.setStatus (T ("Keine Spuren mit Noten gefunden"));
            return;
        }
        main.showOverlay (std::make_unique<TrackPickPanel> (main, *this, idx, std::move (all), std::array<int, 3> { sug[0].partId(), sug[1].partId(), sug[2].partId() }, depth == Depth::tracker));
    }
}

void MidiTab::convertToTracker (int idx, const std::array<int, 3>& parts, const std::array<int, 3>& roles)
{
    VoiceSources sources;
    for (size_t v = 0; v < 3; ++v)
        if (parts[v] != -1) sources[v].push_back ({ parts[v], roles[v], 0 });
    convertToTracker (idx, sources);
}

void MidiTab::convertToTracker (int idx, const VoiceSources& sources)
{
    const auto* m = item (idx);
    if (m == nullptr) return;
    main.openTracker();
    if (auto* tw = main.trackerWindow()) tw->importMidiFile (m->file, sources);
}

void MidiTab::convertTracks (int idx, const std::array<int, 3>& tracks, const std::array<int, 3>& roles, bool roundTrip)
{
    VoiceSources sources;
    for (size_t v = 0; v < 3; ++v)
        if (tracks[v] >= 0) sources[v].push_back ({ tracks[v], roles[v], 0 });
    convertTracks (idx, sources, roundTrip);
}

void MidiTab::convertTracks (int idx, const VoiceSources& sources, bool roundTrip)
{
    // pressing a conversion depth opens the save dialog (suggestion: the opened MIDI folder); the conversion then runs at once
    const auto* m = item (idx);
    if (m == nullptr) return;
    const auto suggested = (folder.isDirectory() ? folder : m->file.getParentDirectory())
                               .getChildFile (juce::File::createLegalFileName (m->file.getFileNameWithoutExtension()) + ".sid");
    juce::Component::SafePointer<MidiTab> safe (this);
    main.chooseSaveFile (T ("SID-Datei speichern unter"), suggested, "*.sid", [safe, idx, sources, roundTrip] (const juce::File& chosen)
    {
        if (safe == nullptr) return;
        safe->runConversion (idx, sources, roundTrip, chosen.hasFileExtension ("sid") ? chosen : chosen.withFileExtension ("sid"));
    });
}

void MidiTab::rememberResult (const juce::File& midi, const juce::File& sid)
{
    knownResults[midi.getFullPathName()] = sid.getFullPathName();
    juce::String all;
    int n = 0;
    for (auto it = knownResults.rbegin(); it != knownResults.rend() && n < 5000; ++it, ++n)
        all << it->first << "\t" << it->second << "\n";
    main.archive().settings.setProperty ("sidResults", all, nullptr);
    main.notifySettings();
}

void MidiTab::runConversion (int idx, const VoiceSources& sources, bool roundTrip, const juce::File& target)
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
    juce::Component::SafePointer<MidiTab> safe (this);
    main.jobs().addJob ([safe, src, target, sources, roundTrip, idx]
    {
        juce::String log;
        target.getParentDirectory().createDirectory();
        const bool ok = MidiToSidCoreEngine::convertMidiToSidFile (src, target, sources, roundTrip, log);
        juce::MessageManager::callAsync ([safe, ok, target, log, idx]
        {
            if (safe == nullptr) return;
            if (ok)
            {
                if (idx >= 0 && idx < (int) safe->items.size())
                {
                    safe->items[(size_t) idx].result = target;
                    safe->rememberResult (safe->items[(size_t) idx].file, target);
                }
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
