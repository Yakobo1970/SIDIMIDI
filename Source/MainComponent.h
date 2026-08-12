#pragma once

#include <JuceHeader.h>
#include "SidToMidiCore.h"
#include "AudioPlayerEngine.h"
#include "MidiToSidCore.h"
#include <thread>
#include <algorithm>

//==============================================================================
/**
    Malt eine halbtransparente C64-Cyan-Markierung über die Zeile eines TreeView, über der
    sich gerade die Maus befindet. JUCEs TreeView/FileTreeComponent bietet dafür keinen
    eingebauten Mechanismus (nur Auswahl-Highlighting), daher als schwebendes Overlay
    realisiert, das Klicks unverändert an den darunterliegenden Baum durchreicht.

    Übernimmt daneben auch die Rechtsklick-Erkennung für das Datei-Kontextmenü (Löschen/
    Verschieben) - beides sind "Mausposition -> Baum-Item"-Belange, die inhaltlich
    zusammengehören; die eigentliche Lösch-/Verschiebe-Logik bleibt aber über den
    onRightClicked-Callback beim Aufrufer (MainComponent), damit diese Klasse ein reiner
    Maus-Interaktions-Helfer bleibt statt Dateisystem-Operationen selbst auszuführen.
*/
class TreeHoverOverlay : public juce::Component
{
public:
    TreeHoverOverlay() = default;

    void attachTo(juce::FileTreeComponent& treeToWatch)
    {
        tree = &treeToWatch;
        tree->addMouseListener(this, true);
        setInterceptsMouseClicks(false, false);
    }

    ~TreeHoverOverlay() override
    {
        if (tree != nullptr)
            tree->removeMouseListener(this);
    }

    void paint(juce::Graphics& g) override
    {
        if (hoverRect.isEmpty())
            return;

        g.setColour(juce::Colours::cyan.withAlpha(0.28f));
        g.fillRect(hoverRect);
    }

    // Wird mit der rechtsgeklickten Datei aufgerufen. JUCEs TreeView wählt das Item unter
    // dem Mauszeiger bereits bei mouseDown aus, unabhängig von der gedrückten Maustaste
    // (siehe TreeView::mouseDownInternal) - getSelectedFile() bei mouseUp ist daher bereits
    // zuverlässig die rechtsgeklickte Datei, ganz ohne eigene Item-Positions-Berechnung.
    std::function<void(const juce::File&)> onRightClicked;

private:
    void mouseMove(const juce::MouseEvent& e) override { updateHover(e); }
    void mouseEnter(const juce::MouseEvent& e) override { updateHover(e); }
    void mouseExit(const juce::MouseEvent&) override { setHoverRect({}); }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (tree != nullptr && e.mods.isPopupMenu() && onRightClicked)
        {
            auto file = tree->getSelectedFile();
            if (file.existsAsFile())
                onRightClicked(file);
        }
    }

    void updateHover(const juce::MouseEvent& e)
    {
        if (tree == nullptr) { setHoverRect({}); return; }

        auto posInTree = e.getEventRelativeTo(tree).getPosition();
        if (auto* item = tree->getItemAt(posInTree.y))
        {
            auto itemRect = item->getItemPosition(true);
            setHoverRect({ 0, itemRect.getY(), getWidth(), itemRect.getHeight() });
        }
        else
        {
            setHoverRect({});
        }
    }

    void setHoverRect(juce::Rectangle<int> r)
    {
        if (r != hoverRect)
        {
            hoverRect = r;
            repaint();
        }
    }

    juce::FileTreeComponent* tree = nullptr;
    juce::Rectangle<int> hoverRect;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TreeHoverOverlay)
};

