#include "SidTab.h"
#include "MainComponent.h"
#include "AudioPlayerEngine.h"
#include "SidToMidiCore.h"

namespace sm
{
using ui::font;

// ================================================================================================ small widgets
void DragBar::paint (juce::Graphics& g)
{
    auto* l = dynamic_cast<ui::Look*> (&getLookAndFeel());
    const auto& th = l != nullptr ? l->theme() : themes()[0];
    g.setColour (th.panel2);
    g.fillRect (getLocalBounds());
    g.setColour (th.dim.withAlpha (0.7f));
    const int cx = getWidth() / 2, cy = getHeight() / 2;
    for (int i = -1; i <= 1; ++i) g.fillEllipse ((float) (cx + i * 8) - 1.5f, (float) cy - 1.5f, 3.0f, 3.0f);
}

void PositionBar::paint (juce::Graphics& g)
{
    auto* l = dynamic_cast<ui::Look*> (&getLookAndFeel());
    const auto& th = l != nullptr ? l->theme() : themes()[0];
    auto r = getLocalBounds().toFloat();
    g.setColour (th.panel2);
    g.fillRoundedRectangle (r, 5.0f);
    const double f = getFraction ? juce::jlimit (0.0, 1.0, getFraction()) : 0.0;
    if (f > 0)
    {
        g.setColour (th.accent.withAlpha (0.65f));
        g.fillRoundedRectangle (r.withWidth (std::max (6.0f, r.getWidth() * (float) f)), 5.0f);
    }
    g.setColour (th.text);
    g.setFont (font (12.0f));
    g.drawText (getText ? getText() : juce::String(), getLocalBounds().reduced (6, 0), juce::Justification::centred, false);
}

// ================================================================================================ SidTab
SidTab::SidTab (MainComponent& m) : main (m), arch (m.archive()), list (m.archive())
{
    addAndMakeVisible (list);
    addAndMakeVisible (graph);
    addAndMakeVisible (dragBar);
    addAndMakeVisible (position);
    addAndMakeVisible (info);
    addAndMakeVisible (songBox);
    addAndMakeVisible (songLabel);
    addAndMakeVisible (emptyHint);
    for (auto* b : { &folders, &scan, &hvscBtn, &playBtn, &stopBtn, &filterBtn, &ratingBtn, &clearFilters, &zoomReset }) addAndMakeVisible (b);
    for (auto* b : { &onlyFav, &autoNext }) addAndMakeVisible (b);
    addAndMakeVisible (search);
    playBtn.setButtonText (juce::String::fromUTF8 ("▶"));
    stopBtn.setButtonText (juce::String::fromUTF8 ("■"));
    hvscBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3b6fd6));
    search.setFont (font (14.0f));
    search.setIndents (6, 4);
    search.onTextChange = [this] { list.setGlobalSearch (search.getText()); updateButtons(); };
    info.setMultiLine (true);
    info.setReadOnly (true);
    info.setScrollbarsShown (true);
    info.setCaretVisible (false);
    info.setFont (font (12.5f));
    info.setReturnKeyStartsNewLine (true);
    emptyHint.setJustificationType (juce::Justification::centred);
    emptyHint.setFont (font (16.0f));
    emptyHint.setMinimumHorizontalScale (1.0f);
    songLabel.setJustificationType (juce::Justification::centredRight);

    folders.onClick = [this] { showFolderMenu(); };
    scan.onClick = [this] { if (arch.isScanning()) arch.stopScan(); else startScanAll(); updateButtons(); };
    hvscBtn.onClick = [this] { showHvscMenu(); };
    playBtn.onClick = [this] { togglePlay(); };
    stopBtn.onClick = [this] { stopPlayback(); };
    filterBtn.onClick = [this] { showFilterMenu(); };
    ratingBtn.onClick = [this] { showRatingMenu(); };
    clearFilters.onClick = [this]
    {
        search.clear(); filterType = 0; minStars = 0; onlyFav.setToggleState (false, juce::dontSendNotification);
        list.clearMasks(); updateButtons();
    };
    zoomReset.onClick = [this] { graph.resetZoom(); };
    onlyFav.onClick = [this] { list.setOnlyFavourites (onlyFav.getToggleState()); updateButtons(); };
    autoNext.setToggleState ((bool) arch.settings.getProperty ("autoNext", true), juce::dontSendNotification);
    autoNext.onClick = [this] { arch.settings.setProperty ("autoNext", autoNext.getToggleState(), nullptr); arch.settingsChanged(); };
    songBox.onChange = [this] { onSongChosen(); };

    list.onSelect = [this] (Entry* e) { onSelectionChanged (e); };
    list.onPlay = [this] (Entry* e) { play (e, 0); };
    list.onMenu = [this] (Entry& e, int col, juce::Point<int> p) { showEntryMenu (e, col, p); };
    list.onListChanged = [this] { updateEmptyHint(); updateButtons(); };

    graph.onSeek = [this] (juce::uint32 frame)
    {
        auto& pl = main.player();
        if (pl.hasSidTrace() && graph.entryId() == playingId && graph.song() == playingSong) { pl.seekFrame (frame); paused = false; pl.setSidPaused (false); }
        else if (graph.trace() != nullptr && ! graph.trace()->empty())
        {
            playingId = graph.entryId(); playingSong = graph.song();
            startPlayback (frame);
        }
    };
    graph.onTraceReady = [this]
    {
        if (pendingPlay) { pendingPlay = false; startPlayback ((juce::uint32) pendingStartFrame); pendingStartFrame = 0; }
        updateInfo (selected());
    };
    graph.onLaneToggle = [this] (int v)
    {
        const bool now = ! graph.laneMuted (v);
        graph.setVoiceMute (v, now);
        main.player().setVoiceMuted (v, now);
    };

    dragBar.onDrag = [this] (int dy) { viewHeight = juce::jlimit (150, std::max (170, getHeight() - 230), dragStart - dy); resized(); };
    dragBar.onEnd = [this] { dragStart = viewHeight; arch.settings.setProperty ("viewHeight", viewHeight, nullptr); arch.settingsChanged(); };

    position.getFraction = [this]
    {
        auto t = graph.trace();
        if (playingId == 0 || t == nullptr || graph.entryId() != playingId || t->frames == 0) return 0.0;
        return (double) main.player().getPlayFrame() / (double) t->frames;
    };
    position.getText = [this]
    {
        auto t = graph.trace();
        if (playingId == 0 || t == nullptr || graph.entryId() != playingId) return U ("–:– / –:–");
        return formatTime (t->frameSeconds (main.player().getPlayFrame())) + " / " + formatTime (t->seconds());
    };
    position.onSeekFraction = [this] (double f)
    {
        auto t = graph.trace();
        if (t == nullptr || graph.entryId() != playingId) return;
        main.player().seekFrame ((juce::uint32) (f * t->frames));
    };

    restoreState();
    applyLanguage();
    refreshAll();
    startTimerHz (30);
}

