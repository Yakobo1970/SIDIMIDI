// SIDiMIDI – the instrument pool of the tracker: instruments (the 8 bytes with the arpeggio, pulse and filter programs they point at)
// that can be saved under a name and loaded into any instrument slot of any tune (player v18 / v20 tables).
#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <vector>
#include "NpTune.h"

namespace sm
{
struct InstrumentPreset
{
    juce::String name, category;
    bool factory = false;
    std::array<juce::uint8, 8> ins {};
    using Step = std::array<juce::uint8, 2>;
    using Set = std::array<juce::uint8, 4>;
    struct ArpProg { std::vector<Step> rows; int start = 0; };        // start = row in the tune the program was taken from (wrap targets refer to it)
    ArpProg arpOn, arpOff;                                            // the programs of "Arp +" and "Arp -"
    bool arpShared = true;                                            // both pointers point at one program
    std::vector<int> pulseRows, filterRows;                           // rows the sets were taken from (the "next" bytes are offsets = row * 4)
    std::vector<Set> pulseSets, filterSets;
};

namespace InstrumentPool
{
const char* const kCategories[] = { "Leads", "Bässe", "Arpeggios", "Pads", "Kicks", "Snares", "Hi-Hats", "Toms & Becken", "Effekte", "Eigene" };
constexpr int kNumCategories = 10;

bool supported (const NpTune& t);                                     // player variants whose tables have the layout the presets use
std::vector<InstrumentPreset> factoryPresets();
std::vector<InstrumentPreset> userPresets();
bool saveUserPresets (const std::vector<InstrumentPreset>& list);
InstrumentPreset capture (const NpTune& t, int instrument, const juce::String& name, const juce::String& category);
bool apply (NpTune& t, int instrument, const InstrumentPreset& p, juce::String& error);   // false: no room in a table (nothing is changed)
void clear (NpTune& t, int instrument);
}
} // namespace sm
