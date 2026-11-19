// Shared look and feel (Kreisel colours) and small UI helpers.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

namespace sm::ui
{
inline juce::Font font (float h, bool bold = false)
{
    return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font mono (float h, bool bold = false)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), h, bold ? juce::Font::bold : juce::Font::plain));
}

class Look : public juce::LookAndFeel_V4
{
public:
    void setTheme (const Theme& t)
    {
        th = t;
        setColour (juce::ResizableWindow::backgroundColourId, t.bg);
        setColour (juce::Label::textColourId, t.dim);
        setColour (juce::Slider::textBoxTextColourId, t.text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, t.accent.withAlpha (0.4f));
        setColour (juce::Slider::trackColourId, t.accent);
        setColour (juce::ComboBox::backgroundColourId, t.panel2);
        setColour (juce::ComboBox::textColourId, t.text);
        setColour (juce::ComboBox::outlineColourId, t.panel2);
        setColour (juce::ComboBox::arrowColourId, t.dim);
        setColour (juce::PopupMenu::backgroundColourId, t.panel);
        setColour (juce::PopupMenu::textColourId, t.text);
        setColour (juce::PopupMenu::headerTextColourId, t.accent);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, t.accent.withAlpha (0.35f));
        setColour (juce::PopupMenu::highlightedTextColourId, t.text);
        setColour (juce::TextButton::textColourOffId, t.text);
        setColour (juce::TextButton::textColourOnId, t.bg);
        setColour (juce::TextButton::buttonColourId, t.panel2);
        setColour (juce::TextButton::buttonOnColourId, t.accent);
        setColour (juce::ToggleButton::textColourId, t.text);
        setColour (juce::ToggleButton::tickColourId, t.accent);
        setColour (juce::TextEditor::backgroundColourId, t.panel);
        setColour (juce::TextEditor::textColourId, t.text);
        setColour (juce::TextEditor::outlineColourId, t.panel2);
        setColour (juce::TextEditor::focusedOutlineColourId, t.accent);
        setColour (juce::TextEditor::highlightColourId, t.accent.withAlpha (0.35f));
        setColour (juce::ScrollBar::thumbColourId, t.dim.withAlpha (0.5f));
        setColour (juce::AlertWindow::backgroundColourId, t.panel);
        setColour (juce::AlertWindow::textColourId, t.text);
        setColour (juce::CaretComponent::caretColourId, t.accent);
        setColour (juce::ListBox::backgroundColourId, t.bg);
        setColour (juce::ListBox::textColourId, t.text);
        setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, t.accent.withAlpha (0.35f));
        setColour (juce::DirectoryContentsDisplayComponent::textColourId, t.text);
        setColour (juce::FileBrowserComponent::currentPathBoxBackgroundColourId, t.panel2);
        setColour (juce::FileBrowserComponent::currentPathBoxTextColourId, t.text);
        setColour (juce::FileBrowserComponent::filenameBoxBackgroundColourId, t.panel2);
        setColour (juce::FileBrowserComponent::filenameBoxTextColourId, t.text);
        setColour (juce::TooltipWindow::backgroundColourId, t.panel2);
        setColour (juce::TooltipWindow::textColourId, t.text);
        setColour (juce::TooltipWindow::outlineColourId, t.accent);
    }
    const Theme& theme() const { return th; }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto base = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : b.findColour (juce::TextButton::buttonColourId);
        if (over) base = base.brighter (0.12f);
        if (down) base = base.darker (0.15f);
        g.setColour (base);
        g.fillRoundedRectangle (r, 5.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                         .withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.45f));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (3, 1), juce::Justification::centred, 1);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool) override
    {
        auto r = b.getLocalBounds().toFloat();
        auto box = r.removeFromLeft (r.getHeight()).reduced (4.0f);
        g.setColour (b.getToggleState() ? th.accent : th.panel2.brighter (over ? 0.15f : 0.0f));
        g.fillRoundedRectangle (box, 4.0f);
        if (b.getToggleState())
        {
            g.setColour (th.bg);
            juce::Path p;
            p.startNewSubPath (box.getX() + box.getWidth() * 0.22f, box.getCentreY());
            p.lineTo (box.getX() + box.getWidth() * 0.43f, box.getBottom() - box.getHeight() * 0.25f);
            p.lineTo (box.getRight() - box.getWidth() * 0.2f, box.getY() + box.getHeight() * 0.25f);
            g.strokePath (p, juce::PathStrokeType (2.0f));
        }
        g.setColour (th.text.withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
        g.setFont (font (13.0f));
        g.drawFittedText (b.getButtonText(), r.toNearestInt().withTrimmedLeft (4), juce::Justification::centredLeft, 1);
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox&) override
    {
        g.setColour (th.panel2);
        g.fillRoundedRectangle (0, 0, (float) w, (float) h, 5.0f);
        juce::Path p;
        const float cx = (float) w - 12.0f, cy = (float) h * 0.5f;
        p.addTriangle (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
        g.setColour (th.dim);
        g.fillPath (p);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, juce::Slider::SliderStyle, juce::Slider& s) override
    {
        auto b = juce::Rectangle<int> (x, y, w, h).toFloat();
        const float cy = b.getCentreY();
        g.setColour (th.panel2);
        g.fillRoundedRectangle (b.getX(), cy - 3, b.getWidth(), 6, 3);
        g.setColour (s.findColour (juce::Slider::trackColourId));
        g.fillRoundedRectangle (b.getX(), cy - 3, juce::jmax (2.0f, pos - b.getX()), 6, 3);
        g.setColour (th.text);
        g.fillEllipse (pos - 6, cy - 6, 12, 12);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return font (13.0f); }
    juce::Font getTextButtonFont (juce::TextButton&, int h) override { return font (juce::jmin (14.0f, h * 0.55f)); }
    juce::Font getLabelFont (juce::Label&) override { return font (12.5f); }
    juce::Font getPopupMenuFont() override { return font (14.0f); }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (font (12.5f));
        l->setColour (juce::Label::textColourId, th.text);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        return l;
    }

private:
    Theme th = themes()[0];
};

// heart (favourite) inside a rectangle
inline void drawHeart (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    juce::Path heart;
    const float s = r.getHeight() * 0.55f, cx = r.getCentreX(), cy = r.getCentreY();
    heart.startNewSubPath (cx, cy + s * 0.45f);
    heart.cubicTo (cx - s * 0.9f, cy - s * 0.1f, cx - s * 0.45f, cy - s * 0.75f, cx, cy - s * 0.25f);
    heart.cubicTo (cx + s * 0.45f, cy - s * 0.75f, cx + s * 0.9f, cy - s * 0.1f, cx, cy + s * 0.45f);
    g.setColour (c);
    g.fillPath (heart);
}

// background of a list row: every second row is a little darker (C64 blue by default)
inline juce::Colour rowColour (const Theme& t, int row)
{
    return (row & 1) ? t.bg.darker (t.bg.getBrightness() > 0.5f ? 0.05f : 0.17f) : t.bg;
}

// the five rating stars take their colours from the SIDiMIDI logo letters
inline juce::Colour starColour (const Theme& t, int index)
{
    static const int logoColour[5] = { 0, 3, 2, 1, 5 };
    return t.track[(size_t) logoColour[juce::jlimit (0, 4, index)]];
}

inline juce::String hex2 (int v) { return juce::String::toHexString (v & 0xff).paddedLeft ('0', 2).toUpperCase(); }
} // namespace sm::ui
