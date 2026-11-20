#include "MainComponent.h"
#include "SidTab.h"
#include "MidiTab.h"
#include "SidView.h"
#include "AudioPlayerEngine.h"
#include <BinaryData.h>

namespace sm
{
using ui::font;

// ================================================================================================ tab button
void TabButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto* l = dynamic_cast<ui::Look*> (&getLookAndFeel());
    const auto& th = l != nullptr ? l->theme() : themes()[0];
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const auto c = voiceColour (colour);
    const bool on = getToggleState();
    auto base = on ? c : th.panel2;
    if (over) base = base.brighter (0.1f);
    if (down) base = base.darker (0.15f);
    g.setColour (base);
    g.fillRoundedRectangle (r, 9.0f);
    if (! on)
    {
        g.setColour (c.withAlpha (0.8f));
        g.fillRoundedRectangle (r.withTrimmedTop (r.getHeight() - 3.0f).reduced (8.0f, 0.0f), 1.5f);
    }
    g.setColour (on ? (c.getPerceivedBrightness() > 0.55f ? juce::Colours::black : juce::Colours::white) : th.text);
    g.setFont (font (15.0f, true));
    g.drawFittedText (getButtonText(), getLocalBounds().reduced (6, 0), juce::Justification::centred, 1);
}

// ================================================================================================ oscillators
ScopeBox::ScopeBox (AudioPlayerEngine& e, int voiceIndex) : engine (e), voice (voiceIndex)
{
    startTimerHz (30);
}

void ScopeBox::timerCallback()
{
    AudioPlayerEngine::VoiceDisplayInfo info[3];
    engine.getVoiceDisplaySnapshot (info);
    const bool active = info[voice].active;
    if (active || wasActive) repaint();
    wasActive = active;
}

void ScopeBox::paint (juce::Graphics& g)
{
    auto* l = dynamic_cast<ui::Look*> (&getLookAndFeel());
    const auto& th = l != nullptr ? l->theme() : themes()[0];
    const bool light = th.bg.getBrightness() > 0.5f;
    const auto vc = voiceColour (voice, light);
    auto r = getLocalBounds().toFloat();
    g.setColour (light ? th.panel2 : juce::Colours::black.withAlpha (0.38f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (vc.withAlpha (0.55f));
    g.fillRoundedRectangle (juce::Rectangle<float> (r.getX(), r.getY() + 4.0f, 3.0f, r.getHeight() - 8.0f), 1.5f);
    g.setColour (vc.withAlpha (0.9f));
    g.setFont (ui::font (10.5f, true));
    g.drawText (juce::String (voice + 1), r.toNearestInt().withTrimmedLeft (7).withHeight (12), juce::Justification::topLeft, false);

    AudioPlayerEngine::VoiceDisplayInfo info[3];
    engine.getVoiceDisplaySnapshot (info);
    const auto& v = info[voice];
    auto plot = r.reduced (4.0f, 3.0f).withTrimmedLeft (4.0f);
    if (! v.active)
    {
        g.setColour (th.dim.withAlpha (0.35f));
        g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
        return;
    }
    // the level of the envelope sets the brightness; the line is the waveform of the last few milliseconds
    g.setColour (vc.withAlpha (0.35f + 0.65f * juce::jlimit (0.0f, 1.0f, v.envelopeLevel * 1.3f)));
    juce::Path p;
    const int n = AudioPlayerEngine::kOscHistorySize;
    for (int i = 0; i < n; ++i)
    {
        const int idx = (v.oscHistoryWritePos + i) % n;
        const float sample = juce::jlimit (-1.0f, 1.0f, v.oscHistory[(size_t) idx]);
        const float x = plot.getX() + plot.getWidth() * (float) i / (float) (n - 1);
        const float y = plot.getCentreY() - sample * plot.getHeight() * 0.46f;
        if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
    }
    g.strokePath (p, juce::PathStrokeType (1.5f));
}

// ================================================================================================ main component
MainComponent::MainComponent()
{
    initLanguage (arch->settings);
    playerEngine = std::make_unique<AudioPlayerEngine>();
    for (int v = 0; v < 3; ++v) { scopes[v] = std::make_unique<ScopeBox> (*playerEngine, v); addAndMakeVisible (*scopes[v]); }
    iconImage = juce::ImageFileFormat::loadFrom (BinaryData::icon_png, (size_t) BinaryData::icon_pngSize);
    bannerImage = juce::ImageFileFormat::loadFrom (BinaryData::banner_png, (size_t) BinaryData::banner_pngSize);

    setLookAndFeel (&look);
    juce::LookAndFeel::setDefaultLookAndFeel (&look);   // dialogs (file chooser, alerts) get the colours of the theme too
    applyTheme();

    sidPane = std::make_unique<SidTab> (*this);
    midiPane = std::make_unique<MidiTab> (*this);
    addAndMakeVisible (*sidPane);
    addChildComponent (*midiPane);
    for (auto* b : { &tabSid, &tabMidi }) addAndMakeVisible (b);
    tabSid.setToggleState (true, juce::dontSendNotification);
    tabSid.onClick = [this] { selectTab (0); };
    tabMidi.onClick = [this] { selectTab (1); };
    addAndMakeVisible (settingsBtn);
    addAndMakeVisible (aboutBtn);
    addChildComponent (updateBtn);
    settingsBtn.onClick = [this] { showSettingsMenu(); };
    aboutBtn.onClick = [this] { showAbout (overlay == nullptr); };
    updateBtn.onClick = [this] { showUpdateMenu(); };
    updateBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff2f9e44));
    updateBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);

    loupe = std::make_unique<ui::Loupe>();
    addChildComponent (*loupe);
    catcher = std::make_unique<ui::RightClickCatcher>();
    catcher->onRightClick = [this] { showLoupeMenu(); };
    catcher->passThrough = [this] (juce::Point<int> p)
    {
        if (inPassThrough) return true;
        if (overlay != nullptr) return true;
        auto* list = tab == 0 ? &sidPane->table().list() : nullptr;
        if (list != nullptr)
        {
            const auto lp = list->getLocalPoint (this, p);
            if (list->getLocalBounds().contains (lp) && lp.y >= list->getHeaderHeight() && list->getRowContainingPosition (lp.x, lp.y) >= 0) return true;
        }
        const juce::ScopedValueSetter<bool> guard (inPassThrough, true);
        for (auto* c = getComponentAt (p); c != nullptr && c != this; c = c->getParentComponent())
            if (dynamic_cast<juce::TextEditor*> (c) != nullptr || dynamic_cast<juce::ListBox*> (c) != nullptr) return true;
        return false;
    };
    addAndMakeVisible (*catcher);
    catcher->setBounds (getLocalBounds());
    addMouseListener (&loupeTracker, true);

    applyLanguage();
    selectTab ((int) arch->settings.getProperty ("tab", 0));
    setWantsKeyboardFocus (true);
    setSize (1440, 880);
    startTimerHz (10);
}