//==============================================================================
/**
    Zeigt Live-Daten einer SID-Voice: bei "Play SID" den Oszillator als kleine
    Wellenform-Spur, bei "Play MIDI" die (rein visuelle) Hüllkurve der gespielten Note auf
    dem zugehörigen Kanal (2/3/4 -> Voice 0/1/2). Fragt dafür periodisch AudioPlayerEngine::
    getVoiceDisplaySnapshot() ab, die selbst thread-sicher (eigenes Lock) ist.
*/
class VoiceDisplay : public juce::Component,
    private juce::Timer
{
public:
    VoiceDisplay(AudioPlayerEngine& engineToWatch, int voiceIndexToShow)
        : engine(engineToWatch), voiceIndex(voiceIndexToShow)
    {
        startTimerHz(30);
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRect(bounds);

        AudioPlayerEngine::VoiceDisplayInfo info[3];
        engine.getVoiceDisplaySnapshot(info);
        const auto& v = info[voiceIndex];

        if (!v.active)
            return;

        // Bei SID-Wiedergabe die echte Oszillator-Wellenform, bei MIDI-Wiedergabe eine
        // Tonhöhen-Verlaufslinie (siehe AudioPlayerEngine::getVoiceDisplaySnapshot()) - in
        // beiden Fällen optisch dieselbe einfache grüne Linie, damit sich die MIDI-Anzeige an
        // die SID-Oszillator-Ansicht angleicht statt eines eigenen Pegelbalken-Stils.
        g.setColour(juce::Colours::lightgreen);
        juce::Path p;
        const int n = AudioPlayerEngine::kOscHistorySize;
        for (int i = 0; i < n; ++i)
        {
            int idx = (v.oscHistoryWritePos + i) % n;
            float sample = juce::jlimit(-1.0f, 1.0f, v.oscHistory[(size_t)idx]);
            float x = bounds.getX() + bounds.getWidth() * (float)i / (float)(n - 1);
            float y = bounds.getCentreY() - sample * bounds.getHeight() * 0.45f;
            if (i == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        g.strokePath(p, juce::PathStrokeType(1.5f));
    }

private:
    void timerCallback() override { repaint(); }

    AudioPlayerEngine& engine;
    int voiceIndex;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceDisplay)
};

//==============================================================================
/**
    Haupt-Benutzeroberfläche für den C64 SID zu MIDI/MPE Konverter.
*/
class MainComponent : public juce::Component,
    public juce::Button::Listener,
    public juce::ChangeListener,
    public juce::Timer,
    public juce::KeyListener,
    public juce::ListBoxModel
{
public:
    MainComponent()
    {
        thread.startThread();

        // Zifferntasten 1-4 wechseln während laufender Mehrstufen-MIDI-Wiedergabe zwischen
        // den 4 Auflösungsstufen (siehe keyPressed()/AudioPlayerEngine::setActiveTier()).
        // Als KeyListener statt reinem Component::keyPressed()-Override, damit die Tasten
        // unabhängig davon greifen, welches Kind-Element (Baum/Textfeld) gerade den
        // Tastaturfokus hält.
        setWantsKeyboardFocus(true);
        addKeyListener(this);

        sidDirectoryList.setDirectory(juce::File::getSpecialLocation(juce::File::userHomeDirectory), true, true);
        midiDirectoryList.setDirectory(juce::File::getSpecialLocation(juce::File::userHomeDirectory), true, true);

        // Dateibaum-Komponenten mit C64 Retro Look & sichtbarem Auswahl-Feedback
        addAndMakeVisible(sidTree);
        sidTree.setColour(juce::FileTreeComponent::backgroundColourId, juce::Colour::fromRGB(53, 49, 167));
        sidTree.setColour(juce::TreeView::backgroundColourId, juce::Colour::fromRGB(53, 49, 167));
        sidTree.setColour(juce::TreeView::selectedItemBackgroundColourId, juce::Colours::cyan);

        addAndMakeVisible(midiTree);
        midiTree.setColour(juce::FileTreeComponent::backgroundColourId, juce::Colour::fromRGB(53, 49, 167));
        midiTree.setColour(juce::TreeView::backgroundColourId, juce::Colour::fromRGB(53, 49, 167));
        midiTree.setColour(juce::TreeView::selectedItemBackgroundColourId, juce::Colours::orange);

        // Mouseover-Markierung in C64-Cyan über der jeweiligen Zeile (JUCE bietet dafür
        // keinen eingebauten Mechanismus, siehe TreeHoverOverlay).
        addAndMakeVisible(sidTreeHover);
        sidTreeHover.attachTo(sidTree);
        sidTreeHover.onRightClicked = [this](const juce::File& f) { showFileContextMenu(f, true); };
        addAndMakeVisible(midiTreeHover);
        midiTreeHover.attachTo(midiTree);
        midiTreeHover.onRightClicked = [this](const juce::File& f) { showFileContextMenu(f, false); };

        // Ersetzt midiTree (gleiche Fläche) während der manuellen "MIDI-SET auswählen"-
        // Stimmenauswahl - JUCEs FileTreeComponent kann keine virtuellen (nicht auf der
        // Festplatte liegenden) Kind-Elemente wie MIDI-Kanäle anzeigen, daher ein separates
        // ListBox statt einer echten Baum-Aufklappung (siehe startChannelPicker()).
        addAndMakeVisible(channelPickerList);
        channelPickerList.setModel(this);
        channelPickerList.setColour(juce::ListBox::backgroundColourId, juce::Colour::fromRGB(53, 49, 167));
        channelPickerList.setRowHeight(22);
        channelPickerList.setVisible(false);

        sidDirectoryList.addChangeListener(this);
        midiDirectoryList.addChangeListener(this);

        // Headergrafik aus den eingebetteten Binaries (per Projucer als GUI-Ressource
        // hinzugefügt, siehe JuceLibraryCode/BinaryData.h) - ersetzt die zuvor nie
        // positionierte (und damit unsichtbare) Textüberschrift in derselben Box.
        headerImage = juce::ImageFileFormat::loadFrom(BinaryData::sidimidi_gif, (size_t)BinaryData::sidimidi_gifSize);

        lblSidHeader.setText("1. SID QUELL-DATEIEN (.sid)", juce::dontSendNotification);
        lblSidHeader.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        lblSidHeader.setColour(juce::Label::textColourId, juce::Colours::cyan);
        lblSidHeader.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(lblSidHeader);

        lblMidiHeader.setText("2. MIDI ZIEL-DATEIEN (.mid)", juce::dontSendNotification);
        lblMidiHeader.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        lblMidiHeader.setColour(juce::Label::textColourId, juce::Colours::cyan);
        lblMidiHeader.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(lblMidiHeader);

        // Kopierbares Textfeld für Song-Infos und Diagnose-Logs
        txtLogInfo.setMultiLine(true);
        txtLogInfo.setReturnKeyStartsNewLine(true);
        txtLogInfo.setReadOnly(true);
        txtLogInfo.setCaretVisible(false);
        txtLogInfo.setScrollbarsShown(true);
        txtLogInfo.setText(greetingText() + "\n\n" + juce::String(juce::CharPointer_UTF8("Keine .sid-Datei gew\xc3\xa4hlt")));
        txtLogInfo.setFont(juce::FontOptions(11.0f, juce::Font::plain));
        txtLogInfo.setColour(juce::TextEditor::backgroundColourId, juce::Colour::fromRGB(25, 30, 40));
        txtLogInfo.setColour(juce::TextEditor::textColourId, juce::Colours::lightgrey);
        txtLogInfo.setColour(juce::TextEditor::outlineColourId, juce::Colours::cyan);
        addAndMakeVisible(txtLogInfo);

        lblSubsong.setText("Subsong:", juce::dontSendNotification);
        lblSubsong.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        lblSubsong.setColour(juce::Label::textColourId, juce::Colours::orange);
        addAndMakeVisible(lblSubsong);

        addAndMakeVisible(cmbSubsongs);
        cmbSubsongs.addItem("Song 1", 1);
        cmbSubsongs.setSelectedId(1);

        addAndMakeVisible(btnSourceDir);
        btnSourceDir.setButtonText("<< SID-Ordner");
        btnSourceDir.addListener(this);

        addAndMakeVisible(btnTargetDir);
        btnTargetDir.setButtonText("MIDI-Ordner >>");
        btnTargetDir.addListener(this);

        addAndMakeVisible(btnPlaySid);
        btnPlaySid.setButtonText("< Play SID");
        btnPlaySid.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(40, 70, 120));
        btnPlaySid.addListener(this);

        addAndMakeVisible(btnPlayMidi);
        btnPlayMidi.setButtonText("Play MIDI >");
        btnPlayMidi.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(30, 110, 80));
        btnPlayMidi.addListener(this);

        addAndMakeVisible(btnStop);
        btnStop.setButtonText("Stop");
        btnStop.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(120, 40, 40));
        btnStop.addListener(this);
        // Doppelklick löst zusätzlich einen härteren Reset aus (siehe mouseDoubleClick()) -
        // für den Fall, dass ein normaler Stop einen hängengebliebenen Synth/MIDI-Zustand
        // nicht sauber beendet.
        btnStop.addMouseListener(this, false);

        addAndMakeVisible(btnConvert);
        btnConvert.setButtonText("SID zu MIDI >>");
        btnConvert.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(20, 80, 160));
        btnConvert.addListener(this);

        // MIDI-zu-SID-Konvertierung: UI-Platzhalter, die eigentliche Konvertierungslogik
        // (Stimmenauswahl-Heuristik, miniplayer2-Datenformat-Encoder, DASM-Assemblierung)
        // ist noch nicht implementiert, siehe buttonClicked().
        addAndMakeVisible(btnMidiToSid);
        btnMidiToSid.setButtonText("<< MIDI zu SID");
        btnMidiToSid.setColour(juce::TextButton::buttonColourId, juce::Colour::fromRGB(160, 80, 20));
        btnMidiToSid.addListener(this);

        // Beschriftung nur noch als kurze Überschrift - der Rest der Box zeigt die
        // Live-Visualisierung (Oszillator bei SID-, Hüllkurve bei MIDI-Wiedergabe).
        // Kanalnummer ist seit den 4 MIDI-Auflösungsstufen tier-abhängig (siehe lblMidiTier
        // statt einer festen "Ch X"-Beschriftung, siehe AudioPlayerEngine::setActiveTier()).
        lblVoice1.setText("Voice 1", juce::dontSendNotification);
        lblVoice1.setJustificationType(juce::Justification::centred);
        lblVoice1.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        addAndMakeVisible(lblVoice1);
        addAndMakeVisible(voiceDisplay1);

        lblVoice2.setText("Voice 2", juce::dontSendNotification);
        lblVoice2.setJustificationType(juce::Justification::centred);
        lblVoice2.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        addAndMakeVisible(lblVoice2);
        addAndMakeVisible(voiceDisplay2);

        lblVoice3.setText("Voice 3", juce::dontSendNotification);
        lblVoice3.setJustificationType(juce::Justification::centred);
        lblVoice3.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        addAndMakeVisible(lblVoice3);
        addAndMakeVisible(voiceDisplay3);

        // Zeigt normalerweise die Begrüßung - wird nur während einer Mehrstufen-MIDI-
        // Wiedergabe kurzzeitig durch den Tasten-1-4-Hinweis ersetzt (siehe timerCallback()),
        // die Zeile bleibt also nie ungenutzt.
        lblMidiTier.setText(greetingText(), juce::dontSendNotification);
        lblMidiTier.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        lblMidiTier.setColour(juce::Label::textColourId, juce::Colours::orange);
        lblMidiTier.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(lblMidiTier);

        loadSettings();
        startTimer(150);

        setSize(1000, 680);
    }

    ~MainComponent() override
    {
        stopTimer();
        audioPlayer.stop();
        saveSettings();
        sidDirectoryList.removeChangeListener(this);
        midiDirectoryList.removeChangeListener(this);
        thread.stopThread(1000);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour::fromRGB(18, 22, 30));

        g.setColour(juce::Colour::fromRGB(35, 31, 167));
        g.fillRect(10, 10, getWidth() - 20, 50);
        if (headerImage.isValid())
            g.drawImageWithin(headerImage, 12, 12, getWidth() - 24, 46,
                juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
        g.setColour(juce::Colours::cyan);
        g.drawRect(10, 10, getWidth() - 20, 50, 2);

        auto voiceBounds = getLocalBounds();
        voiceBounds.removeFromTop(65);
        voiceBounds.removeFromBottom(20); // Platz für lblMidiTier, siehe resized()
        auto voiceArea = voiceBounds.removeFromBottom(110).reduced(10);
        int widthPerVoice = (voiceArea.getWidth() - 20) / 3;

        g.setColour(juce::Colours::green);
        g.drawRect(voiceArea.removeFromLeft(widthPerVoice), 1);
        voiceArea.removeFromLeft(10);
        g.drawRect(voiceArea.removeFromLeft(widthPerVoice), 1);
        voiceArea.removeFromLeft(10);
        g.drawRect(voiceArea, 1);
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        bounds.removeFromTop(65);

        lblMidiTier.setBounds(bounds.removeFromBottom(20).reduced(10, 0));

        auto bottomArea = bounds.removeFromBottom(110).reduced(10);
        int vWidth = (bottomArea.getWidth() - 20) / 3;

        auto voice1Box = bottomArea.removeFromLeft(vWidth);
        lblVoice1.setBounds(voice1Box.removeFromTop(18));
        voiceDisplay1.setBounds(voice1Box);
        bottomArea.removeFromLeft(10);

        auto voice2Box = bottomArea.removeFromLeft(vWidth);
        lblVoice2.setBounds(voice2Box.removeFromTop(18));
        voiceDisplay2.setBounds(voice2Box);
        bottomArea.removeFromLeft(10);

        auto voice3Box = bottomArea;
        lblVoice3.setBounds(voice3Box.removeFromTop(18));
        voiceDisplay3.setBounds(voice3Box);

        auto mainArea = bounds.reduced(10);
        int sideWidth = (mainArea.getWidth() - 200) / 2;

        auto leftArea = mainArea.removeFromLeft(sideWidth);
        lblSidHeader.setBounds(leftArea.removeFromTop(24));
        sidTree.setBounds(leftArea);
        sidTreeHover.setBounds(sidTree.getBounds());

        auto rightArea = mainArea.removeFromRight(sideWidth);
        lblMidiHeader.setBounds(rightArea.removeFromTop(24));
        midiTree.setBounds(rightArea);
        midiTreeHover.setBounds(midiTree.getBounds());
        channelPickerList.setBounds(midiTree.getBounds());

        auto centerArea = mainArea.reduced(8, 0);

        btnSourceDir.setBounds(centerArea.removeFromTop(30));
        centerArea.removeFromTop(6);
        btnTargetDir.setBounds(centerArea.removeFromTop(30));
        centerArea.removeFromTop(12);

        auto subsongArea = centerArea.removeFromTop(28);
        lblSubsong.setBounds(subsongArea.removeFromLeft(65));
        cmbSubsongs.setBounds(subsongArea);
        centerArea.removeFromTop(12);

        btnPlaySid.setBounds(centerArea.removeFromTop(30));
        centerArea.removeFromTop(6);
        btnPlayMidi.setBounds(centerArea.removeFromTop(30));
        centerArea.removeFromTop(6);
        btnStop.setBounds(centerArea.removeFromTop(30));
        centerArea.removeFromTop(12);

        btnMidiToSid.setBounds(centerArea.removeFromBottom(40));
        centerArea.removeFromBottom(8);
        btnConvert.setBounds(centerArea.removeFromBottom(40));
        centerArea.removeFromBottom(8);
        txtLogInfo.setBounds(centerArea);
    }

    void timerCallback() override
    {
        // Beide Bäume unabhängig voneinander auf eine ECHTE Änderung ihrer eigenen
        // Auswahl prüfen (nicht per if/else if gegeneinander), sonst bleibt die
        // SID-Baum-Auswahl (die nach einer Konvertierung immer noch auf der zuletzt
        // gewählten .sid-Datei steht) für immer im Vorrang, und ein späterer Klick auf
        // eine andere MIDI-Datei wird nie mehr erkannt - der Baum markiert sie zwar
        // optisch, aber selectedMidiFile bleibt auf der zuletzt konvertierten Datei stehen.
        auto sidFile = sidTree.getSelectedFile();
        if (sidFile != lastPolledSidTreeFile)
        {
            lastPolledSidTreeFile = sidFile;
            if (sidFile.existsAsFile() && sidFile.getFileExtension().equalsIgnoreCase(".sid"))
            {
                selectedSidFile = sidFile;
                selectedMidiFile = juce::File();
                updateSidFileInfo(selectedSidFile);
            }
        }

        auto midiFile = midiTree.getSelectedFile();
        if (midiFile != lastPolledMidiTreeFile)
        {
            lastPolledMidiTreeFile = midiFile;
            if (midiFile.existsAsFile() && midiFile.getFileExtension().equalsIgnoreCase(".mid"))
            {
                selectedMidiFile = midiFile;
                selectedSidFile = juce::File();
                txtLogInfo.moveCaretToEnd();
                txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\nGew\xc3\xa4hlt (MIDI): ")) + selectedMidiFile.getFileName() + "\n");
            }
        }

        // Stufen-Anzeige laufend an den Engine-Zustand angleichen (statt an jeder einzelnen
        // Stelle, die die Wiedergabe verändert, manuell nachzuziehen - Play/Stop/Tastenwechsel
        // laufen so automatisch mit).
        if (channelPickerActive)
        {
            lblMidiTier.setText(juce::String(juce::CharPointer_UTF8("W\xc3\xa4hle 3 Spuren zur Konvertierung ("))
                + juce::String((int)pickerSelectedTracks.size())
                + juce::String(juce::CharPointer_UTF8("/3 gew\xc3\xa4hlt)")),
                juce::dontSendNotification);
        }
        else if (audioPlayer.isPlaying() && audioPlayer.isCurrentFileMultiRes())
        {
            int tier = audioPlayer.getActiveTierIndex();
            lblMidiTier.setText("MIDI-Stufe " + juce::String(tier + 1) + "/4: "
                + juce::String(AudioPlayerEngine::tierDisplayName(tier)) + "  (Tasten 1-4 wechseln)",
                juce::dontSendNotification);
        }
        else
        {
            lblMidiTier.setText(greetingText(), juce::dontSendNotification);
        }
    }

    // Zifferntasten 1-4: Auflösungsstufe der laufenden Mehrstufen-MIDI-Wiedergabe wechseln
    // (siehe AudioPlayerEngine::setActiveTier()). Als KeyListener registriert (siehe
    // Konstruktor), damit es unabhängig vom aktuellen Tastaturfokus-Kind greift.
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        auto ch = key.getTextCharacter();
        if (ch >= '1' && ch <= '4')
        {
            int tierIdx = (int)(ch - '1');
            if (audioPlayer.setActiveTier(tierIdx))
            {
                txtLogInfo.moveCaretToEnd();
                txtLogInfo.insertTextAtCaret("[MIDI] Stufe " + juce::String(tierIdx + 1) + ": "
                    + juce::String(AudioPlayerEngine::tierDisplayName(tierIdx)) + "\n");
                return true;
            }
        }
        return false;
    }

    // Doppelklick auf "Stop" (via btnStop.addMouseListener(this, false) im Konstruktor):
    // löst zusätzlich zum normalen Stop (der bei jedem Einzelklick bereits über
    // buttonClicked() läuft) einen härteren Reset aus - für einen hängengebliebenen
    // Synth/Audio-/MIDI-Zustand, den ein normales stop() nicht sauber beendet.
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.eventComponent != &btnStop)
            return;

        audioPlayer.hardReset();
        txtLogInfo.moveCaretToEnd();
        txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
            "\n[STOP] Harter Reset (Doppelklick) - Audioger\xc3\xa4t und MIDI-Ausgabe neu gestartet.\n")));
    }

    void buttonClicked(juce::Button* button) override
    {
        if (button == &btnSourceDir)
        {
            selectFolder(juce::String(juce::CharPointer_UTF8("SID-Quellverzeichnis w\xc3\xa4hlen...")), sidDirectoryList);
        }
        else if (button == &btnTargetDir)
        {
            selectFolder(juce::String(juce::CharPointer_UTF8("MIDI-Zielverzeichnis w\xc3\xa4hlen...")), midiDirectoryList);
        }
        else if (button == &btnPlaySid)
        {
            if (selectedSidFile.existsAsFile())
            {
                int subsong = cmbSubsongs.getSelectedId();
                juce::String playLog;
                playLog += "\n[PLAY] Starte SID Wiedergabe (Song " + juce::String(subsong) + ")...\n";

                bool success = audioPlayer.playSidFile(selectedSidFile, subsong, &playLog);

                txtLogInfo.moveCaretToEnd();
                txtLogInfo.insertTextAtCaret(playLog);

                if (!success)
                {
                    txtLogInfo.insertTextAtCaret("[FEHLER] Wiedergabe konnte nicht gestartet werden.\n");
                }
            }
            else
            {
                txtLogInfo.moveCaretToEnd();
                txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\n[FEHLER] Bitte zuerst eine .sid Datei w\xc3\xa4hlen!\n")));
            }
        }
        else if (button == &btnPlayMidi)
        {
            if (!selectedMidiFile.existsAsFile())
                selectedMidiFile = midiTree.getSelectedFile();

            if (selectedMidiFile.existsAsFile() && selectedMidiFile.getFileExtension().equalsIgnoreCase(".mid"))
            {
                juce::String playLog;
                bool started = audioPlayer.playMidiFile(selectedMidiFile, &playLog);

                txtLogInfo.moveCaretToEnd();
                if (started)
                {
                    txtLogInfo.insertTextAtCaret("\n[PLAY MIDI] " + selectedMidiFile.getFileName() + "\n");
                }
                else
                {
                    txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\n[FEHLER] Fehler beim Laden der MIDI-Datei!\n")) + playLog);
                }
            }
            else
            {
                txtLogInfo.moveCaretToEnd();
                txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\n[FEHLER] Bitte zuerst eine .mid Datei rechts w\xc3\xa4hlen!\n")));
            }
        }
        else if (button == &btnStop)
        {
            audioPlayer.stop();
            txtLogInfo.moveCaretToEnd();
            txtLogInfo.insertTextAtCaret("\n[STOP] Wiedergabe gestoppt.\n");

            if (channelPickerActive)
            {
                exitChannelPicker();
                txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("[MIDI-SET] Auswahl abgebrochen.\n")));
            }
        }
        else if (button == &btnConvert)
        {
            startConversion();
        }
        else if (button == &btnMidiToSid)
        {
            startMidiToSidConversion();
        }
    }

    void changeListenerCallback(juce::ChangeBroadcaster* source) override
    {
        if (source == &sidDirectoryList)
        {
            auto selectedFile = sidTree.getSelectedFile();
            if (selectedFile.existsAsFile() && selectedFile.getFileExtension().equalsIgnoreCase(".sid"))
            {
                selectedSidFile = selectedFile;
                updateSidFileInfo(selectedSidFile);
            }
        }
    }

