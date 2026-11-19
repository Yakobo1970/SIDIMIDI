// SIDiMIDI – the main window content: header with tabs, the two tabs, status bar, overlays, loupe.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Archive.h"
#include "Look.h"
#include "Loupe.h"
#include "Lang.h"
#include "UpdateCheck.h"
#include "Panels.h"

class AudioPlayerEngine;

namespace sm
{
class SidTab;
class MidiTab;

class TabButton : public juce::Button
{
public:
    TabButton (const juce::String& n, int colourIndex) : juce::Button (n), colour (colourIndex) { setClickingTogglesState (true); setRadioGroupId (777); }
    void paintButton (juce::Graphics&, bool over, bool down) override;
    int colour;
    juce::String title;
};

class MainComponent : public juce::Component, public juce::DragAndDropContainer, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    // services for the tabs
    Archive& archive() { return *arch; }
    AudioPlayerEngine& player() { return *playerEngine; }
    const Theme& theme() const { return look.theme(); }
    juce::ThreadPool& jobs() { return pool; }
    void setStatus (const juce::String& s);
    void showOverlay (std::unique_ptr<Overlay> o);
    void closeOverlay();
    bool overlayOpen() const { return overlay != nullptr; }
    void showAbout (bool show);
    void applyTheme();
    void applyLanguage();
    void addLoupeMenu (juce::PopupMenu& m, int firstId);   // ids firstId .. firstId+19
    void handleLoupeMenu (int r, int firstId);
    void showLoupeMenu();
    void updateLoupe();
    void showLoupeAt (juce::Point<int> p);
    void selectTab (int index);                             // 0 = C64-SID, 1 = MIDI
    int currentTab() const { return tab; }
    SidTab& sidTab() { return *sidPane; }
    MidiTab& midiTab() { return *midiPane; }
    void chooseFolder (const juce::String& title, std::function<void (const juce::File&)> done);
    void notifySettings() { arch->settingsChanged(); }
    void paintLogo (juce::Graphics&, juce::Rectangle<float>) const;
    void paintTitle (juce::Graphics&, juce::Rectangle<int>, float height) const;

    // scaling: 0 = automatic (follows the window), else a factor 0.5 … 3
    double uiScaleSetting() const { return (double) arch->settings.getProperty ("uiScale", 0.0); }
    void setUiScaleSetting (double s);
    double currentScale() const { return scaleNow; }
    std::function<void()> onScaleSettingChanged;
    void setScaleNow (double s) { scaleNow = s; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    juce::SharedResourcePointer<Archive> arch;
    juce::SharedResourcePointer<juce::TooltipWindow> tooltip;
    juce::SharedResourcePointer<UpdateCheck> updateCheck;
    ui::Look look;
    std::unique_ptr<AudioPlayerEngine> playerEngine;
    juce::ThreadPool pool { 2 };
    std::unique_ptr<SidTab> sidPane;
    std::unique_ptr<MidiTab> midiPane;
    TabButton tabSid { "C64-SID", 0 }, tabMidi { "MIDI → SID", 1 };
    juce::TextButton settingsBtn, aboutBtn { "?" }, updateBtn;
    juce::Image iconImage, bannerImage;
    juce::Rectangle<int> headerArea, statusArea, logoArea, contentArea;
    juce::String statusText, shownUpdate;
    int tab = 0;
    double scaleNow = 1.0;
    std::unique_ptr<Overlay> overlay;
    std::unique_ptr<juce::FileChooser> chooser;

    std::unique_ptr<ui::Loupe> loupe;
    std::unique_ptr<ui::RightClickCatcher> catcher;
    struct LoupeTracker : juce::MouseListener
    {
        explicit LoupeTracker (MainComponent& e) : ed (e) {}
        MainComponent& ed;
        void mouseMove (const juce::MouseEvent&) override { ed.updateLoupe(); }
        void mouseDrag (const juce::MouseEvent&) override { ed.updateLoupe(); }
        void mouseExit (const juce::MouseEvent&) override { ed.updateLoupe(); }
    } loupeTracker { *this };
    juce::Point<int> loupeTestPos { -1, -1 };
    bool inPassThrough = false;

    void refreshUpdateHint();
    void showUpdateMenu();
    void showSettingsMenu();
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace sm
