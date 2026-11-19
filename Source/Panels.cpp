#include "Panels.h"
#include "MainComponent.h"
#include "SidTab.h"

namespace sm
{
using ui::font;

const Theme& Overlay::theme() const { return main.theme(); }

juce::Rectangle<int> Overlay::box() const
{
    return getLocalBounds().withSizeKeepingCentre (juce::jmin (boxWidth, getWidth() - 40), juce::jmin (boxHeight, getHeight() - 40));
}

void Overlay::paint (juce::Graphics& g)
{
    const auto& th = theme();
    g.fillAll (th.bg.withAlpha (0.78f));
    auto b = box().toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (b.translated (3.0f, 5.0f), 12.0f);
    g.setColour (th.panel);
    g.fillRoundedRectangle (b, 12.0f);
    g.setColour (th.accent);
    g.drawRoundedRectangle (b, 12.0f, 1.5f);
}

void Overlay::mouseDown (const juce::MouseEvent& e)
{
    if (closeOnOutside && ! box().contains (e.getPosition())) closeLater();
}

bool Overlay::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { closeLater(); return true; }
    return false;
}

void Overlay::closeLater()
{
    juce::Component::SafePointer<MainComponent> safe (&main);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->closeOverlay(); });
}

// ================================================================================================ About
AboutPanel::AboutPanel (MainComponent& m) : Overlay (m)
{
    boxWidth = 900; boxHeight = 740;
    tabAbout.setButtonText (T ("Über SIDiMIDI"));
    tabTips.setButtonText (T ("Tooltipps"));
    close.setButtonText (T ("Schließen"));
    donate.setButtonText (T ("Spenden (PayPal)"));
    website.setButtonText ("undabtanzbar.de");
    github.setButtonText ("GitHub");
    text.setMultiLine (true);
    text.setReadOnly (true);
    text.setScrollbarsShown (true);
    text.setCaretVisible (false);
    text.setFont (font (14.0f));
    text.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    text.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    about = T ("@about");
    tips = T ("@tips");
    for (auto* b : { &tabAbout, &tabTips })
    {
        b->setRadioGroupId (404);
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }
    tabAbout.onClick = [this] { showPage (0); };
    tabTips.onClick = [this] { showPage (1); };
    donate.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffd6336c));
    donate.setTooltip (kDonateUrl);
    website.setTooltip (kSoftwareUrl);
    github.setTooltip (kGithubUrl);
    donate.onClick = [] { juce::URL (kDonateUrl).launchInDefaultBrowser(); };
    website.onClick = [] { juce::URL (kSoftwareUrl).launchInDefaultBrowser(); };
    github.onClick = [] { juce::URL (kGithubUrl).launchInDefaultBrowser(); };
    close.onClick = [this] { closeLater(); };
    for (auto* c : std::initializer_list<juce::Component*> { &donate, &website, &github, &close, &text }) addAndMakeVisible (c);
    showPage (0);
}

void AboutPanel::showPage (int p)
{
    page = p;
    (p == 0 ? tabAbout : tabTips).setToggleState (true, juce::dontSendNotification);
    text.setText (p == 0 ? about : tips, false);
    text.moveCaretToTop (false);
}

void AboutPanel::paint (juce::Graphics& g)
{
    Overlay::paint (g);
    const auto& th = theme();
    auto head = box().reduced (18).removeFromTop (72);
    head.removeFromLeft (4);
    main.paintLogo (g, head.removeFromLeft (72).toFloat());
    head.removeFromLeft (14);
    main.paintTitle (g, head.removeFromTop (40), 30.0f);
    g.setColour (th.dim);
    g.setFont (font (14.0f));
    g.drawText (T ("Version %1  ·  SID ↔ MIDI und C64-Musikarchiv  ·  von Un(d)AbtanzBAR", SM_VERSION_STRING), head, juce::Justification::topLeft);
}