SidTab::~SidTab()
{
    stopTimer();
    saveState();
    main.player().stop();
}

void SidTab::restoreState()
{
    viewHeight = dragStart = juce::jlimit (150, 900, (int) arch.settings.getProperty ("viewHeight", 330));
    list.restoreLayout (arch.settings.getProperty ("columns").toString());
}

void SidTab::saveState()
{
    arch.settings.setProperty ("columns", list.layoutString(), nullptr);
    arch.settingsChanged();
}

void SidTab::applyLanguage()
{
    folders.setButtonText (T ("Ordner"));
    scan.setButtonText (arch.isScanning() ? T ("Stopp") : T ("Einlesen"));
    hvscBtn.setButtonText (hvscNewest > 0 ? juce::String::fromUTF8 ("HVSC ▲") : juce::String ("HVSC"));
    filterBtn.setButtonText (T ("Filter"));
    ratingBtn.setButtonText (juce::String::fromUTF8 ("★"));
    clearFilters.setButtonText (T ("Filter löschen"));
    zoomReset.setButtonText (T ("Ganz sehen"));
    onlyFav.setButtonText (juce::String::fromUTF8 ("♥ ") + T ("Nur Favoriten"));
    autoNext.setButtonText (T ("Weiter zum nächsten"));
    songLabel.setText (T ("Song:"), juce::dontSendNotification);
    search.setTextToShowWhenEmpty (T ("Suche in Titel, Autor, Komponist, Pfad …"), juce::Colours::grey);
    folders.setTooltip (T ("SID-Ordner hinzufügen, entfernen oder schreibschützen (Ordner lassen sich auch ins Fenster ziehen)"));
    scan.setTooltip (T ("Alle Ordner einlesen – neue und geänderte SID-Dateien werden in die Datenbank aufgenommen"));
    hvscBtn.setTooltip (hvscNewest > 0 ? T ("Neue HVSC-Version verfügbar: #%1. Klicken zum Aktualisieren.", hvscNewest)
                                       : T ("High Voltage SID Collection: Version, Update-Prüfung und Aktualisierung"));
    playBtn.setTooltip (T ("Markierten Tune abspielen / pausieren (Leertaste)"));
    stopBtn.setTooltip (T ("Stopp"));
    filterBtn.setTooltip (T ("Nach Herkunft und Besonderheiten filtern"));
    ratingBtn.setTooltip (T ("Nur Tunes ab einer Mindestbewertung zeigen"));
    onlyFav.setTooltip (T ("Nur Favoriten zeigen"));
    autoNext.setTooltip (T ("Nach dem Ende eines Tunes den nächsten in der Liste abspielen"));
    zoomReset.setTooltip (T ("Zoom zurücksetzen (Strg + Mausrad zoomt, Doppelklick in der Darstellung ebenfalls)"));
    clearFilters.setTooltip (T ("Alle Suchfelder und Filter leeren"));
    songBox.setTooltip (T ("Song (Subtune) des Tunes, der dargestellt und abgespielt wird"));
    list.applyLanguage();
    updateEmptyHint();
    updateButtons();
    resized();
}

void SidTab::applyTheme()
{
    repaint();
    list.repaint();
    graph.repaint();
}

void SidTab::updateButtons()
{
    scan.setButtonText (arch.isScanning() ? T ("Stopp") : T ("Einlesen"));
    scan.setToggleState (arch.isScanning(), juce::dontSendNotification);
    filterBtn.setToggleState (filterType != 0, juce::dontSendNotification);
    ratingBtn.setToggleState (minStars > 0, juce::dontSendNotification);
    ratingBtn.setButtonText (minStars > 0 ? juce::String::fromUTF8 ("≥ ") + juce::String (minStars) + juce::String::fromUTF8 (" ★") : juce::String::fromUTF8 ("★"));
    clearFilters.setVisible (list.hasFilters());
    playBtn.setButtonText (isPlaying() ? juce::String::fromUTF8 ("❚❚") : juce::String::fromUTF8 ("▶"));
}

