// SIDiMIDI – overlay windows inside the main window: About, folders, track choice, HVSC update.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Look.h"
#include "Lang.h"

namespace sm
{
class MainComponent;

// A dimmed full-window layer with a centred box. Click outside the box closes it (unless modal-only).
class Overlay : public juce::Component
{
public:
    explicit Overlay (MainComponent& m) : main (m) { setWantsKeyboardFocus (true); setInterceptsMouseClicks (true, true); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress&) override;
    void resized() override { layout(); }
    virtual juce::Rectangle<int> box() const;
    void closeLater();

protected:
    MainComponent& main;
    int boxWidth = 640, boxHeight = 420;
    bool closeOnOutside = true;
    virtual void layout() {}
    const Theme& theme() const;
};

class AboutPanel : public Overlay
{
public:
    static constexpr const char* kWebsiteUrl = "https://www.undabtanzbar.de";
    static constexpr const char* kSoftwareUrl = "https://www.undabtanzbar.de/software/sidimidi/";
    static constexpr const char* kMidiFundusUrl = "https://www.undabtanzbar.de/software/midi-fundus/";
    static constexpr const char* kDonateUrl = "https://paypal.me/yakobo";
    static constexpr const char* kGithubUrl = "https://github.com/Yakobo1970/SIDIMIDI";
    explicit AboutPanel (MainComponent& m);
    void paint (juce::Graphics&) override;
    void showPage (int page);

private:
    juce::TextEditor text;
    juce::TextButton tabAbout, tabTips, close, donate, website, github;
    juce::String about, tips;
    int page = 0;
    void layout() override;
};
} // namespace sm