void AboutPanel::layout()
{
    auto b = box().reduced (18);
    b.removeFromTop (82);
    auto tabs = b.removeFromTop (30);
    tabAbout.setBounds (tabs.removeFromLeft (180));
    tabs.removeFromLeft (6);
    tabTips.setBounds (tabs.removeFromLeft (130));
    b.removeFromTop (8);
    auto bottom = b.removeFromBottom (32);
    close.setBounds (bottom.removeFromRight (110));
    donate.setBounds (bottom.removeFromLeft (190));
    bottom.removeFromLeft (8);
    website.setBounds (bottom.removeFromLeft (150));
    bottom.removeFromLeft (8);
    github.setBounds (bottom.removeFromLeft (100));
    b.removeFromBottom (10);
    text.setBounds (b);
}

// ================================================================================================ HVSC
HvscPanel::HvscPanel (MainComponent& m) : Overlay (m), juce::Thread ("HVSC update")
{
    boxWidth = 860; boxHeight = 640;
    checkBtn.setButtonText (T ("Online prüfen"));
    installBtn.setButtonText (T ("Update installieren"));
    fullBtn.setButtonText (T ("Komplette HVSC (Webseite)"));
    addBtn.setButtonText (T ("HVSC-Ordner hinzufügen …"));
    closeBtn.setButtonText (T ("Schließen"));
    checkBtn.setTooltip (T ("Fragt die HVSC-Server, welche Update-Pakete es nach Ihrer Version gibt"));
    installBtn.setTooltip (T ("Lädt die fehlenden Update-Pakete, sichert alle betroffenen Dateien und spielt die Updates in die Sammlung ein"));
    fullBtn.setTooltip (kDownloadsUrl);
    installBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f9e44));
    log.setMultiLine (true);
    log.setReadOnly (true);
    log.setScrollbarsShown (true);
    log.setCaretVisible (false);
    log.setFont (font (13.0f));
    log.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha (0.25f));
    log.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    bar.setPercentageDisplay (false);
    for (auto* c : std::initializer_list<juce::Component*> { &collections, &log, &checkBtn, &installBtn, &fullBtn, &closeBtn, &addBtn, &bar }) addAndMakeVisible (c);

    checkBtn.onClick = [this]
    {
        if (busy()) { signalThreadShouldExit(); say (T ("Abbruch angefordert …")); }
        else startTask (Task::check);
    };
    installBtn.onClick = [this] { askInstall(); };
    fullBtn.onClick = [] { juce::URL (kDownloadsUrl).launchInDefaultBrowser(); };
    closeBtn.onClick = [this] { if (! busy()) closeLater(); };
    addBtn.onClick = [this] { if (! busy()) { main.sidTab().addFolderDialog(); closeLater(); } };
    collections.onChange = [this] { missing.clear(); missingSizes.clear(); refreshInfo(); updateButtons(); };

    list = main.archive().hvscList();
    for (size_t i = 0; i < list.size(); ++i)
        collections.addItem ("HVSC #" + juce::String (list[i]->version) + "   " + list[i]->root.getFullPathName(), (int) i + 1);
    collections.setSelectedItemIndex (0, juce::dontSendNotification);
    refreshInfo();
    updateButtons();
    startTimerHz (10);
    if (selected() != nullptr && selected()->version > 0 && main.archive().settings.getProperty ("hvscAutoCheck", true))
        startTask (Task::check);
}

HvscPanel::~HvscPanel()
{
    stopTimer();
    stopThread (20000);
}

std::shared_ptr<Hvsc> HvscPanel::selected() const
{
    const int i = collections.getSelectedItemIndex();
    return i >= 0 && i < (int) list.size() ? list[(size_t) i] : nullptr;
}

void HvscPanel::mouseDown (const juce::MouseEvent& e)
{
    if (! busy()) Overlay::mouseDown (e);
}

bool HvscPanel::keyPressed (const juce::KeyPress& k)
{
    if (busy() && k == juce::KeyPress::escapeKey) return true;
    return Overlay::keyPressed (k);
}

