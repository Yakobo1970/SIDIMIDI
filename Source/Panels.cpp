#include "Panels.h"
#include "MainComponent.h"

namespace sm
{
using ui::font;

const Theme& Overlay::theme() const { return main.theme(); }

juce::Rectangle<int> Overlay::box() const
{
    return getLocalBounds().withSizeKeepingCentre (juce::jmin (boxWidth, getWidth() - 40), juce::jmin (boxHeight, getHeight() - 40));
}

void Overlay::paint (juce::Graphics& g)
{
    const auto& th = theme();
    g.fillAll (th.bg.withAlpha (0.78f));
    auto b = box().toFloat();
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (b.translated (3.0f, 5.0f), 12.0f);
    g.setColour (th.panel);
    g.fillRoundedRectangle (b, 12.0f);
    g.setColour (th.accent);
    g.drawRoundedRectangle (b, 12.0f, 1.5f);
}

void Overlay::mouseDown (const juce::MouseEvent& e)
{
    if (closeOnOutside && ! box().contains (e.getPosition())) closeLater();
}

bool Overlay::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { closeLater(); return true; }
    return false;
}

void Overlay::closeLater()
{
    juce::Component::SafePointer<MainComponent> safe (&main);
    juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->closeOverlay(); });
}

// ================================================================================================ About
AboutPanel::AboutPanel (MainComponent& m) : Overlay (m)
{
    boxWidth = 900; boxHeight = 740;
    tabAbout.setButtonText (T ("Über SIDiMIDI"));
    tabTips.setButtonText (T ("Tooltipps"));
    close.setButtonText (T ("Schließen"));
    donate.setButtonText (T ("Spenden (PayPal)"));
    website.setButtonText ("undabtanzbar.de");
    github.setButtonText ("GitHub");
    text.setMultiLine (true);
    text.setReadOnly (true);
    text.setScrollbarsShown (true);
    text.setCaretVisible (false);
    text.setFont (font (14.0f));
    text.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    text.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    about = T ("@about");
    tips = T ("@tips");
    for (auto* b : { &tabAbout, &tabTips })
    {
        b->setRadioGroupId (404);
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }
    tabAbout.onClick = [this] { showPage (0); };
    tabTips.onClick = [this] { showPage (1); };
    donate.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffd6336c));
    donate.setTooltip (kDonateUrl);
    website.setTooltip (kSoftwareUrl);
    github.setTooltip (kGithubUrl);
    donate.onClick = [] { juce::URL (kDonateUrl).launchInDefaultBrowser(); };
    website.onClick = [] { juce::URL (kSoftwareUrl).launchInDefaultBrowser(); };
    github.onClick = [] { juce::URL (kGithubUrl).launchInDefaultBrowser(); };
    close.onClick = [this] { closeLater(); };
    for (auto* c : std::initializer_list<juce::Component*> { &donate, &website, &github, &close, &text }) addAndMakeVisible (c);
    showPage (0);
}

void AboutPanel::showPage (int p)
{
    page = p;
    (p == 0 ? tabAbout : tabTips).setToggleState (true, juce::dontSendNotification);
    text.setText (p == 0 ? about : tips, false);
    text.moveCaretToTop (false);
}

void AboutPanel::paint (juce::Graphics& g)
{
    Overlay::paint (g);
    const auto& th = theme();
    auto head = box().reduced (18).removeFromTop (72);
    head.removeFromLeft (4);
    main.paintLogo (g, head.removeFromLeft (72).toFloat());
    head.removeFromLeft (14);
    main.paintTitle (g, head.removeFromTop (40), 30.0f);
    g.setColour (th.dim);
    g.setFont (font (14.0f));
    g.drawText (T ("Version %1  ·  SID ↔ MIDI und C64-Musikarchiv  ·  von Un(d)AbtanzBAR", SM_VERSION_STRING), head, juce::Justification::topLeft);
}

void AboutPanel::layout()
{
    auto b = box().reduced (18);
    b.removeFromTop (82);
    auto tabs = b.removeFromTop (30);
    tabAbout.setBounds (tabs.removeFromLeft (180));
    tabs.removeFromLeft (6);
    tabTips.setBounds (tabs.removeFromLeft (130));
    b.removeFromTop (8);
    auto bottom = b.removeFromBottom (32);
    close.setBounds (bottom.removeFromRight (110));
    donate.setBounds (bottom.removeFromLeft (190));
    bottom.removeFromLeft (8);
    website.setBounds (bottom.removeFromLeft (150));
    bottom.removeFromLeft (8);
    github.setBounds (bottom.removeFromLeft (100));
    b.removeFromBottom (10);
    text.setBounds (b);
}
} // namespace sm