void SidTab::updateEmptyHint()
{
    const bool empty = arch.entries().empty();
    emptyHint.setVisible (empty);
    emptyHint.setText (arch.folders.isEmpty()
        ? T ("Noch leer. Über „Ordner“ einen SID-Ordner (z. B. die HVSC) hinzufügen – oder ins Fenster ziehen – und „Einlesen“ klicken.")
        : T ("Noch keine SID-Dateien in der Datenbank. „Einlesen“ klicken."), juce::dontSendNotification);
}

void SidTab::focusSearch() { search.grabKeyboardFocus(); search.selectAll(); }

// ---- layout
void SidTab::resized()
{
    auto r = getLocalBounds();
    toolbarArea = r.removeFromTop (46);
    auto h = toolbarArea.reduced (10, 7);
    folders.setBounds (h.removeFromLeft (76)); h.removeFromLeft (6);
    scan.setBounds (h.removeFromLeft (92)); h.removeFromLeft (6);
    hvscBtn.setBounds (h.removeFromLeft (70)); h.removeFromLeft (14);
    // right side: transport and position
    stopBtn.setBounds (h.removeFromRight (34)); h.removeFromRight (4);
    playBtn.setBounds (h.removeFromRight (34)); h.removeFromRight (8);
    const int posW = juce::jlimit (110, 300, h.getWidth() / 4);
    position.setBounds (h.removeFromRight (posW)); h.removeFromRight (12);
    const bool compact = h.getWidth() < 640;
    onlyFav.setButtonText (compact ? juce::String::fromUTF8 ("♥") : juce::String::fromUTF8 ("♥ ") + T ("Nur Favoriten"));
    onlyFav.setBounds (h.removeFromRight (compact ? 44 : 140)); h.removeFromRight (6);
    ratingBtn.setBounds (h.removeFromRight (64)); h.removeFromRight (6);
    filterBtn.setBounds (h.removeFromRight (70)); h.removeFromRight (8);
    search.setBounds (h);

    auto status = r.removeFromBottom (0);
    (void) status;
    const int minTable = 140;
    const int vh = juce::jlimit (150, std::max (170, r.getHeight() - minTable - 10), viewHeight);
    auto bottom = r.removeFromBottom (vh);
    dragBar.setBounds (r.removeFromBottom (8));
    list.setBounds (r);
    emptyHint.setBounds (r.reduced (30, 40).withHeight (120));
    clearFilters.setBounds (r.getRight() - 140, r.getBottom() - 28, 130, 24);

    // bottom: header line (song, zoom), picture, info
    graphHeader = bottom.removeFromTop (34);
    auto gh = graphHeader.reduced (10, 4);
    songLabel.setBounds (gh.removeFromLeft (52));
    songBox.setBounds (gh.removeFromLeft (190)); gh.removeFromLeft (10);
    zoomReset.setBounds (gh.removeFromLeft (100)); gh.removeFromLeft (10);
    autoNext.setBounds (gh.removeFromLeft (std::min (210, gh.getWidth())));
    infoWidth = juce::jlimit (260, 520, bottom.getWidth() * 3 / 10);
    auto infoArea = bottom.removeFromRight (infoWidth);
    info.setBounds (infoArea.reduced (6, 2));
    graph.setBounds (bottom);
}

void SidTab::paint (juce::Graphics& g)
{
    const auto& th = main.theme();
    g.fillAll (th.bg);
    g.setColour (th.panel);
    g.fillRect (toolbarArea);
    g.setColour (th.panel);
    g.fillRect (graphHeader.getX(), graphHeader.getY(), getWidth(), graphHeader.getHeight());
}

// ---- selection and info
void SidTab::onSelectionChanged (Entry* e)
{
    songBox.clear (juce::dontSendNotification);
    if (e == nullptr)
    {
        graph.show (nullptr, 0);
        updateInfo (nullptr);
        return;
    }
    const int n = std::min (e->songs, 256);
    for (int i = 1; i <= n; ++i)
    {
        const double len = e->lengthOf (i);
        songBox.addItem (T ("Song %1 von %2", i, e->songs) + (len > 0 ? "  (" + formatTime (len) + ")" : juce::String()), i);
    }
    const int song = e->id == playingId && playingSong > 0 ? playingSong : e->startSong;
    songBox.setSelectedId (juce::jlimit (1, std::max (1, n), song), juce::dontSendNotification);
    graph.show (e, song);
    updateInfo (e);
}

int SidTab::chosenSong() const { return std::max (1, songBox.getSelectedId()); }

void SidTab::onSongChosen()
{
    auto* e = selected();
    if (e == nullptr) return;
    const int song = chosenSong();
    const bool wasPlayingThis = isPlaying() && playingId == e->id;
    graph.show (e, song);
    updateInfo (e);
    if (wasPlayingThis) play (e, song);
}

