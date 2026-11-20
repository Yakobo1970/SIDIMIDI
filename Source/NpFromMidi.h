// SIDiMIDI – MIDI -> NewPlayer: turns a MIDI file into a tune of JCH's NewPlayer editor (see NpTune.h).
// Compared with the plain MIDI -> SID conversion the result has chords as fast arpeggios, drum instruments (noise and
// pitch sweeps through the arpeggio table), pulse swings, hard restart – and it can be edited in the tracker.
#pragma once
#include "NpTune.h"
#include "VoiceMix.h"
#include <array>

namespace sm
{
struct NpImport
{
    bool ok = false;
    NpTune tune;
    double startOffset = 0;    // seconds of the MIDI file before the first note (the tune begins with the first note)
    juce::String report;       // what was done (tempo, voices, chords, sequences …) or why it failed
};

// several MIDI tracks per voice with priorities (VoiceMix.h); a source with part -2 is the automatic choice for its voice
NpImport npImportMidi (const juce::File& midi, const void* templateData, size_t templateSize, VoiceSources sources);

// roles: 0 automatic (GM program / channel), 1 lead, 2 bass, 3 pad, 4 drums
// parts: MIDI tracks for the three voices, as in MidiToSidCoreEngine::ChannelSuggestion::partId(); -2 = automatic
NpImport npImportMidi (const juce::File& midi, const void* templateData, size_t templateSize,
                       std::array<int, 3> parts = { { -2, -2, -2 } }, std::array<int, 3> roles = { { 0, 0, 0 } });
} // namespace sm