MainComponent::~MainComponent()
{
    stopTimer();
    removeMouseListener (&loupeTracker);
    arch->settings.setProperty ("tab", tab, nullptr);
    overlay.reset();
    sidPane.reset();
    midiPane.reset();
    tracker.reset();
    for (auto& s : scopes) s.reset();
    playerEngine.reset();
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void MainComponent::applyTheme()
{
    const int idx = juce::jlimit (0, (int) themes().size() - 1, (int) arch->settings.getProperty ("theme", 0));
    const bool light = arch->settings.getProperty ("light", false);
    look.setTheme (light ? lightVariant (themes()[(size_t) idx]) : themes()[(size_t) idx]);
    const auto& th = look.theme();
    look.setColour (juce::TableHeaderComponent::backgroundColourId, th.panel2);
    look.setColour (juce::TableHeaderComponent::textColourId, th.text);
    look.setColour (juce::TableHeaderComponent::outlineColourId, th.bg);
    look.setColour (juce::TableHeaderComponent::highlightColourId, th.accent.withAlpha (0.3f));
    look.setColour (juce::ListBox::backgroundColourId, th.bg);
    sendLookAndFeelChange();
    if (sidPane != nullptr) sidPane->applyTheme();
    if (midiPane != nullptr) midiPane->applyTheme();
    repaint();
}

void MainComponent::refreshHvscHint()
{
    const int n = sidPane != nullptr ? sidPane->hvscNewestVersion() : 0;
    settingsBtn.setButtonText (n > 0 ? T ("Einstellungen") + juce::String::fromUTF8 (" ▲") : T ("Einstellungen"));
    settingsBtn.setTooltip (n > 0 ? T ("Neue HVSC-Version verfügbar: #%1 – unter „HVSC-Sammlung aktualisieren …“", n) : T ("Farbthema, Sprache, Skalierung, Lupe, Updates"));
}

void MainComponent::applyLanguage()
{
    settingsBtn.setButtonText (T ("Einstellungen"));
    tabSid.setButtonText ("SID>MIDI");
    tabMidi.setButtonText ("MIDI>SID");
    tabSid.setTooltip (T ("SID-Archiv: Datenbank der SID-Dateien, Ablauf der Tunes, HVSC, Konvertierung nach MIDI"));
    tabMidi.setTooltip (T ("MIDI-Ordner öffnen und einzelne MIDI-Dateien nach SID konvertieren"));
    aboutBtn.setTooltip (T ("Über SIDiMIDI, Spenden und Tooltipps"));
    for (auto& s : scopes) if (s != nullptr) s->setTooltip (T ("Die drei SID-Oszillatoren OSC1–3 (live beim Abspielen)"));
    shownUpdate = "-";
    refreshUpdateHint();
    if (sidPane != nullptr) sidPane->applyLanguage();
    if (midiPane != nullptr) midiPane->applyLanguage();
    refreshHvscHint();
    overlay.reset();
    resized();
    repaint();
}

void MainComponent::setStatus (const juce::String& s)
{
    statusText = s;
    repaint (statusArea);
}

void MainComponent::selectTab (int index)
{
    tab = juce::jlimit (0, 1, index);
    tabSid.setToggleState (tab == 0, juce::dontSendNotification);
    tabMidi.setToggleState (tab == 1, juce::dontSendNotification);
    sidPane->setVisible (tab == 0);
    midiPane->setVisible (tab == 1);
    if (catcher != nullptr) catcher->toFront (false);
    if (loupe != nullptr) loupe->toFront (false);
    arch->settings.setProperty ("tab", tab, nullptr);
    arch->settingsChanged();
    repaint();
}

void MainComponent::chooseFolder (const juce::String& title, std::function<void (const juce::File&)> done, const juce::String& showPattern, const juce::String& rememberKey)
{
    // JUCE's own dialog instead of the Windows folder picker: it lists the files of the folder as well,
    // so one can see what a folder contains before choosing it.
    juce::File start (rememberKey.isNotEmpty() ? arch->settings.getProperty (rememberKey).toString() : juce::String());
    if (! start.isDirectory()) start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    chooser = std::make_unique<juce::FileChooser> (title, start, showPattern, false);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<MainComponent> (this), done, rememberKey] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              auto f = fc.getResult();
                              if (f == juce::File()) return;
                              if (f.existsAsFile()) f = f.getParentDirectory();   // a file was clicked: take its folder
                              if (! f.isDirectory()) return;
                              if (rememberKey.isNotEmpty()) { safe->arch->settings.setProperty (rememberKey, f.getFullPathName(), nullptr); safe->arch->settingsChanged(); }
                              if (done) done (f);
                          });
}