juce::String SidTab::infoText (const Entry& e) const
{
    juce::String s;
    s << (e.title.isNotEmpty() ? e.title : e.name) << "\n";
    if (e.author.isNotEmpty() && e.author != "<?>") s << e.author << "\n";
    if (e.released.isNotEmpty() && e.released != "<?>") s << e.released << "\n";
    s << "\n";
    if (e.has (fError)) { s << TS (e.error) << "\n"; }
    else
    {
        s << T ("%1 · %2 Song(s) · Start: Song %3", cellText (e, cType), e.songs, e.startSong) << "\n";
        juce::StringArray chip;
        if (cellText (e, cSid).isNotEmpty()) chip.add ("SID " + cellText (e, cSid));
        if (cellText (e, cClock).isNotEmpty()) chip.add (cellText (e, cClock));
        if (e.sidCount > 1) chip.add (T ("%1 SID-Chips", (int) e.sidCount));
        if (! chip.isEmpty()) s << chip.joinIntoString (U (" · ")) << "\n";
        s << T ("Laden $%1 · Init $%2 · Play $%3 · %4 Bytes Daten",
                juce::String::toHexString ((int) e.load).toUpperCase().paddedLeft ('0', 4),
                juce::String::toHexString ((int) e.init).toUpperCase().paddedLeft ('0', 4),
                juce::String::toHexString ((int) e.play).toUpperCase().paddedLeft ('0', 4), e.dataSize) << "\n";
        if (e.seconds > 0) s << T ("Länge Start-Song: %1", formatTime (e.seconds)) << "\n";
    }
    s << "\n" << T ("Pfad") << ": " << (e.rel.isNotEmpty() ? e.rel : e.path) << "\n";
    if (e.composer.isNotEmpty()) s << T ("Komponist") << ": " << e.composer << (e.country.isNotEmpty() ? "  (" + niceCountry (e.country) + ")" : juce::String()) << "\n";
    if (e.md5.isNotEmpty()) s << "MD5: " << e.md5 << "\n";
    if (auto hv = arch.hvscFor (e.path))
    {
        if (hv->version > 0) s << "HVSC #" << hv->version << (hv->versionDate.isNotEmpty() ? " (" + hv->versionDate + ")" : juce::String()) << "\n";
        const auto rel = e.rel;
        if (rel.isNotEmpty())
        {
            if (auto* m = hv->musicianFor (rel))
            {
                if (m->name.isNotEmpty()) s << T ("Bürgerlicher Name") << ": " << m->name << "\n";
                if (m->group.isNotEmpty()) s << T ("Gruppe") << ": " << m->group << "\n";
            }
            const auto dirStil = hv->stilDirectory (rel);
            const auto stil = hv->stil (rel);
            if (dirStil.isNotEmpty() || stil.isNotEmpty())
            {
                s << "\n" << U ("— STIL —") << "\n";
                if (dirStil.isNotEmpty()) s << dirStil << "\n\n";
                if (stil.isNotEmpty()) s << stil << "\n";
            }
            const auto bug = hv->bug (rel);
            if (bug.isNotEmpty()) s << "\n" << U ("— ") << T ("Bug-Liste") << U (" —") << "\n" << bug << "\n";
        }
    }
    if (e.has (fSameFile)) s << "\n" << T ("Es gibt %1 Dateien mit genau diesem Inhalt.", e.sameGroup) << "\n";
    return s;
}

void SidTab::updateInfo (Entry* e)
{
    const juce::String text = e == nullptr ? juce::String() : infoText (*e);
    const int id = e == nullptr ? 0 : e->id;
    if (id == shownInfoId && text == shownInfoText) return;
    shownInfoId = id;
    shownInfoText = text;
    info.setText (text, false);
    info.moveCaretToTop (false);
}

// ---- playback
bool SidTab::isPlaying() const { return playingId != 0 && ! paused && main.player().isSidTunePlaying(); }

void SidTab::play (Entry* e, int song)
{
    if (e == nullptr || e->has (fMissing) || e->has (fError)) return;
    if (song <= 0) song = (e->id == shownInfoId && selected() == e) ? chosenSong() : e->startSong;
    pendingPlay = true;
    pendingStartFrame = 0;
    playingId = e->id; playingSong = song;
    paused = false;
    if (selected() != e) list.selectEntry (e->id);
    if (graph.entryId() == e->id && graph.song() == song && graph.trace() != nullptr)
    {
        pendingPlay = false;
        startPlayback (0);
    }
    else
    {
        songBox.setSelectedId (song, juce::dontSendNotification);
        graph.show (e, song);
        setEnabled (true);
    }
    list.showPlayingRow (e->id);
}

void SidTab::startPlayback (juce::uint32 frame)
{
    auto t = graph.trace();
    if (t == nullptr || t->empty())
    {
        main.setStatus (t != nullptr && ! t->note.isEmpty() ? TS (t->note) : T ("Dieser Tune lässt sich nicht abspielen"));
        playingId = 0;
        list.showPlayingRow (0);
        return;
    }
    auto& pl = main.player();
    for (int v = 0; v < 3; ++v) pl.setVoiceMuted (v, graph.laneMuted (v));
    paused = false;
    pl.playTrace (*t->events, t->fps, t->frames, frame, graph.info().ntsc());
    list.showPlayingRow (playingId);
    main.setStatus ({});
    updateButtons();
}

void SidTab::togglePlay()
{
    auto& pl = main.player();
    if (playingId != 0 && pl.hasSidTrace())
    {
        paused = pl.isSidTunePlaying();
        pl.setSidPaused (paused);
        updateButtons();
        return;
    }
    play (selected(), 0);
}