private:
    static juce::String greetingText()
    {
        return "SIDiMIDI (c)2026 by Un(d)AbtanzBAR and Claude AI.";
    }

    void updateSidFileInfo(const juce::File& file)
    {
        SidFileHeader header;
        std::vector<uint8_t> codeData;
        juce::String parseLog;

        if (SidToMidiCoreEngine::parseSidHeader(file, header, codeData, &parseLog))
        {
            juce::String title = juce::String(header.title).trim();
            if (title.isEmpty()) title = file.getFileNameWithoutExtension();

            juce::String infoText;
            infoText += "========================================\n";
            infoText += "Datei: " + file.getFileName() + "\n";
            infoText += "Titel: " + title + "\n";
            infoText += "Autor: " + juce::String(header.author).trim() + "\n";
            infoText += "Jahr/Release: " + juce::String(header.released).trim() + "\n";
            infoText += "Subsongs: " + juce::String(header.songs) + " (Start: " + juce::String(header.startSong) + ")\n";
            infoText += "Load: 0x" + juce::String::toHexString(header.loadAddress) + "\n";
            infoText += "Init: 0x" + juce::String::toHexString(header.initAddress) + "\n";
            infoText += "Play: 0x" + juce::String::toHexString(header.playAddress) + "\n";
            infoText += parseLog;

            txtLogInfo.setText(infoText);
            txtLogInfo.moveCaretToEnd();

            cmbSubsongs.clear();
            // PSID/RSID begrenzt die Songanzahl spezifikationsgemäß auf 256;
            // ohne Deckelung könnte eine präparierte Datei (header.songs bis
            // 65535) hier kurzzeitig die UI mit Zehntausenden Einträgen einfrieren.
            constexpr int maxSubsongs = 256;
            int totalSongs = std::clamp((int)header.songs, 1, maxSubsongs);
            for (int i = 1; i <= totalSongs; ++i)
            {
                cmbSubsongs.addItem("Song " + juce::String(i) + " von " + juce::String(totalSongs), i);
            }

            int defaultSong = std::clamp((int)header.startSong, 1, totalSongs);
            cmbSubsongs.setSelectedId(defaultSong);
        }
    }

    // Rechtsklick-Kontextmenü für sidTree/midiTree: Löschen oder Verschieben in den jeweils
    // anderen Ordner (SID<->MIDI). fromSidTree bestimmt sowohl das Ziel des "Verschieben"-
    // Eintrags als auch, welche der beiden DirectoryContentsList danach aufgefrischt wird.
    void showFileContextMenu(const juce::File& file, bool fromSidTree)
    {
        juce::String otherLabel = fromSidTree ? "MIDI-Ordner" : "SID-Ordner";

        juce::PopupMenu menu;
        menu.addItem(1, juce::String(juce::CharPointer_UTF8("L\xc3\xb6schen")));
        menu.addItem(2, juce::String(juce::CharPointer_UTF8("Verschieben nach ")) + otherLabel);
        menu.addItem(3, juce::String(juce::CharPointer_UTF8("Umbenennen...")));

        // "MIDI-SET ausw\xc3\xa4hlen" nur f\xc3\xbcr .mid-Dateien im MIDI-Baum - startet die
        // manuelle Stimmenauswahl (siehe startChannelPicker()).
        bool offerChannelPicker = !fromSidTree && file.getFileExtension().equalsIgnoreCase(".mid");
        if (offerChannelPicker)
            menu.addItem(4, juce::String(juce::CharPointer_UTF8("MIDI-SET ausw\xc3\xa4hlen...")));

        juce::Component::SafePointer<MainComponent> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis, file, fromSidTree](int result)
            {
                if (safeThis == nullptr || result == 0)
                    return;
                safeThis->handleFileContextMenuResult(result, file, fromSidTree);
            });
    }

    void handleFileContextMenuResult(int result, const juce::File& file, bool fromSidTree)
    {
        auto& sourceList = fromSidTree ? sidDirectoryList : midiDirectoryList;
        auto& otherList = fromSidTree ? midiDirectoryList : sidDirectoryList;

        if (result == 1)
        {
            // Löschen ist unumkehrbar - vorher eine kurze Rückfrage, damit ein Fehlklick im
            // Kontextmenü keine Datei ungewollt endgültig entfernt.
            juce::Component::SafePointer<MainComponent> safeThis(this);
            auto options = juce::MessageBoxOptions::makeOptionsOkCancel(
                juce::MessageBoxIconType::WarningIcon,
                juce::String(juce::CharPointer_UTF8("Datei l\xc3\xb6schen")),
                juce::String(juce::CharPointer_UTF8("\"")) + file.getFileName()
                    + juce::String(juce::CharPointer_UTF8("\" wirklich unwiderruflich l\xc3\xb6schen?")),
                juce::String(juce::CharPointer_UTF8("L\xc3\xb6schen")),
                "Abbrechen", this);
            juce::AlertWindow::showAsync(options, [safeThis, file, fromSidTree](int choice)
                {
                    if (safeThis == nullptr || choice != 1)
                        return;
                    // sourceList erst hier (nicht als Referenz eingefangen) neu ermitteln - die
                    // lokale Referenz aus handleFileContextMenuResult() existiert zu diesem
                    // späteren, asynchronen Zeitpunkt längst nicht mehr.
                    auto& sourceListNow = fromSidTree ? safeThis->sidDirectoryList : safeThis->midiDirectoryList;
                    juce::String fileName = file.getFileName();
                    if (file.deleteFile())
                    {
                        safeThis->txtLogInfo.moveCaretToEnd();
                        safeThis->txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\n[GEL\xc3\x96SCHT] ")) + fileName + "\n");
                        sourceListNow.refresh();
                    }
                    else
                    {
                        safeThis->txtLogInfo.moveCaretToEnd();
                        safeThis->txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
                            "\n[FEHLER] Konnte ")) + fileName + juce::String(juce::CharPointer_UTF8(" nicht l\xc3\xb6schen.\n")));
                    }
                });
        }
        else if (result == 2)
        {
            juce::File target = otherList.getDirectory().getChildFile(file.getFileName());
            juce::String fileName = file.getFileName();
            if (target.existsAsFile())
            {
                txtLogInfo.moveCaretToEnd();
                txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
                    "\n[FEHLER] ")) + fileName + juce::String(juce::CharPointer_UTF8(
                    " existiert im Zielordner bereits, nicht verschoben.\n")));
                return;
            }
            if (file.moveFileTo(target))
            {
                txtLogInfo.moveCaretToEnd();
                txtLogInfo.insertTextAtCaret("\n[VERSCHOBEN] " + fileName + " -> "
                    + otherList.getDirectory().getFileName() + "\n");
                sourceList.refresh();
                otherList.refresh();
            }
            else
            {
                txtLogInfo.moveCaretToEnd();
                txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
                    "\n[FEHLER] Konnte ")) + fileName + juce::String(juce::CharPointer_UTF8(" nicht verschieben.\n")));
            }
        }
        else if (result == 3)
        {
            showRenameDialog(file, fromSidTree);
        }
        else if (result == 4)
        {
            startChannelPicker(file);
        }
    }

    // Eigenes std::unique_ptr-Member statt AlertWindows deleteWhenDismissed=true, damit der
    // Zeitpunkt der Zerstörung eindeutig hier im Callback liegt (nachdem getTextEditorContents()
    // gelesen wurde) statt von JUCEs interner Reihenfolge abzuhängen - gleiches Muster wie
    // fileChooser weiter unten. fromSidTree (statt einer eingefangenen DirectoryContentsList&)
    // wird bewusst als einfacher Wert mitgegeben, damit die Ziel-Liste erst im (später, nach
    // Rückgabe dieser Methode feuernden) Callback über safeThis-> neu ermittelt wird - siehe
    // dieselbe Überlegung bei handleFileContextMenuResult()'s Lösch-Bestätigung.
    void showRenameDialog(const juce::File& file, bool fromSidTree)
    {
        renameDialog = std::make_unique<juce::AlertWindow>(
            juce::String(juce::CharPointer_UTF8("Datei umbenennen")),
            juce::String(juce::CharPointer_UTF8("Neuer Name f\xc3\xbcr \"")) + file.getFileName() + "\":",
            juce::MessageBoxIconType::NoIcon);
        renameDialog->addTextEditor("newName", file.getFileNameWithoutExtension());
        renameDialog->addButton("Umbenennen", 1, juce::KeyPress(juce::KeyPress::returnKey));
        renameDialog->addButton("Abbrechen", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        juce::Component::SafePointer<MainComponent> safeThis(this);
        renameDialog->enterModalState(true, juce::ModalCallbackFunction::create(
            [safeThis, file, fromSidTree](int choice)
            {
                if (safeThis == nullptr)
                    return;
                juce::String newBaseName = safeThis->renameDialog != nullptr
                    ? safeThis->renameDialog->getTextEditorContents("newName").trim()
                    : juce::String();
                safeThis->renameDialog.reset();

                if (choice != 1 || newBaseName.isEmpty())
                    return;

                auto& sourceList = fromSidTree ? safeThis->sidDirectoryList : safeThis->midiDirectoryList;
                juce::File target = file.getSiblingFile(newBaseName + file.getFileExtension());
                if (target == file)
                    return;
                if (target.exists())
                {
                    safeThis->txtLogInfo.moveCaretToEnd();
                    safeThis->txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
                        "\n[FEHLER] ")) + target.getFileName() + juce::String(juce::CharPointer_UTF8(
                        " existiert bereits, nicht umbenannt.\n")));
                    return;
                }
                if (file.moveFileTo(target))
                {
                    safeThis->txtLogInfo.moveCaretToEnd();
                    safeThis->txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
                        "\n[UMBENANNT] ")) + file.getFileName() + " -> " + target.getFileName() + "\n");
                    sourceList.refresh();
                }
                else
                {
                    safeThis->txtLogInfo.moveCaretToEnd();
                    safeThis->txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
                        "\n[FEHLER] Konnte ")) + file.getFileName() + juce::String(juce::CharPointer_UTF8(" nicht umbenennen.\n")));
                }
            }), false);
    }

    //==============================================================================
    // "MIDI-SET auswählen": manuelle Stimmenauswahl statt der automatischen Notendichte-
    // Heuristik (MidiToSidCoreEngine::analyzeMidiFile). Da JUCEs FileTreeComponent keine
    // virtuellen (nicht auf der Festplatte liegenden) Kind-Elemente wie MIDI-Kanäle anzeigen
    // kann, wird midiTree währenddessen durch channelPickerList an derselben Stelle ersetzt
    // (siehe resized()) statt echter Baum-Aufklappung oder eines separaten Popup-Fensters.
    void startChannelPicker(const juce::File& midiFile)
    {
        juce::String log;
        std::vector<MidiToSidCoreEngine::ChannelSuggestion> tracks;
        if (!MidiToSidCoreEngine::listAllTracks(midiFile, tracks, log) || tracks.empty())
        {
            txtLogInfo.moveCaretToEnd();
            txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
                "\n[FEHLER] Keine Spuren mit Noten in ")) + midiFile.getFileName() + ".\n" + log);
            return;
        }

        audioPlayer.stop();

        pickerMidiFile = midiFile;
        pickerTracks = std::move(tracks);
        pickerSelectedTracks.clear();
        channelPickerActive = true;

        midiTree.setVisible(false);
        midiTreeHover.setVisible(false);
        channelPickerList.setVisible(true);
        channelPickerList.updateContent();
        channelPickerList.repaint();

        btnPlaySid.setEnabled(false);
        btnPlayMidi.setEnabled(false);
        btnConvert.setEnabled(false);
        btnMidiToSid.setEnabled(false);

        txtLogInfo.moveCaretToEnd();
        txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
            "\n[MIDI-SET] ")) + midiFile.getFileName() + juce::String(juce::CharPointer_UTF8(
            ": W\xc3\xa4hle 3 Spuren (Linksklick = vorh\xc3\xb6ren, Rechtsklick = ausw\xc3\xa4hlen, Stop = abbrechen).\n")));
    }

    void exitChannelPicker()
    {
        if (!channelPickerActive)
            return;

        channelPickerActive = false;
        pickerTracks.clear();
        pickerSelectedTracks.clear();
        pickerMidiFile = juce::File();

        channelPickerList.setVisible(false);
        midiTree.setVisible(true);
        midiTreeHover.setVisible(true);

        btnPlaySid.setEnabled(true);
        btnPlayMidi.setEnabled(true);
        btnConvert.setEnabled(true);
        btnMidiToSid.setEnabled(true);
    }

    // Sucht das ChannelSuggestion-Label (Trackname + Notenstatistik) zu einem per Rechtsklick
    // ausgewählten Spurindex - für Log-/Dialogtexte, die mehr als nur die rohe Indexzahl
    // anzeigen sollen.
    juce::String pickerTrackLabel(int trackIndex) const
    {
        for (auto& t : pickerTracks)
            if (t.trackIndex == trackIndex)
                return t.label;
        return "Spur " + juce::String(trackIndex);
    }

    void previewPickerChannel(int row)
    {
        if (row < 0 || row >= (int)pickerTracks.size())
            return;

        int trackIndex = pickerTracks[(size_t)row].trackIndex;
        juce::String log;
        bool started = audioPlayer.previewMidiTrack(pickerMidiFile, trackIndex, &log);

        txtLogInfo.moveCaretToEnd();
        if (started)
            txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\n[VORH\xc3\x96REN] ")) + pickerTrackLabel(trackIndex) + "\n");
        else
            txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\n[FEHLER] Spur konnte nicht vorgeh\xc3\xb6rt werden.\n")) + log);
    }

    static juce::String voiceRoleLabel(MidiToSidCoreEngine::VoiceRole role)
    {
        switch (role)
        {
        case MidiToSidCoreEngine::VoiceRole::Lead:  return "Lead";
        case MidiToSidCoreEngine::VoiceRole::Bass:  return "Bass";
        case MidiToSidCoreEngine::VoiceRole::Pad:   return "Pad";
        case MidiToSidCoreEngine::VoiceRole::Drums: return "Drums";
        case MidiToSidCoreEngine::VoiceRole::Auto:
        default:
            return juce::String(juce::CharPointer_UTF8("Automatisch"));
        }
    }

    void toggleChannelSelection(int row)
    {
        if (row < 0 || row >= (int)pickerTracks.size())
            return;

        int trackIndex = pickerTracks[(size_t)row].trackIndex;
        auto it = std::find_if(pickerSelectedTracks.begin(), pickerSelectedTracks.end(),
            [trackIndex](const auto& p) { return p.first == trackIndex; });
        if (it != pickerSelectedTracks.end())
        {
            pickerSelectedTracks.erase(it);
            channelPickerList.repaint();
            return;
        }

        if (pickerSelectedTracks.size() >= 3)
        {
            txtLogInfo.moveCaretToEnd();
            txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
                "\n[HINWEIS] Bereits 3 Spuren gew\xc3\xa4hlt - erst eine per Rechtsklick abw\xc3\xa4hlen.\n")));
            return;
        }

        showRolePickerMenu(trackIndex);
    }

    // Öffnet nach einem Rechtsklick auf eine noch nicht gewählte Spur ein Kontextmenü zur
    // Rollenwahl - erst nach der Auswahl wird die Spur (inkl. Rolle) zu pickerSelectedTracks
    // hinzugefügt. Das steuert direkt die Instrumentwahl in pickInstrument() und ersetzt bei
    // Bedarf die fehleranfällige Kanal-10-Drum-Erkennung (siehe MidiToSidCore.h).
    void showRolePickerMenu(int trackIndex)
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Lead");
        menu.addItem(2, "Bass");
        menu.addItem(3, "Pad");
        menu.addItem(4, "Drums");
        menu.addSeparator();
        menu.addItem(5, juce::String(juce::CharPointer_UTF8("Automatisch (GM-Programm)")));

        juce::Component::SafePointer<MainComponent> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis, trackIndex](int result)
            {
                if (safeThis == nullptr || result == 0)
                    return; // Menü ohne Auswahl geschlossen

                MidiToSidCoreEngine::VoiceRole role = MidiToSidCoreEngine::VoiceRole::Auto;
                switch (result)
                {
                case 1: role = MidiToSidCoreEngine::VoiceRole::Lead; break;
                case 2: role = MidiToSidCoreEngine::VoiceRole::Bass; break;
                case 3: role = MidiToSidCoreEngine::VoiceRole::Pad; break;
                case 4: role = MidiToSidCoreEngine::VoiceRole::Drums; break;
                default: break;
                }

                // Während das Menü offen war, könnte der Picker verlassen oder die 3. Stimme
                // anderweitig belegt worden sein - dann die verspätete Auswahl verwerfen.
                if (!safeThis->channelPickerActive || safeThis->pickerSelectedTracks.size() >= 3)
                    return;
                bool alreadyPicked = std::any_of(safeThis->pickerSelectedTracks.begin(), safeThis->pickerSelectedTracks.end(),
                    [trackIndex](const auto& p) { return p.first == trackIndex; });
                if (alreadyPicked)
                    return;

                safeThis->pickerSelectedTracks.push_back({ trackIndex, role });
                safeThis->channelPickerList.repaint();

                if (safeThis->pickerSelectedTracks.size() == 3)
                    safeThis->confirmChannelSelectionAsync();
            });
    }

    void confirmChannelSelectionAsync()
    {
        juce::String msg = juce::String(juce::CharPointer_UTF8("Mit ausgew\xc3\xa4hlten Spuren konvertieren?\n\n"));
        for (int i = 0; i < (int)pickerSelectedTracks.size(); ++i)
            msg += "Stimme " + juce::String(i + 1) + " (" + voiceRoleLabel(pickerSelectedTracks[(size_t)i].second) + "): "
                 + pickerTrackLabel(pickerSelectedTracks[(size_t)i].first) + "\n";

        juce::Component::SafePointer<MainComponent> safeThis(this);
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel(
            juce::MessageBoxIconType::QuestionIcon,
            "MIDI-SET",
            msg,
            "Konvertieren",
            "Abbrechen", this);
        juce::AlertWindow::showAsync(options, [safeThis](int choice)
            {
                if (safeThis == nullptr)
                    return;
                if (choice == 1)
                    safeThis->startMidiToSidConversionFromPicker();
                // Bei Abbruch bleibt der Picker aktiv - die 3. Auswahl kann per Rechtsklick
                // wieder abgewählt und neu getroffen werden.
            });
    }

    void startMidiToSidConversionFromPicker()
    {
        if (pickerSelectedTracks.size() != 3 || !pickerMidiFile.existsAsFile())
        {
            exitChannelPicker();
            return;
        }

        std::array<int, 3> trackIndices = { pickerSelectedTracks[0].first, pickerSelectedTracks[1].first, pickerSelectedTracks[2].first };
        std::array<MidiToSidCoreEngine::VoiceRole, 3> voiceRoles = { pickerSelectedTracks[0].second, pickerSelectedTracks[1].second, pickerSelectedTracks[2].second };
        juce::File midiFile = pickerMidiFile;
        juce::File targetDir = sidDirectoryList.getDirectory();
        juce::File targetSidFile = targetDir.getChildFile(midiFile.getFileNameWithoutExtension() + ".sid");

        exitChannelPicker();
        audioPlayer.stop();

        btnMidiToSid.setEnabled(false);
        btnMidiToSid.setButtonText("Konvertiere...");

        txtLogInfo.moveCaretToEnd();
        txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\n[MIDI ZU SID] Starte Konvertierung f\xc3\xbcr "))
            + midiFile.getFileName()
            + juce::String(juce::CharPointer_UTF8(" (Spuren "))
            + juce::String(trackIndices[0]) + "/" + juce::String(trackIndices[1]) + "/" + juce::String(trackIndices[2])
            + ")...\n");

        juce::Component::SafePointer<MainComponent> safeThis(this);

        std::thread([safeThis, midiFile, targetSidFile, trackIndices, voiceRoles]() {
            juce::String logOutput;
            bool success = MidiToSidCoreEngine::convertMidiToSidFile(midiFile, targetSidFile, trackIndices, false, logOutput, voiceRoles);

            juce::MessageManager::callAsync([safeThis, success, targetSidFile, logOutput]() {
                if (safeThis == nullptr)
                    return;

                safeThis->btnMidiToSid.setEnabled(true);
                safeThis->btnMidiToSid.setButtonText("<< MIDI zu SID");

                safeThis->txtLogInfo.moveCaretToEnd();
                safeThis->txtLogInfo.insertTextAtCaret(logOutput + (success
                    ? "[ERFOLG] Gespeichert: " + targetSidFile.getFileName() + "\n"
                    : juce::String(juce::CharPointer_UTF8("[FEHLER] Konvertierung fehlgeschlagen.\n"))));

                if (success)
                {
                    safeThis->sidDirectoryList.refresh();
                    safeThis->selectedSidFile = targetSidFile;
                    safeThis->sidTree.setSelectedFile(targetSidFile);
                }
                });
            }).detach();
    }