void MainComponent::chooseOpenFile (const juce::String& title, const juce::File& start, const juce::String& pattern, std::function<void (const juce::File&)> done)
{
    chooser = std::make_unique<juce::FileChooser> (title, start, pattern, false);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<MainComponent> (this), done] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              const auto f = fc.getResult();
                              if (f.existsAsFile() && done) done (f);
                          });
}

void MainComponent::openTracker()
{
    if (tracker != nullptr) { tracker->setVisible (true); tracker->toFront (true); return; }
    tracker = std::make_unique<TrackerWindow> (*this);
    tracker->onClosed = [this] { tracker.reset(); };
}

void MainComponent::chooseSaveFile (const juce::String& title, const juce::File& suggested, const juce::String& pattern, std::function<void (const juce::File&)> done)
{
    chooser = std::make_unique<juce::FileChooser> (title, suggested, pattern, false);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<MainComponent> (this), done] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              const auto f = fc.getResult();
                              if (f != juce::File() && done) done (f);
                          });
}

void MainComponent::showOverlay (std::unique_ptr<Overlay> o)
{
    overlay = std::move (o);
    addAndMakeVisible (*overlay);
    overlay->setBounds (getLocalBounds());
    overlay->toFront (true);
    overlay->grabKeyboardFocus();
    if (loupe != nullptr) loupe->toFront (false);
}

void MainComponent::closeOverlay()
{
    overlay.reset();
    grabKeyboardFocus();
}

