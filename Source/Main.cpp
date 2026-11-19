// SIDiMIDI – application, main window and the scale host (stepless scaling of the whole user interface).
#include <juce_gui_basics/juce_gui_basics.h>
#include "ScaleHost.h"

using namespace sm;

namespace
{
class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (const juce::String& name)
        : DocumentWindow (name, juce::Colour (0xff131033), DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        host = new ScaleHost();
        setContentOwned (host, false);
        auto& st = host->main().archive().settings;
        setResizable (true, true);
        setResizeLimits (640, 400, 7680, 4320);
        const int w = st.getProperty ("w", 1440), h = st.getProperty ("h", 880);
        const int x = st.getProperty ("x", -1), y = st.getProperty ("y", -1);
        centreWithSize (juce::jlimit (640, 7680, w), juce::jlimit (400, 4320, h));
        if (x >= 0 && y >= 0)
        {
            const auto area = juce::Desktop::getInstance().getDisplays().getTotalBounds (true);
            const juce::Rectangle<int> r (x, y, getWidth(), getHeight());
            if (area.intersects (r)) setTopLeftPosition (x, y);
        }
        if ((bool) st.getProperty ("max", false)) setFullScreen (true);
        setVisible (true);
    }

    ~MainWindow() override { saveBounds(); }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    void saveBounds()
    {
        if (host == nullptr) return;
        auto& st = host->main().archive().settings;
        st.setProperty ("max", isFullScreen(), nullptr);
        if (! isFullScreen())
        {
            st.setProperty ("w", getWidth(), nullptr);
            st.setProperty ("h", getHeight(), nullptr);
            st.setProperty ("x", getX(), nullptr);
            st.setProperty ("y", getY(), nullptr);
        }
        host->main().archive().settingsChanged();
    }

private:
    ScaleHost* host = nullptr;
};
} // namespace

class SidimidiApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "SIDiMIDI"; }
    const juce::String getApplicationVersion() override { return SM_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override { window = std::make_unique<MainWindow> ("SIDiMIDI"); }
    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }

private:
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (SidimidiApplication)
