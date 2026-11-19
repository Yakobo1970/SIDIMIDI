#include "MidiTab.h"
#include "MainComponent.h"
namespace sm
{
MidiTab::MidiTab (MainComponent& m) : main (m) {}
MidiTab::~MidiTab() {}
void MidiTab::resized() {}
void MidiTab::paint (juce::Graphics& g) { g.fillAll (main.theme().bg); }
}