void HvscPanel::say (const juce::String& line)
{
    const juce::ScopedLock sl (lock);
    queuedLog.add (line);
}

void HvscPanel::refreshInfo()
{
    infoLines.clear();
    auto h = selected();
    if (h == nullptr)
    {
        infoLines = T ("In den Archiv-Ordnern wurde keine HVSC gefunden. Fügen Sie den Ordner „C64Music“ (oder dessen Elternordner) hinzu – oder laden Sie die komplette Sammlung von der HVSC-Webseite.");
        return;
    }
    infoLines << T ("Installierte Version") << ":  #" << (h->version > 0 ? juce::String (h->version) : juce::String ("?"))
              << (h->versionDate.isNotEmpty() ? "   (" + h->versionDate + ")" : juce::String()) << "\n";
    infoLines << T ("Sammlungsordner") << ":  " << h->root.getFullPathName() << "\n";
    infoLines << T ("%1 Songlängen · %2 STIL-Einträge · %3 Bug-Hinweise · %4 Musiker", h->lengthEntries(), h->stilEntries(), h->bugEntries(), h->musicianEntries()) << "\n";
    if (main.archive().isProtected (h->root.getFullPathName()))
        infoLines << T ("Der Ordner ist schreibgeschützt (Ordner-Menü). Updates sind gesperrt.") << "\n";
}

void HvscPanel::updateButtons()
{
    const bool b = busy();
    auto h = selected();
    checkBtn.setButtonText (b ? T ("Abbrechen") : T ("Online prüfen"));
    checkBtn.setEnabled (b || (h != nullptr && h->version > 0));
    installBtn.setEnabled (! b && h != nullptr && ! missing.empty());
    closeBtn.setEnabled (! b);
    addBtn.setEnabled (! b);
    collections.setEnabled (! b);
    bar.setVisible (b);
    repaint();
}

void HvscPanel::layout()
{
    auto b = box().reduced (20);
    b.removeFromTop (44);                                   // title
    collections.setBounds (b.removeFromTop (28));
    b.removeFromTop (8);
    b.removeFromTop (88);                                   // info (painted)
    auto bottom = b.removeFromBottom (34);
    closeBtn.setBounds (bottom.removeFromRight (110));
    bottom.removeFromRight (8);
    addBtn.setBounds (bottom.removeFromRight (210));
    checkBtn.setBounds (bottom.removeFromLeft (150));
    bottom.removeFromLeft (8);
    installBtn.setBounds (bottom.removeFromLeft (190));
    bottom.removeFromLeft (8);
    fullBtn.setBounds (bottom.removeFromLeft (220));
    b.removeFromBottom (8);
    bar.setBounds (b.removeFromBottom (24));
    b.removeFromBottom (8);
    log.setBounds (b);
}

void HvscPanel::paint (juce::Graphics& g)
{
    Overlay::paint (g);
    const auto& th = theme();
    auto b = box().reduced (20);
    g.setColour (th.text);
    g.setFont (font (22.0f).boldened());
    g.drawText (T ("HVSC – High Voltage SID Collection"), b.removeFromTop (40), juce::Justification::centredLeft);
    b.removeFromTop (36);
    g.setColour (th.dim);
    g.setFont (font (14.0f));
    g.drawFittedText (infoLines, b.removeFromTop (88), juce::Justification::topLeft, 6);
}

void HvscPanel::startTask (Task t)
{
    if (busy() || selected() == nullptr) return;
    target = selected();
    {
        const juce::ScopedLock sl (lock);
        fraction = 0.0;
        stage.clear();
    }
    finished = false;
    task = t;
    if (t == Task::check) { missing.clear(); missingSizes.clear(); offline = false; }
    installOk = false; installedVersion = 0;
    updateButtons();
    startThread();
}