void SidTab::stopPlayback()
{
    main.player().stop();
    playingId = 0; playingSong = 0; paused = false; pendingPlay = false;
    list.showPlayingRow (0);
    graph.setPlayFrame (-1);
    updateButtons();
    position.repaint();
}

void SidTab::playNext (int delta)
{
    const int row = list.rowOf (playingId != 0 ? playingId : (selected() != nullptr ? selected()->id : 0));
    for (int r = row + delta; r >= 0 && r < list.numRows(); r += delta)
    {
        auto* e = list.rowEntry (r);
        if (e != nullptr && ! e->has (fMissing) && ! e->has (fError)) { list.selectRow (r); play (e, 0); return; }
    }
    stopPlayback();
}

void SidTab::timerCallback()
{
    if (! hvscChecked && ! arch.isScanning() && main.updateChecksEnabled())
    {
        const auto hv = arch.hvscList();
        if (! hv.empty() && hv.front()->version > 0)
        {
            hvscChecked = true;
            const int local = hv.front()->version;
            juce::Component::SafePointer<SidTab> safe (this);
            main.jobs().addJob ([safe, local]
            {
                const int newest = HvscOnline::latestVersion (local);
                juce::MessageManager::callAsync ([safe, newest, local]
                {
                    if (safe == nullptr || newest <= local) return;
                    safe->hvscNewest = newest;
                    safe->applyLanguage();
                });
            });
        }
    }
    auto& pl = main.player();
    if (playingId != 0 && pl.hasSidTrace())
    {
        const bool shown = graph.entryId() == playingId && graph.song() == playingSong;
        graph.setPlayFrame (shown ? (juce::int64) pl.getPlayFrame() : -1);
        position.repaint();
        if (pl.takeSidFinished())
        {
            if (autoNext.getToggleState()) playNext (1);
            else stopPlayback();
        }
    }
    if (arch.isScanning() || scan.getButtonText() != (arch.isScanning() ? T ("Stopp") : T ("Einlesen"))) updateButtons();
    if (arch.version() != lastListVersion)
    {
        lastListVersion = arch.version();
        refreshAll();
    }
}

// ---- archive changes
void SidTab::refreshAll()
{
    lastListVersion = arch.version();
    list.refresh();
    updateEmptyHint();
    updateButtons();
    updateInfo (selected());
    if (arch.isScanning()) main.setStatus (arch.scanStatus());
}

// ---- folders
void SidTab::addFolderDialog()
{
    main.chooseFolder (T ("SID-Ordner zum Archiv hinzufügen"), [this] (const juce::File& f) { addFolders ({ f.getFullPathName() }); });
}

void SidTab::addFolders (const juce::StringArray& paths)
{
    juce::StringArray added;
    for (const auto& p : paths)
    {
        juce::File f (p);
        if (f.existsAsFile()) f = f.getParentDirectory();
        if (! f.isDirectory()) continue;
        added.add (arch.addFolder (f));
    }
    added.removeDuplicates (false);
    if (! added.isEmpty())
    {
        arch.startScan (false, added);
        updateButtons();
    }
}

void SidTab::startScanAll()
{
    if (arch.folders.isEmpty()) { addFolderDialog(); return; }
    arch.startScan (false);
}

void SidTab::showFolderMenu()
{
    juce::PopupMenu m;
    m.addItem (1, T ("Ordner hinzufügen …"));
    if (! arch.folders.isEmpty()) m.addSeparator();
    for (int i = 0; i < arch.folders.size(); ++i)
    {
        const auto f = arch.folders[i];
        juce::PopupMenu sub;
        sub.addItem (1000 + i * 10 + 1, T ("Schreibgeschützt (nie ändern, verschieben oder löschen)"), true, arch.isProtected (f));
        sub.addItem (1000 + i * 10 + 2, T ("Im Explorer zeigen"));
        sub.addItem (1000 + i * 10 + 3, T ("Nur diesen Ordner einlesen"));
        sub.addSeparator();
        sub.addItem (1000 + i * 10 + 4, T ("Aus dem Archiv entfernen (%1 Einträge) …", arch.countInFolder (f)));
        m.addSubMenu ((arch.isProtected (f) ? juce::String::fromUTF8 ("🔒 ") : juce::String()) + f, sub);
    }
    const int missing = (int) std::count_if (arch.entries().begin(), arch.entries().end(), [] (const std::unique_ptr<Entry>& e) { return e->has (fMissing); });
    if (missing > 0)
    {
        m.addSeparator();
        m.addItem (900, T ("%1 fehlende Dateien aus der Datenbank entfernen", missing));
    }
    juce::Component::SafePointer<SidTab> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&folders), [safe] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& t = *safe;
        if (r == 1) { t.addFolderDialog(); return; }
        if (r == 900) { t.arch.removeMissing(); return; }
        const int idx = (r - 1000) / 10, op = (r - 1000) % 10;
        if (idx < 0 || idx >= t.arch.folders.size()) return;
        const auto folder = t.arch.folders[idx];
        if (op == 1) t.arch.setProtected (folder, ! t.arch.isProtected (folder));
        else if (op == 2) juce::File (folder).revealToUser();
        else if (op == 3) { t.arch.startScan (false, { folder }); t.updateButtons(); }
        else if (op == 4)
        {
            const int n = t.arch.countInFolder (folder);
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, T ("Ordner entfernen"),
                T ("Der Ordner „%1“ und seine %2 Einträge werden aus der Datenbank genommen. Die Dateien auf der Festplatte bleiben unberührt.", folder, n),
                T ("Entfernen"), T ("Abbrechen"), &t,
                juce::ModalCallbackFunction::create ([safe, folder] (int ok) { if (ok != 0 && safe != nullptr) safe->arch.removeFolder (folder); }));
        }
    });
}