public:
    //==============================================================================
    // juce::ListBoxModel - Anzeige/Interaktion für channelPickerList (siehe startChannelPicker()).
    int getNumRows() override { return (int)pickerTracks.size(); }

    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool /*rowIsSelected*/) override
    {
        if (rowNumber < 0 || rowNumber >= (int)pickerTracks.size())
            return;

        const auto& ch = pickerTracks[(size_t)rowNumber];
        auto it = std::find_if(pickerSelectedTracks.begin(), pickerSelectedTracks.end(),
            [&ch](const auto& p) { return p.first == ch.trackIndex; });
        bool picked = it != pickerSelectedTracks.end();

        g.fillAll(picked ? juce::Colours::orange.withAlpha(0.35f) : juce::Colour::fromRGB(53, 49, 167));
        g.setColour(picked ? juce::Colours::orange : juce::Colours::white);

        juce::String prefix;
        if (picked)
            prefix = "[Stimme " + juce::String((int)std::distance(pickerSelectedTracks.begin(), it) + 1)
                    + juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")) + voiceRoleLabel(it->second) + "] ";

        g.drawText(prefix + ch.label, 6, 0, width - 12, height, juce::Justification::centredLeft);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            toggleChannelSelection(row);
        else
            previewPickerChannel(row);
    }

