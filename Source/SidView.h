// SIDiMIDI – the course of a SID tune: three voices in three colours over the whole length, plus filter and volume.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "SidTrace.h"
#include "Archive.h"
#include "Look.h"
#include "Lang.h"

namespace sm
{
// fixed voice colours (same in every theme; darker variants on a light background)
juce::Colour voiceColour (int voice, bool light = false);

class SidView : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    SidView();
    ~SidView() override;

    std::function<void (juce::uint32 frame)> onSeek;     // user clicked/dragged into the picture
    std::function<void()> onTraceReady;                  // a new trace was computed

    // shows song `song` (1-based) of the entry; computes in the background. nullptr clears the view.
    void show (const Entry* e, int song);
    void showNothing (const juce::String& message);
    std::shared_ptr<const SidTrace> trace() const { return current; }
    const SidInfo& info() const { return sidInfo; }
    int entryId() const { return shownId; }
    int song() const { return shownSong; }
    bool busy() const { return computing; }
    juce::String statusText() const { return status; }

    void setPlayFrame (juce::int64 frame);               // cursor (-1 = none)
    void resetZoom();
    void setVoiceMute (int voice, bool muted) { if (voice >= 0 && voice < 3) { muteLanes[voice] = muted; invalidate(); } }
    bool laneMuted (int voice) const { return voice >= 0 && voice < 3 && muteLanes[voice]; }
    std::function<void (int voice)> onLaneToggle;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::String getTooltip() override;
    void timerCallback() override { if (! computing) stopTimer(); repaint(); }

private:
    struct Geometry { juce::Rectangle<int> plot, ruler, lanes[3], filter; };
    Geometry geometry() const;
    const Theme& theme() const;
    void invalidate() { cache = juce::Image(); repaint(); }
    void renderCache();
    double xToSeconds (int x) const;
    int secondsToX (double s) const;
    void clampView();
    void startCompute (const Entry& e, int song);

    std::shared_ptr<SidTrace> current;
    SidInfo sidInfo;
    juce::ThreadPool pool { 1 };
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    int shownId = 0, shownSong = 0;
    bool computing = false;
    juce::String message, status;
    double viewStart = 0, viewSpan = 0;      // seconds; viewSpan 0 = whole tune
    juce::int64 playFrame = -1;
    juce::int64 lastCursorX = -1;
    juce::Image cache;
    float cacheScale = 1.0f;
    bool muteLanes[3] = { false, false, false };
    int hoverX = -1;
    bool dragging = false;
    juce::uint32 spinner = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidView)
};
} // namespace sm