void MainComponent::showAbout (bool show)
{
    if (show) showOverlay (std::make_unique<AboutPanel> (*this));
    else closeOverlay();
}

// ---- painting
void MainComponent::paintLogo (juce::Graphics& g, juce::Rectangle<float> r) const
{
    if (iconImage.isValid())
    {
        juce::Path clip;
        clip.addRoundedRectangle (r, r.getHeight() * 0.18f);
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (clip);
        g.drawImage (iconImage, r, juce::RectanglePlacement::centred);
    }
}

void MainComponent::paintTitle (juce::Graphics& g, juce::Rectangle<int> r, float height) const
{
    const auto& th = theme();
    static const char* letters[] = { "S", "I", "D", "i", "M", "I", "D", "I" };
    static const int colourOf[] = { 0, 7, 3, 2, 6, 1, 5, 4 };
    auto f = font (height, true);
    g.setFont (f);
    float x = (float) r.getX();
    for (int i = 0; i < 8; ++i)
    {
        const float w = juce::GlyphArrangement::getStringWidth (f, letters[i]);
        g.setColour (th.track[(size_t) colourOf[i]]);
        g.drawText (letters[i], juce::Rectangle<float> (x, (float) r.getY(), w + 2.0f, (float) r.getHeight()), juce::Justification::centredLeft, false);
        x += w - (i == 2 ? 1.0f : 0.0f);
    }
}

void MainComponent::paint (juce::Graphics& g)
{
    const auto& th = theme();
    g.fillAll (th.bg);
    g.setColour (th.panel);
    g.fillRect (headerArea);
    paintTitle (g, logoArea, 27.0f);

    // status bar
    g.setColour (th.panel);
    g.fillRect (statusArea);
    g.setColour (th.dim);
    g.setFont (font (12.5f));
    juce::String s;
    if (tab == 0)
    {
        s << T ("%1 Dateien  ·  %2 angezeigt", (int) arch->entries().size(), sidPane->table().numRows());
        double secs = 0;
        const int n = sidPane->table().totalDurationOfShown (secs);
        if (n > 0) s << U ("  ·  ") << T ("Spieldauer %1", formatLong (secs));
        for (auto& h : arch->hvscList()) s << U ("  ·  HVSC #") << h->version;
        if (arch->isScanning()) s << U ("  ·  ") << arch->scanStatus();
    }
    else s << midiPane->statusSummary();
    if (statusText.isNotEmpty()) s << U ("  ·  ") << statusText;
    g.drawText (s, statusArea.reduced (10, 0), juce::Justification::centredLeft, true);
    if (tab == 0 && arch->isScanning() && arch->scanTotal() > 0)
    {
        g.setColour (th.accent);
        g.fillRect (statusArea.getX(), statusArea.getY(), (int) ((double) statusArea.getWidth() * arch->scanDone() / arch->scanTotal()), 2);
    }
}

void MainComponent::resized()
{
    if (catcher != nullptr) { catcher->setBounds (getLocalBounds()); catcher->toFront (false); }
    if (loupe != nullptr) loupe->toFront (false);
    auto r = getLocalBounds();
    headerArea = r.removeFromTop (52);
    auto h = headerArea.reduced (10, 8);
    logoArea = h.removeFromLeft (135);
    h.removeFromLeft (14);
    tabSid.setBounds (h.removeFromLeft (130)); h.removeFromLeft (8);
    tabMidi.setBounds (h.removeFromLeft (130));
    aboutBtn.setBounds (h.removeFromRight (34)); h.removeFromRight (6);
    settingsBtn.setBounds (h.removeFromRight (120)); h.removeFromRight (12);
    if (updateBtn.isVisible()) updateBtn.setBounds (h.removeFromRight (juce::jmin (210, h.getWidth() / 3)));
    // the three oscillators sit behind the tabs, as far as there is room
    h.removeFromLeft (16);
    {
        const int gap = 6, avail = h.getWidth() - 10;
        const int w = juce::jmin (190, (avail - 2 * gap) / 3);
        for (int v = 0; v < 3; ++v)
        {
            scopes[v]->setVisible (w >= 70);
            if (w >= 70) scopes[v]->setBounds (h.removeFromLeft (w)), h.removeFromLeft (gap);
        }
    }
    statusArea = r.removeFromBottom (26);
    contentArea = r;
    sidPane->setBounds (r);
    midiPane->setBounds (r);
    if (overlay != nullptr) overlay->setBounds (getLocalBounds());
}

