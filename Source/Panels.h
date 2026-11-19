// SIDiMIDI – overlay windows inside the main window: About, folders, track choice, HVSC update.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Look.h"
#include "Lang.h"
#include "Hvsc.h"
#include "HvscUpdate.h"
#include <atomic>
#include <memory>

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

// HVSC: installed version, online check, download + installation of the update packages (with backup)
class HvscPanel : public Overlay, private juce::Thread, private juce::Timer
{
public:
    static constexpr const char* kDownloadsUrl = "https://hvsc.c64.org/downloads";
    explicit HvscPanel (MainComponent& m);
    ~HvscPanel() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    enum class Task { none, check, install };
    juce::ComboBox collections;
    juce::TextEditor log;
    juce::TextButton checkBtn, installBtn, fullBtn, closeBtn, addBtn;
    double progressValue = 0.0;
    juce::ProgressBar bar { progressValue };
    std::vector<std::shared_ptr<Hvsc>> list;
    juce::String infoLines;

    // worker side (the thread only touches these under `lock`)
    juce::CriticalSection lock;
    juce::StringArray queuedLog;
    juce::String stage;
    double fraction = 0.0;
    Task task = Task::none;
    std::atomic<bool> finished { false };
    std::shared_ptr<Hvsc> target;
    // results of the check
    std::vector<int> missing;
    std::vector<juce::int64> missingSizes;
    bool offline = false;
    bool installOk = false;
    int installedVersion = 0;
    juce::File lastBackup;

    bool busy() const { return task != Task::none; }
    std::shared_ptr<Hvsc> selected() const;
    void layout() override;
    void refreshInfo();
    void updateButtons();
    void startTask (Task t);
    void askInstall();
    void say (const juce::String& line);
    void run() override;
    void doCheck();
    void doInstall();
    void timerCallback() override;
    void taskFinished();
};
} // namespace sm
