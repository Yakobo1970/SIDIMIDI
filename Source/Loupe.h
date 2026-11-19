// Mouse loupe (from x0x): a floating window inside the editor that shows the area around
// the mouse pointer magnified. Switched on and off with the right mouse button (and in the cell menu).
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace sm::ui
{
struct LoupeSettings
{
    static constexpr int kZooms[] = { 200, 250, 300, 400 };
    static juce::Point<int> windowSize (int size)
    {
        static constexpr int w[] = { 260, 360, 480 };
        return { w[juce::jlimit (0, 2, size)], w[juce::jlimit (0, 2, size)] * 5 / 8 };
    }
};

// Shows a magnified snapshot of `source` around a point. Lets every mouse event through.
class Loupe : public juce::Component
{
public:
    Loupe() { setInterceptsMouseClicks (false, false); }

    juce::Colour frame { 0xff7c9cff };

    void setImage (juce::Image img) { image = std::move (img); brighten (image, 1.12f); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (r.translated (3.0f, 4.0f), 7.0f);
        {
            juce::Graphics::ScopedSaveState state (g);
            juce::Path clip;
            clip.addRoundedRectangle (r.reduced (2.0f), 6.0f);
            g.reduceClipRegion (clip);
            g.fillAll (juce::Colour (0xff09090a));
            g.setOpacity (1.0f);
            if (image.isValid())
                g.drawImage (image, r, juce::RectanglePlacement::stretchToFit);
            const auto c = r.getCentre();
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.drawLine (c.x - 9, c.y, c.x - 3, c.y, 1.5f);
            g.drawLine (c.x + 3, c.y, c.x + 9, c.y, 1.5f);
            g.drawLine (c.x, c.y - 9, c.x, c.y - 3, 1.5f);
            g.drawLine (c.x, c.y + 3, c.x, c.y + 9, 1.5f);
        }
        g.setColour (frame);
        g.drawRoundedRectangle (r.reduced (1.5f), 6.0f, 2.5f);
    }

private:
    static void brighten (juce::Image& img, float gain)
    {
        if (! img.isValid()) return;
        img = img.convertedToFormat (juce::Image::ARGB);
        const juce::Image::BitmapData bd (img, juce::Image::BitmapData::readWrite);
        for (int y = 0; y < bd.height; ++y)
        {
            auto* p = reinterpret_cast<juce::PixelARGB*> (bd.getLinePointer (y));
            for (int x = 0; x < bd.width; ++x, ++p)
            {
                const auto boost = [gain] (juce::uint8 v) { return (juce::uint8) std::min (255, (int) ((float) v * gain + 5.0f)); };
                p->setARGB (p->getAlpha(), boost (p->getRed()), boost (p->getGreen()), boost (p->getBlue()));
            }
        }
    }

    juce::Image image;
};

// Transparent layer over the whole editor that only catches right clicks, so controls underneath
// never react to them. Components with their own right-click function are let through.
class RightClickCatcher : public juce::Component
{
public:
    std::function<void()> onRightClick;
    std::function<bool (juce::Point<int>)> passThrough;

    bool hitTest (int x, int y) override
    {
        // the modifiers of the mouse event being dispatched (set by the window before the hit test);
        // the "realtime" query can still report the button as up at this moment
        const bool right = juce::ModifierKeys::currentModifiers.isRightButtonDown();
        return right && ! (passThrough && passThrough ({ x, y }));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isRightButtonDown() && onRightClick) onRightClick();
    }
};
} // namespace sm::ui