void SidTab::showHvscMenu()
{
    main.showOverlay (std::make_unique<HvscPanel> (main));
}

// ---- filter menus
void SidTab::showFilterMenu()
{
    juce::PopupMenu m;
    const char* names[] = { "Alle", "Nur Dateien aus der HVSC", "Nur Dateien außerhalb der HVSC", "Nur mit STIL-Eintrag", "Nur mit Bug-Hinweis", "Nur identische Dateien (Dubletten)" };
    for (int i = 0; i < 6; ++i) m.addItem (100 + i, T (names[i]), true, filterType == i);
    juce::Component::SafePointer<SidTab> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&filterBtn), [safe] (int r)
    {
        if (safe == nullptr || r < 100) return;
        safe->filterType = r - 100;
        safe->list.setOnlyType (safe->filterType);
        safe->updateButtons();
    });
}

void SidTab::showRatingMenu()
{
    juce::PopupMenu m;
    m.addItem (200, T ("Alle Bewertungen"), true, minStars == 0);
    for (int i = 1; i <= 5; ++i) m.addItem (200 + i, juce::String::fromUTF8 ("≥ ") + juce::String (i) + juce::String::fromUTF8 (" ★"), true, minStars == i);
    juce::Component::SafePointer<SidTab> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&ratingBtn), [safe] (int r)
    {
        if (safe == nullptr || r < 200) return;
        safe->minStars = r - 200;
        safe->list.setMinRating (safe->minStars);
        safe->updateButtons();
    });
}

// ---- context menu
void SidTab::showEntryMenu (Entry& clicked, int, juce::Point<int> screen)
{
    auto targets = list.selectedEntries();
    if (std::find (targets.begin(), targets.end(), &clicked) == targets.end()) { list.selectRow (list.rowOf (clicked.id)); targets = { &clicked }; }
    const bool many = targets.size() > 1;
    const bool anyProtected = std::any_of (targets.begin(), targets.end(), [] (Entry* e) { return e->has (fProtected); });
    const bool playable = ! clicked.has (fMissing) && ! clicked.has (fError);

    juce::PopupMenu m;
    m.addItem (1, T ("Abspielen"), playable);
    m.addSeparator();

    juce::PopupMenu conv;
    if (! many)
    {
        const int n = std::min (clicked.songs, 40);
        for (int i = 1; i <= n; ++i)
        {
            const double len = clicked.lengthOf (i);
            conv.addItem (300 + i, T ("Song %1 von %2", i, clicked.songs) + (len > 0 ? "  (" + formatTime (len) + ")" : juce::String()) + (i == clicked.startSong ? juce::String::fromUTF8 ("  ★") : juce::String()), playable);
        }
        if (clicked.songs > 1) { conv.addSeparator(); conv.addItem (299, T ("Alle %1 Songs als eigene MIDI-Dateien", clicked.songs), playable); }
    }
    else
        conv.addItem (298, T ("Start-Song von %1 Dateien", (int) targets.size()), playable);
    conv.addSeparator();
    const int mode = (int) arch.settings.getProperty ("midiOutMode", 0);
    conv.addItem (290, T ("MIDI neben die SID-Datei schreiben"), true, mode == 0);
    conv.addItem (291, mode == 1 ? T ("In festen Ordner schreiben: %1", arch.settings.getProperty ("midiOutDir").toString()) : T ("In festen Ordner schreiben …"), true, mode == 1);
    conv.addSeparator();
    conv.addItem (292, T ("Hinweis: je 4 Auflösungsstufen (Spur 1–3 vereinfacht bis Spur 10–12 mit Pitch-Bend/CC)"), false);
    m.addSubMenu (T ("Nach MIDI konvertieren"), conv, playable);
    m.addSeparator();

    juce::PopupMenu rate;
    for (int i = 0; i <= 5; ++i)
        rate.addItem (400 + i, i == 0 ? T ("Keine Bewertung") : juce::String::repeatedString (juce::String::fromUTF8 ("★"), i), true, clicked.rating == i);
    m.addSubMenu (T ("Bewertung"), rate);
    m.addItem (410, clicked.favourite ? T ("Favorit entfernen") : T ("Als Favorit markieren"));
    m.addSeparator();

    juce::PopupMenu file;
    file.addItem (500, T ("Im Explorer zeigen"), ! many);
    file.addItem (501, T ("Pfad kopieren"), ! many);
    file.addSeparator();
    file.addItem (502, T ("Umbenennen …"), ! many && ! anyProtected && ! clicked.has (fMissing));
    file.addItem (503, T ("Kopieren nach …"), ! clicked.has (fMissing));
    file.addItem (504, T ("Verschieben nach …"), ! anyProtected && ! clicked.has (fMissing));
    file.addItem (505, T ("In den Papierkorb …"), ! anyProtected && ! clicked.has (fMissing));
    file.addSeparator();
    file.addItem (506, T ("Nur aus der Datenbank entfernen"));
    m.addSubMenu (T ("Datei"), file);

    juce::PopupMenu web;
    const auto q = (clicked.title + " " + clicked.author).trim();
    web.addItem (600, "CSDb");
    web.addItem (601, "YouTube");
    web.addItem (602, "Google");
    m.addSubMenu (T ("Im Internet suchen"), web, ! many);

    main.addLoupeMenu (m, 900);

    juce::Component::SafePointer<SidTab> safe (this);
    const int id = clicked.id;
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (screen.x, screen.y, 1, 1)), [safe, id, targets, q] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& t = *safe;
        auto* e = t.arch.byId (id);
        if (e == nullptr) return;
        if (r >= 900 && r < 920) { t.main.handleLoupeMenu (r, 900); return; }
        std::vector<Entry*> live;
        for (auto* x : targets) if (t.arch.byId (x->id) != nullptr) live.push_back (x);
        switch (r)
        {
            case 1: t.play (e, 0); break;
            case 290: t.arch.settings.setProperty ("midiOutMode", 0, nullptr); t.arch.settingsChanged(); break;
            case 291:
                t.main.chooseFolder (T ("Zielordner für MIDI-Dateien"), [safe] (const juce::File& f)
                {
                    if (safe == nullptr) return;
                    safe->arch.settings.setProperty ("midiOutMode", 1, nullptr);
                    safe->arch.settings.setProperty ("midiOutDir", f.getFullPathName(), nullptr);
                    safe->arch.settingsChanged();
                });
                break;
            case 298: t.convertToMidi (live, 0); break;
            case 299: t.convertToMidi ({ e }, -1); break;
            case 410: t.arch.setFavourite (*e, ! e->favourite); break;
            case 500: juce::File (e->path).revealToUser(); break;
            case 501: juce::SystemClipboard::copyTextToClipboard (e->path); break;
            case 502: t.renameEntry (*e); break;
            case 503: t.copyOrMove (live, false); break;
            case 504: t.copyOrMove (live, true); break;
            case 505: t.trashEntries (live); break;
            case 506:
            {
                std::vector<int> ids;
                for (auto* x : live) ids.push_back (x->id);
                t.arch.removeEntries (ids);
                break;
            }
            case 600: juce::URL ("https://csdb.dk/search/?seinsel=all&search=" + juce::URL::addEscapeChars (q, true)).launchInDefaultBrowser(); break;
            case 601: juce::URL ("https://www.youtube.com/results?search_query=" + juce::URL::addEscapeChars (q + " C64 SID", true)).launchInDefaultBrowser(); break;
            case 602: juce::URL ("https://www.google.com/search?q=" + juce::URL::addEscapeChars (q + " C64 SID", true)).launchInDefaultBrowser(); break;
            default:
                if (r >= 300 && r < 299 + 42 && r != 299) t.convertToMidi ({ e }, r - 300);
                else if (r >= 400 && r <= 405) { for (auto* x : live) t.arch.setRating (*x, r - 400); }
                break;
        }
    });
}