void MainComponent::timerCallback()
{
    refreshUpdateHint();
    if (arch->isScanning()) repaint (statusArea);
    if (loupe != nullptr && (loupe->isVisible() || (bool) arch->settings.getProperty ("loupeOn", false))) updateLoupe();
}

void MainComponent::mouseDown (const juce::MouseEvent&) { grabKeyboardFocus(); }

void MainComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        const double cur = currentScale();
        setUiScaleSetting (juce::jlimit (0.5, 3.0, cur * std::pow (1.1, (double) w.deltaY * 4.0)));
    }
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (mods.isCtrlDown() || mods.isCommandDown())
    {
        const auto c = k.getTextCharacter();
        if (c == '+' || c == '=') { setUiScaleSetting (juce::jlimit (0.5, 3.0, currentScale() * 1.1)); return true; }
        if (c == '-') { setUiScaleSetting (juce::jlimit (0.5, 3.0, currentScale() / 1.1)); return true; }
        if (c == '0') { setUiScaleSetting (0.0); return true; }
    }
    if (k == juce::KeyPress::F1Key) { showAbout (overlay == nullptr); return true; }
    if (overlay != nullptr) return false;
    if (k == juce::KeyPress ('1', juce::ModifierKeys::altModifier, 0)) { selectTab (0); return true; }
    if (k == juce::KeyPress ('2', juce::ModifierKeys::altModifier, 0)) { selectTab (1); return true; }
    return tab == 0 ? sidPane->handleKey (k) : midiPane->handleKey (k);
}

void MainComponent::filesDropped (const juce::StringArray& files, int, int)
{
    juce::StringArray sidFolders, midiFolders;
    for (const auto& p : files)
    {
        const juce::File f (p);
        const auto folder = f.isDirectory() ? f : f.getParentDirectory();
        if (f.hasFileExtension ("mid;midi;kar;rmi")) midiFolders.add (folder.getFullPathName());
        else sidFolders.add (folder.getFullPathName());
    }
    if (! midiFolders.isEmpty() && sidFolders.isEmpty()) { selectTab (1); midiPane->openFolder (juce::File (midiFolders[0])); }
    else if (! sidFolders.isEmpty()) { selectTab (0); sidPane->addFolders (sidFolders); }
}

void MainComponent::setUiScaleSetting (double s)
{
    arch->settings.setProperty ("uiScale", s, nullptr);
    arch->settingsChanged();
    if (onScaleSettingChanged) onScaleSettingChanged();
}

// ---- update hint
void MainComponent::refreshUpdateHint()
{
    const auto v = updateCheck->newerVersion();
    if (v == shownUpdate && updateBtn.isVisible() == v.isNotEmpty()) return;
    shownUpdate = v;
    updateBtn.setButtonText (T ("Update %1 verfügbar", v));
    updateBtn.setTooltip (T ("SIDiMIDI %1 ist erschienen (installiert: %2) – klicken für den Download", v, SM_VERSION_STRING));
    updateBtn.setVisible (v.isNotEmpty());
    resized();
}

void MainComponent::showUpdateMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader (T ("SIDiMIDI %1 ist erschienen", shownUpdate));
    m.addItem (1, T ("Download-Seite öffnen …"));
    m.addItem (2, T ("Bis zum nächsten Start ausblenden"));
    m.addSeparator();
    m.addItem (3, T ("Nicht mehr nach Updates suchen"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (updateBtn), [safe = juce::Component::SafePointer<MainComponent> (this)] (int r)
    {
        if (safe == nullptr || r == 0) return;
        if (r == 1) juce::URL (UpdateCheck::kReleasesUrl).launchInDefaultBrowser();
        else if (r == 2) safe->updateCheck->dismiss();
        else if (r == 3) safe->updateCheck->setEnabled (false);
        safe->refreshUpdateHint();
    });
}

