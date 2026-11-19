// SIDiMIDI – the scale host: holds the main component and scales the whole user interface (stepless).
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "MainComponent.h"

namespace sm
{
// Holds the main component and scales it: the content is laid out in "logical" pixels, the window shows it enlarged or reduced.
class ScaleHost : public juce::Component
{
public:
    ScaleHost()
    {
        content = std::make_unique<MainComponent>();
        addAndMakeVisible (*content);
        content->onScaleSettingChanged = [this] { resized(); };
        setWantsKeyboardFocus (false);
    }

    MainComponent& main() { return *content; }

    void resized() override
    {
        const double s = computeScale();
        content->setScaleNow (s);
        content->setTransform (juce::AffineTransform::scale ((float) s));
        content->setBounds (0, 0, juce::roundToInt ((double) getWidth() / s), juce::roundToInt ((double) getHeight() / s));
    }

    double computeScale() const
    {
        const double setting = content->uiScaleSetting();
        if (setting > 0.0) return juce::jlimit (0.5, 3.0, setting);
        // automatic: the user interface grows and shrinks with the window (reference: 1280 x 780)
        const double sw = (double) getWidth() / 1280.0, sh = (double) getHeight() / 780.0;
        return juce::jlimit (0.6, 3.0, std::min (sw, sh));
    }

private:
    std::unique_ptr<MainComponent> content;
};
} // namespace sm