// ---- conversion SID -> MIDI
juce::File SidTab::midiTargetFor (const Entry& e, int song, bool allSongs) const
{
    juce::File dir (e.folder);
    if ((int) arch.settings.getProperty ("midiOutMode", 0) == 1)
    {
        const juce::File d (arch.settings.getProperty ("midiOutDir").toString());
        if (d.isDirectory()) dir = d;
    }
    juce::String base = juce::File (e.name).getFileNameWithoutExtension();
    if (e.songs > 1 || allSongs) base << "_song" << juce::String (song).paddedLeft ('0', 2);
    juce::File target = dir.getChildFile (base + ".mid");
    // never overwrite an existing file
    for (int i = 2; target.exists() && i < 1000; ++i) target = dir.getChildFile (base + " (" + juce::String (i) + ").mid");
    return target;
}

void SidTab::convertToMidi (const std::vector<Entry*>& entries, int songOrZero)
{
    struct Job { juce::String path; int song; juce::File target; juce::String name; };
    std::vector<Job> jobs;
    for (auto* e : entries)
    {
        if (e->has (fMissing) || e->has (fError)) continue;
        std::vector<int> songs;
        if (songOrZero == -1) for (int i = 1; i <= std::min (e->songs, 99); ++i) songs.push_back (i);
        else songs.push_back (songOrZero > 0 ? songOrZero : e->startSong);
        for (int s : songs)
        {
            auto target = midiTargetFor (*e, s, songOrZero == -1);
            if (arch.isProtected (target.getParentDirectory().getFullPathName()))
            {
                main.setStatus (T ("Der Zielordner ist schreibgeschützt: %1", target.getParentDirectory().getFullPathName()));
                return;
            }
            // two songs of the same job must not pick the same name
            for (auto& j : jobs) if (j.target == target) target = target.getSiblingFile (target.getFileNameWithoutExtension() + "_" + juce::String (s) + ".mid");
            jobs.push_back ({ e->path, s, target, e->name });
        }
    }
    if (jobs.empty()) return;
    main.setStatus (T ("Konvertiere %1 Datei(en) nach MIDI …", (int) jobs.size()));
    juce::Component::SafePointer<SidTab> safe (this);
    main.jobs().addJob ([safe, jobs]
    {
        int ok = 0;
        juce::String firstError;
        juce::File last;
        for (auto& j : jobs)
        {
            juce::String log;
            j.target.getParentDirectory().createDirectory();
            if (SidToMidiCoreEngine::convertSidToMidiFile (juce::File (j.path), j.target, log, j.song)) { ++ok; last = j.target; }
            else if (firstError.isEmpty()) firstError = j.name + ": " + log.trim().fromLastOccurrenceOf ("\n", false, false);
        }
        juce::MessageManager::callAsync ([safe, ok, total = (int) jobs.size(), firstError, last]
        {
            if (safe == nullptr) return;
            if (ok == total) safe->main.setStatus (total == 1 ? T ("MIDI geschrieben: %1", last.getFullPathName()) : T ("%1 MIDI-Dateien geschrieben (zuletzt %2)", ok, last.getFullPathName()));
            else safe->main.setStatus (T ("%1 von %2 konvertiert – %3", ok, total, firstError));
            if (ok > 0) last.revealToUser();
        });
    });
}