// ---- settings
void MainComponent::showSettingsMenu()
{
    auto& st = arch->settings;
    juce::PopupMenu m, th, scale, lang;
    const int cur = st.getProperty ("theme", 0);
    for (int i = 0; i < (int) themes().size(); ++i) th.addItem (100 + i, themes()[(size_t) i].name, true, i == cur);
    th.addSeparator();
    th.addItem (150, T ("Hell"), true, (bool) st.getProperty ("light", false));
    m.addSubMenu (T ("Farbthema"), th);

    lang.addItem (400, "Deutsch", true, language() == Lang::de);
    lang.addItem (401, "English", true, language() == Lang::en);
    lang.addItem (402, U ("Français"), true, language() == Lang::fr);
    m.addSubMenu (U ("Sprache · Language · Langue"), lang);

    const double cs = uiScaleSetting();
    scale.addItem (500, T ("Automatisch (folgt der Fenstergröße)"), true, cs <= 0.0);
    scale.addSeparator();
    for (int pct : { 60, 75, 90, 100, 115, 130, 150, 175, 200, 250, 300 })
        scale.addItem (500 + pct, juce::String (pct) + " %", true, cs > 0.0 && std::abs (cs * 100.0 - pct) < 1.5);
    scale.addSeparator();
    scale.addItem (490, T ("Größer (Strg +)"));
    scale.addItem (491, T ("Kleiner (Strg −)"));
    m.addSubMenu (T ("Skalierung der Oberfläche (stufenlos: Strg + Mausrad)"), scale);

    juce::PopupMenu loupeMenu;
    addLoupeMenu (loupeMenu, 900);
    m.addSubMenu (T ("Lupe (Rechtsklick in einen freien Bereich)"), loupeMenu);
    m.addSeparator();
    {
        const int newest = sidPane->hvscNewestVersion();
        m.addItem (21, newest > 0 ? T ("HVSC-Sammlung aktualisieren … (neu: #%1)", newest) : T ("HVSC-Sammlung prüfen und aktualisieren …"));
    }
    m.addItem (20, T ("Alle SID-Dateien neu einlesen (auch unveränderte)"), ! arch->isScanning());
    m.addItem (22, T ("Spaltenanordnung zurücksetzen"));
    m.addItem (23, T ("Einmal am Tag nach Updates suchen (GitHub)"), true, updateCheck->enabled());
    m.addSeparator();
    m.addItem (30, T ("Datenbank-Ordner öffnen"));
    m.addItem (31, T ("Spenden (PayPal) …"));
    m.addItem (32, T ("Webseite undabtanzbar.de …"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (settingsBtn), [safe = juce::Component::SafePointer<MainComponent> (this)] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& a = *safe->arch;
        if (r >= 100 && r < 150) { a.settings.setProperty ("theme", r - 100, nullptr); safe->applyTheme(); }
        else if (r == 150) { a.settings.setProperty ("light", ! (bool) a.settings.getProperty ("light", false), nullptr); safe->applyTheme(); }
        else if (r >= 400 && r <= 402) { chooseLanguage (a.settings, (Lang) (r - 400)); safe->applyLanguage(); }
        else if (r == 500) safe->setUiScaleSetting (0.0);
        else if (r > 500 && r <= 800) safe->setUiScaleSetting ((r - 500) / 100.0);
        else if (r == 490) safe->setUiScaleSetting (juce::jlimit (0.5, 3.0, safe->currentScale() * 1.1));
        else if (r == 491) safe->setUiScaleSetting (juce::jlimit (0.5, 3.0, safe->currentScale() / 1.1));
        else if (r >= 900 && r < 920) safe->handleLoupeMenu (r, 900);
        else if (r == 20) a.startScan (true);
        else if (r == 21) safe->sidPane->showHvscMenu();
        else if (r == 22)
        {
            a.settings.removeProperty ("columns", nullptr);
            safe->sidPane->table().restoreLayout ({});
            safe->sidPane->saveState();
            safe->setStatus (T ("Spalten zurückgesetzt – gilt ab dem nächsten Start"));
        }
        else if (r == 23)
        {
            const bool on = ! safe->updateCheck->enabled();
            safe->updateCheck->setEnabled (on);
            safe->setStatus (on ? T ("Update-Suche an – gesendet wird nur die Anfrage an GitHub, keine Daten über dich oder dein Archiv") : T ("Update-Suche aus"));
            safe->refreshUpdateHint();
        }
        else if (r == 30) a.dataFolder().startAsProcess();
        else if (r == 31) juce::URL (AboutPanel::kDonateUrl).launchInDefaultBrowser();
        else if (r == 32) juce::URL (AboutPanel::kWebsiteUrl).launchInDefaultBrowser();
        a.settingsChanged();
    });
}