private:
    void selectFolder(const juce::String& title, juce::DirectoryContentsList& list)
    {
        fileChooser = std::make_unique<juce::FileChooser>(title, list.getDirectory());
        auto folderPickerFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

        fileChooser->launchAsync(folderPickerFlags, [&list, this](const juce::FileChooser& fc)
            {
                auto folder = fc.getResult();
                if (folder.isDirectory())
                {
                    list.setDirectory(folder, true, true);
                    saveSettings();
                }
            });
    }

    void startConversion()
    {
        if (!selectedSidFile.existsAsFile())
        {
            txtLogInfo.moveCaretToEnd();
            txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\n[FEHLER] Bitte w\xc3\xa4hle zuerst eine .sid Datei aus der linken Liste aus!\n")));
            return;
        }

        int subsong = cmbSubsongs.getSelectedId();
        juce::File targetDir = midiDirectoryList.getDirectory();

        juce::String midiFileName = selectedSidFile.getFileNameWithoutExtension();
        if (cmbSubsongs.getNumItems() > 1)
            midiFileName += "_Song" + juce::String(subsong);
        midiFileName += ".mid";

        juce::File targetMidiFile = targetDir.getChildFile(midiFileName);

        btnConvert.setEnabled(false);
        btnConvert.setButtonText("Konvertiere...");

        txtLogInfo.moveCaretToEnd();
        txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\n[KONVERTIERUNG] Starte Konvertierung f\xc3\xbcr ")) + selectedSidFile.getFileName() + " (Song " + juce::String(subsong) + ")...\n");

        // Der Konvertierungs-Thread läuft detached weiter, falls das Fenster
        // währenddessen geschlossen wird. Ein rohes "this" im späteren
        // callAsync wäre dann ein Use-after-free; SafePointer wird stattdessen
        // automatisch null, sobald MainComponent zerstört wurde.
        juce::Component::SafePointer<MainComponent> safeThis(this);

        std::thread([safeThis, sidFile = selectedSidFile, targetMidiFile, subsong]() {
            juce::String logOutput;
            bool success = SidToMidiCoreEngine::convertSidToMidiFile(sidFile, targetMidiFile, logOutput, subsong);

            juce::MessageManager::callAsync([safeThis, success, targetMidiFile, logOutput]() {
                if (safeThis == nullptr)
                    return;

                safeThis->btnConvert.setEnabled(true);
                safeThis->btnConvert.setButtonText("SID zu MIDI >>");

                safeThis->txtLogInfo.moveCaretToEnd();
                safeThis->txtLogInfo.insertTextAtCaret(logOutput + (success ? "[ERFOLG] Gespeichert: " + targetMidiFile.getFileName() + "\n" : "[FEHLER] Konvertierung fehlgeschlagen.\n"));

                if (success)
                {
                    safeThis->midiDirectoryList.refresh();
                    // Neu konvertierte Datei sofort selektieren, damit "Play MIDI >" ohne
                    // manuellen Klick im Datei-Browser funktioniert.
                    safeThis->selectedMidiFile = targetMidiFile;
                    safeThis->midiTree.setSelectedFile(targetMidiFile);
                }
                });
            }).detach();
    }

    // Stimmenauswahl aktuell automatisch (Notendichte-Heuristik, siehe
    // MidiToSidCoreEngine::analyzeMidiFile) ohne manuelle Korrektur-UI - das Ergebnis der
    // Analyse wird als Klartext geloggt, damit nachvollziehbar bleibt, welche Spuren
    // gewählt wurden. Eine manuelle Umschaltung (3 ComboBoxen) ist ein sinnvoller nächster
    // Ausbauschritt, aber kein Blocker für eine funktionierende erste Version.
    void startMidiToSidConversion()
    {
        if (!selectedMidiFile.existsAsFile())
            selectedMidiFile = midiTree.getSelectedFile();

        if (!selectedMidiFile.existsAsFile() || !selectedMidiFile.getFileExtension().equalsIgnoreCase(".mid"))
        {
            txtLogInfo.moveCaretToEnd();
            txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8(
                "\n[FEHLER] Bitte zuerst eine .mid Datei rechts w\xc3\xa4hlen!\n")));
            return;
        }

        juce::File targetDir = sidDirectoryList.getDirectory();
        juce::File targetSidFile = targetDir.getChildFile(selectedMidiFile.getFileNameWithoutExtension() + ".sid");

        btnMidiToSid.setEnabled(false);
        btnMidiToSid.setButtonText("Konvertiere...");

        txtLogInfo.moveCaretToEnd();
        txtLogInfo.insertTextAtCaret(juce::String(juce::CharPointer_UTF8("\n[MIDI ZU SID] Starte Konvertierung f\xc3\xbcr "))
            + selectedMidiFile.getFileName() + "...\n");

        juce::Component::SafePointer<MainComponent> safeThis(this);

        std::thread([safeThis, midiFile = selectedMidiFile, targetSidFile]() {
            juce::String logOutput;
            bool isRoundTrip = false;
            std::array<MidiToSidCoreEngine::ChannelSuggestion, 3> suggested;
            bool analyzeOk = MidiToSidCoreEngine::analyzeMidiFile(midiFile, isRoundTrip, suggested, logOutput);

            bool success = false;
            if (analyzeOk)
            {
                std::array<int, 3> trackIndices = { suggested[0].trackIndex, suggested[1].trackIndex, suggested[2].trackIndex };
                success = MidiToSidCoreEngine::convertMidiToSidFile(midiFile, targetSidFile, trackIndices, isRoundTrip, logOutput);
            }

            juce::MessageManager::callAsync([safeThis, success, targetSidFile, logOutput]() {
                if (safeThis == nullptr)
                    return;

                safeThis->btnMidiToSid.setEnabled(true);
                safeThis->btnMidiToSid.setButtonText("<< MIDI zu SID");

                safeThis->txtLogInfo.moveCaretToEnd();
                safeThis->txtLogInfo.insertTextAtCaret(logOutput + (success
                    ? "[ERFOLG] Gespeichert: " + targetSidFile.getFileName() + "\n"
                    : juce::String(juce::CharPointer_UTF8("[FEHLER] Konvertierung fehlgeschlagen.\n"))));

                if (success)
                {
                    safeThis->sidDirectoryList.refresh();
                    safeThis->selectedSidFile = targetSidFile;
                    safeThis->sidTree.setSelectedFile(targetSidFile);
                }
                });
            }).detach();
    }

    void saveSettings()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "SIDIMIDI";
        options.filenameSuffix = "settings";
        options.folderName = "SIDIMIDI";
        options.commonToAllUsers = false;
        appProperties.setStorageParameters(options);

        auto* settings = appProperties.getUserSettings();
        if (settings != nullptr)
        {
            settings->setValue("lastSidDir", sidDirectoryList.getDirectory().getFullPathName());
            settings->setValue("lastMidiDir", midiDirectoryList.getDirectory().getFullPathName());
            settings->saveIfNeeded();
        }
    }

    void loadSettings()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "SIDIMIDI";
        options.filenameSuffix = "settings";
        options.folderName = "SIDIMIDI";
        options.commonToAllUsers = false;
        appProperties.setStorageParameters(options);

        auto* settings = appProperties.getUserSettings();
        if (settings != nullptr)
        {
            juce::String lastSidDir = settings->getValue("lastSidDir", juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName());
            juce::String lastMidiDir = settings->getValue("lastMidiDir", juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName());

            juce::File sidFolder(lastSidDir);
            juce::File midiFolder(lastMidiDir);

            if (sidFolder.isDirectory()) sidDirectoryList.setDirectory(sidFolder, true, true);
            if (midiFolder.isDirectory()) midiDirectoryList.setDirectory(midiFolder, true, true);
        }
    }

    // Audio Engine
    AudioPlayerEngine audioPlayer;

    // Threads & Filter
    juce::TimeSliceThread thread{ "Directory Thread" };
    juce::WildcardFileFilter sidFilter{ "*", "*", "Alle Ordner/Dateien" };
    juce::WildcardFileFilter midiFilter{ "*", "*", "Alle Ordner/Dateien" };

    juce::DirectoryContentsList sidDirectoryList{ &sidFilter, thread };
    juce::DirectoryContentsList midiDirectoryList{ &midiFilter, thread };

    juce::FileTreeComponent sidTree{ sidDirectoryList };
    juce::FileTreeComponent midiTree{ midiDirectoryList };
    TreeHoverOverlay sidTreeHover;
    TreeHoverOverlay midiTreeHover;

    // "MIDI-SET auswählen" - manuelle Stimmenauswahl, ersetzt midiTree an derselben Stelle
    // (siehe startChannelPicker()/exitChannelPicker()).
    juce::ListBox channelPickerList{ "channelPickerList" };
    std::vector<MidiToSidCoreEngine::ChannelSuggestion> pickerTracks;
    std::vector<std::pair<int, MidiToSidCoreEngine::VoiceRole>> pickerSelectedTracks;
    juce::File pickerMidiFile;
    bool channelPickerActive = false;

    // Subsong Selection
    juce::Label lblSubsong;
    juce::ComboBox cmbSubsongs;

    // Buttons
    juce::TextButton btnSourceDir;
    juce::TextButton btnTargetDir;
    juce::TextButton btnPlaySid;
    juce::TextButton btnPlayMidi;
    juce::TextButton btnStop;
    juce::TextButton btnConvert;
    juce::TextButton btnMidiToSid;

    // Labels & TextEditor
    juce::Image headerImage;
    juce::Label lblSidHeader;
    juce::Label lblMidiHeader;
    juce::TextEditor txtLogInfo;
    juce::Label lblVoice1;
    juce::Label lblVoice2;
    juce::Label lblVoice3;
    juce::Label lblMidiTier;
    VoiceDisplay voiceDisplay1{ audioPlayer, 0 };
    VoiceDisplay voiceDisplay2{ audioPlayer, 1 };
    VoiceDisplay voiceDisplay3{ audioPlayer, 2 };

    // App state
    juce::ApplicationProperties appProperties;
    juce::File selectedSidFile;
    juce::File selectedMidiFile;
    juce::File lastPolledSidTreeFile;
    juce::File lastPolledMidiTreeFile;
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<juce::AlertWindow> renameDialog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};