void HvscPanel::askInstall()
{
    auto h = selected();
    if (busy() || h == nullptr || missing.empty()) return;
    auto& arch = main.archive();
    if (arch.isProtected (h->root.getFullPathName()))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, T ("Ordner schreibgeschützt"),
            T ("Der Ordner ist gegen Änderungen geschützt. Heben Sie den Schutz im Ordner-Menü auf, wenn die HVSC aktualisiert werden soll."));
        return;
    }
    if (arch.isScanning())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, T ("Einlesen läuft"), T ("Bitte warten, bis das Einlesen der Ordner beendet ist."));
        return;
    }
    juce::int64 bytes = 0;
    for (auto s : missingSizes) bytes += juce::jmax ((juce::int64) 0, s);
    const auto msg = T ("Es werden %1 Update-Pakete (#%2 bis #%3, zusammen etwa %4 MB) von den HVSC-Servern geladen und in\n%5\neingespielt.\n\nAlle dabei überschriebenen, verschobenen oder gelöschten Dateien werden vorher nach\n%6\ngesichert. Fortfahren?",
                        (int) missing.size(), missing.front(), missing.back(), juce::String ((double) bytes / 1048576.0, 1), h->root.getFullPathName(),
                        arch.dataFolder().getChildFile ("HvscBackup").getFullPathName());
    juce::Component::SafePointer<HvscPanel> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, T ("HVSC aktualisieren"), msg, T ("Herunterladen und installieren"), T ("Abbrechen"), nullptr,
        juce::ModalCallbackFunction::create ([safe] (int ok) { if (ok != 0 && safe != nullptr) safe->startTask (Task::install); }));
}

void HvscPanel::run()
{
    if (task == Task::check) doCheck(); else doInstall();
    finished = true;
}

void HvscPanel::doCheck()
{
    const int local = target->version;
    say (T ("Frage die HVSC-Server nach Updates nach #%1 …", local));
    std::vector<int> found;
    std::vector<juce::int64> sizes;
    for (int v = local + 1; v < local + 60 && ! threadShouldExit(); ++v)
    {
        const auto n = HvscOnline::remoteSize (HvscOnline::updateFileName (v));
        if (n < 0) break;
        found.push_back (v);
        sizes.push_back (n);
        const juce::ScopedLock sl (lock);
        stage = T ("Update #%1 gefunden", v);
    }
    if (found.empty() && ! threadShouldExit() && HvscOnline::remoteSize (HvscOnline::updateFileName (local)) < 0
        && HvscOnline::remoteSize (HvscOnline::updateFileName (local + 1)) < 0)
        offline = true;
    const juce::ScopedLock sl (lock);
    missing = found;
    missingSizes = sizes;
}

void HvscPanel::doInstall()
{
    auto& arch = main.archive();
    const auto root = target->root;
    const auto dl = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SIDIMIDI-hvsc-dl");
    dl.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H%M%S");
    const auto backupBase = arch.dataFolder().getChildFile ("HvscBackup").getChildFile (stamp);
    lastBackup = backupBase;
    const int n = (int) missing.size();
    for (int i = 0; i < n && ! threadShouldExit(); ++i)
    {
        const int v = missing[(size_t) i];
        const auto name = HvscOnline::updateFileName (v);
        const auto file = dl.getChildFile (name);
        say (T ("Update #%1: Download (%2 MB) …", v, juce::String ((double) juce::jmax ((juce::int64) 0, missingSizes[(size_t) i]) / 1048576.0, 2)));
        juce::String err;
        const bool got = HvscOnline::download (name, file, [&] (juce::int64 d, juce::int64 t)
        {
            const juce::ScopedLock sl (lock);
            stage = T ("Update #%1: Download", v) + (t > 0 ? " " + juce::String (juce::roundToInt (100.0 * (double) d / (double) t)) + " %" : juce::String());
            fraction = (i + 0.4 * (t > 0 ? (double) d / (double) t : 0.0)) / n;
            return ! threadShouldExit();
        }, err);
        if (! got) { say (T ("Fehler: %1", err)); return; }
        say (T ("Update #%1: einspielen …", v));
        const auto r = installHvscPackage (root, v, file, backupBase.getChildFile ("Update" + juce::String (v)), [&] (const juce::String& s, double f)
        {
            const juce::ScopedLock sl (lock);
            stage = T ("Update #%1: ", v) + s;
            fraction = (i + 0.4 + 0.6 * f) / n;
            return ! threadShouldExit();
        });
        file.deleteFile();
        for (int k = 0; k < juce::jmin (12, r.applied.messages.size()); ++k) say ("   " + r.applied.messages[k]);
        say (T ("   %1 Dateien kopiert, %2 verschoben, %3 ersetzt, %4 gelöscht, %5 Köpfe geändert", r.copiedFiles, r.applied.moved, r.applied.replaced, r.applied.deleted, r.applied.patched));
        if (! r.ok) { say (T ("Fehler: %1", r.error)); return; }
        installedVersion = v;
        installOk = true;
        say (T ("Update #%1 ist installiert.", v));
    }
    dl.deleteRecursively();
}