// ---- loupe
void MainComponent::addLoupeMenu (juce::PopupMenu& m, int id0)
{
    auto& ui = arch->settings;
    const bool on = ui.getProperty ("loupeOn", false);
    const int zoom = ui.getProperty ("loupeZoom", 250), size = ui.getProperty ("loupeSize", 1);
    m.addItem (id0, on ? T ("Lupe ausschalten") : T ("Lupe einschalten"));
    m.addSeparator();
    m.addSectionHeader (T ("Vergrößerung"));
    for (int i = 0; i < (int) std::size (ui::LoupeSettings::kZooms); ++i)
    {
        const int z = ui::LoupeSettings::kZooms[i];
        auto txt = juce::String (z / 100.0, z % 100 == 0 ? 0 : 1);
        if (language() != Lang::en) txt = txt.replaceCharacter ('.', ',');
        m.addItem (id0 + 2 + i, txt + juce::String::fromUTF8 ("×"), true, zoom == z);
    }
    m.addSeparator();
    m.addSectionHeader (T ("Lupengröße"));
    const juce::String sizes[] = { T ("klein"), T ("mittel"), T ("groß") };
    for (int i = 0; i < 3; ++i) m.addItem (id0 + 10 + i, sizes[i], true, size == i);
}

void MainComponent::handleLoupeMenu (int r, int id0)
{
    auto ui = arch->settings;
    if (r == id0) ui.setProperty ("loupeOn", ! (bool) ui.getProperty ("loupeOn", false), nullptr);
    else if (r >= id0 + 2 && r < id0 + 2 + (int) std::size (ui::LoupeSettings::kZooms))
    { ui.setProperty ("loupeZoom", ui::LoupeSettings::kZooms[r - id0 - 2], nullptr); ui.setProperty ("loupeOn", true, nullptr); }
    else if (r >= id0 + 10 && r < id0 + 13) { ui.setProperty ("loupeSize", r - id0 - 10, nullptr); ui.setProperty ("loupeOn", true, nullptr); }
    arch->settingsChanged();
    updateLoupe();
}

void MainComponent::showLoupeMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader (T ("Lupe"));
    addLoupeMenu (m, 1);
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe = juce::Component::SafePointer<MainComponent> (this)] (int r)
    {
        if (safe != nullptr && r != 0) safe->handleLoupeMenu (r, 1);
    });
}

void MainComponent::showLoupeAt (juce::Point<int> p)
{
    arch->settings.setProperty ("loupeOn", true, nullptr);
    loupeTestPos = p;
    updateLoupe();
}

void MainComponent::updateLoupe()
{
    if (loupe == nullptr) return;
    auto& ui = arch->settings;
    const auto mouse = loupeTestPos.x >= 0 ? loupeTestPos : getMouseXYRelative();
    if (! (bool) ui.getProperty ("loupeOn", false) || ! getLocalBounds().contains (mouse) || (loupeTestPos.x < 0 && ! isShowing()))
    {
        loupe->setVisible (false);
        return;
    }
    const float zoom = (float) (int) ui.getProperty ("loupeZoom", 250) / 100.0f;
    const auto size = ui::LoupeSettings::windowSize ((int) ui.getProperty ("loupeSize", 1));
    const int gap = 90;
    const juce::Point<int> candidates[] = { { mouse.x + gap, mouse.y + gap }, { mouse.x - gap - size.x, mouse.y + gap },
                                            { mouse.x + gap, mouse.y - gap - size.y }, { mouse.x - gap - size.x, mouse.y - gap - size.y } };
    auto pos = candidates[0];
    for (auto c : candidates)
        if (getLocalBounds().contains (juce::Rectangle<int> (c.x, c.y, size.x, size.y))) { pos = c; break; }
    const auto area = juce::Rectangle<float> ((float) size.x / zoom, (float) size.y / zoom).withCentre (mouse.toFloat()).getSmallestIntegerContainer();
    float display = 1.0f;
    if (auto* d = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds())) display = (float) d->scale;
    display *= (float) std::max (0.5, scaleNow);
    loupe->setVisible (false);
    auto img = createComponentSnapshot (area, true, zoom * display);
    loupe->setBounds (juce::jlimit (0, juce::jmax (0, getWidth() - size.x), pos.x), juce::jlimit (0, juce::jmax (0, getHeight() - size.y), pos.y), size.x, size.y);
    loupe->frame = theme().accent;
    loupe->setImage (img);
    loupe->setVisible (true);
    loupe->toFront (false);
}
} // namespace sm