// ---- file operations
void SidTab::renameEntry (Entry& e)
{
    if (e.has (fProtected)) { main.setStatus (T ("Der Ordner ist schreibgeschützt")); return; }
    auto* w = new juce::AlertWindow (T ("Umbenennen"), T ("Neuer Dateiname (ohne .sid):"), juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor ("name", juce::File (e.name).getFileNameWithoutExtension());
    w->addButton (T ("Umbenennen"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (T ("Abbrechen"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    const int id = e.id;
    juce::Component::SafePointer<SidTab> safe (this);
    w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, w, id] (int r)
    {
        const auto name = w->getTextEditorContents ("name").trim();
        delete w;
        if (r != 1 || safe == nullptr || name.isEmpty()) return;
        auto* en = safe->arch.byId (id);
        if (en == nullptr) return;
        const juce::File src (en->path);
        const auto dst = src.getSiblingFile (juce::File::createLegalFileName (name) + ".sid");
        if (dst.exists()) { safe->main.setStatus (T ("Es gibt schon eine Datei mit diesem Namen")); return; }
        if (src.moveFileTo (dst)) safe->arch.relocate (*en, dst);
        else safe->main.setStatus (T ("Umbenennen fehlgeschlagen"));
    }), false);
}

void SidTab::copyOrMove (const std::vector<Entry*>& entries, bool move)
{
    std::vector<int> ids;
    for (auto* e : entries) ids.push_back (e->id);
    juce::Component::SafePointer<SidTab> safe (this);
    main.chooseFolder (move ? T ("Verschieben nach …") : T ("Kopieren nach …"), [safe, ids, move] (const juce::File& dir)
    {
        if (safe == nullptr) return;
        auto& t = *safe;
        if (t.arch.isProtected (dir.getFullPathName())) { t.main.setStatus (T ("Der Zielordner ist schreibgeschützt")); return; }
        int n = 0;
        for (int id : ids)
        {
            auto* e = t.arch.byId (id);
            if (e == nullptr || (move && e->has (fProtected))) continue;
            const juce::File src (e->path);
            auto dst = dir.getChildFile (src.getFileName());
            for (int i = 2; dst.exists() && i < 1000; ++i) dst = dir.getChildFile (src.getFileNameWithoutExtension() + " (" + juce::String (i) + ").sid");
            if (move ? src.moveFileTo (dst) : src.copyFileTo (dst))
            {
                ++n;
                if (move) t.arch.relocate (*e, dst);
                else if (t.arch.isInsideFolders (dst)) t.arch.addFile (dst);
            }
        }
        t.main.setStatus (move ? T ("%1 Datei(en) verschoben", n) : T ("%1 Datei(en) kopiert", n));
    });
}

void SidTab::trashEntries (const std::vector<Entry*>& entries)
{
    std::vector<int> ids;
    for (auto* e : entries) if (! e->has (fProtected)) ids.push_back (e->id);
    if (ids.empty()) return;
    juce::Component::SafePointer<SidTab> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, T ("In den Papierkorb"),
        T ("%1 Datei(en) in den Papierkorb verschieben?", (int) ids.size()), T ("In den Papierkorb"), T ("Abbrechen"), this,
        juce::ModalCallbackFunction::create ([safe, ids] (int ok)
        {
            if (ok == 0 || safe == nullptr) return;
            auto& t = *safe;
            std::vector<int> gone;
            for (int id : ids)
                if (auto* e = t.arch.byId (id))
                    if (juce::File (e->path).moveToTrash()) gone.push_back (id);
            t.arch.removeEntries (gone);
            t.main.setStatus (T ("%1 Datei(en) in den Papierkorb verschoben", (int) gone.size()));
        }));
}

bool SidTab::handleKey (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey && ! search.hasKeyboardFocus (true)) { togglePlay(); return true; }
    if (k == juce::KeyPress::escapeKey && ! search.hasKeyboardFocus (true)) { stopPlayback(); return true; }
    if (k == juce::KeyPress ('f', juce::ModifierKeys::ctrlModifier, 0)) { focusSearch(); return true; }
    if (k == juce::KeyPress::rightKey && k.getModifiers().isAltDown()) { playNext (1); return true; }
    if (k == juce::KeyPress::leftKey && k.getModifiers().isAltDown()) { playNext (-1); return true; }
    return false;
}
} // namespace sm