void HvscPanel::timerCallback()
{
    juce::StringArray lines;
    juce::String st;
    double f;
    {
        const juce::ScopedLock sl (lock);
        lines.swapWith (queuedLog);
        st = stage;
        f = fraction;
    }
    for (auto& l : lines) log.moveCaretToEnd(), log.insertTextAtCaret (l + "\n");
    progressValue = f;
    bar.setTextToDisplay (st);
    if (finished.load() && busy()) taskFinished();
}

void HvscPanel::taskFinished()
{
    waitForThreadToExit (5000);
    const auto t = task;
    task = Task::none;
    auto& arch = main.archive();
    if (t == Task::check)
    {
        const auto h = target;
        if (offline) log.insertTextAtCaret (T ("Keine Verbindung zu den HVSC-Servern.") + "\n");
        else if (missing.empty()) log.insertTextAtCaret (T ("Ihre HVSC (#%1) ist auf dem neuesten Stand.", h != nullptr ? h->version : 0) + "\n");
        else
        {
            juce::int64 bytes = 0;
            for (auto s : missingSizes) bytes += juce::jmax ((juce::int64) 0, s);
            log.insertTextAtCaret (T ("Es gibt %1 neue Update-Pakete (#%2 bis #%3, %4 MB).", (int) missing.size(), missing.front(), missing.back(), juce::String ((double) bytes / 1048576.0, 1)) + "\n");
        }
    }
    else if (t == Task::install)
    {
        if (installedVersion > 0)
        {
            arch.reloadHvsc();
            arch.startScan (true);
            list = arch.hvscList();
            const auto keep = target != nullptr ? target->root.getFullPathName() : juce::String();
            collections.clear (juce::dontSendNotification);
            int sel = 0;
            for (size_t i = 0; i < list.size(); ++i)
            {
                collections.addItem ("HVSC #" + juce::String (list[i]->version) + "   " + list[i]->root.getFullPathName(), (int) i + 1);
                if (list[i]->root.getFullPathName() == keep) sel = (int) i;
            }
            collections.setSelectedItemIndex (sel, juce::dontSendNotification);
            missing.erase (missing.begin(), std::find (missing.begin(), missing.end(), installedVersion) + 1);
            missingSizes.erase (missingSizes.begin(), missingSizes.begin() + (long) (missingSizes.size() - missing.size()));
            main.setStatus (T ("HVSC wurde auf Version #%1 aktualisiert. Die Datenbank wird abgeglichen …", installedVersion));
        }
        if (missing.empty()) log.insertTextAtCaret (T ("Fertig. Die Sicherung liegt in %1", lastBackup.getFullPathName()) + "\n");
    }
    target.reset();
    refreshInfo();
    updateButtons();
}
} // namespace sm
