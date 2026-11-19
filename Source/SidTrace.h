// SIDiMIDI – runs a SID tune in the 6502 emulation and turns the register writes into a per-frame timeline
// (pitch, waveform, envelope level of the three voices, filter) for the display.
#pragma once
#include <juce_core/juce_core.h>
#include <memory>
#include <functional>
#include <vector>
#include "SidInfo.h"
#include "SidCpu6502Engine.h"

namespace sm
{
enum Wave : juce::uint8 { wTriangle = 1, wSaw = 2, wPulse = 4, wNoise = 8 };

struct SidTrace
{
    int song = 1;
    double fps = 50.1245;                 // frames (play routine calls) per second
    juce::uint32 frames = 0;              // length of the timeline
    bool timedOut = false, noPlay = false, cancelled = false;
    juce::String note;                    // short remark for the user ("keine Play-Routine" ...)
    std::shared_ptr<std::vector<SidCpU6502Engine::RegEvent>> events;

    struct Voice
    {
        std::vector<float> note;          // continuous MIDI note, < 0 = silent
        std::vector<juce::uint8> wave;    // control register high nibble
        std::vector<juce::uint8> level;   // envelope level 0 … 255 (modelled)
        std::vector<juce::uint16> pulse;  // 12 bit pulse width
        std::vector<juce::uint8> mods;    // bit0 ring, bit1 sync, bit2 gate
    };
    Voice v[3];
    std::vector<juce::uint16> cutoff;     // 11 bit filter cutoff
    std::vector<juce::uint8> filterMode;  // bit0 low-pass, bit1 band-pass, bit2 high-pass
    std::vector<juce::uint8> routing;     // bit0..2 voices through the filter
    std::vector<juce::uint8> resonance, volume;
    float minNote = 36, maxNote = 96;     // used range of all voices

    double seconds() const { return fps > 0 ? (double) frames / fps : 0.0; }
    double frameSeconds (juce::uint32 f) const { return fps > 0 ? (double) f / fps : 0.0; }
    juce::uint32 frameAt (double seconds) const { return (juce::uint32) std::max (0.0, seconds * fps); }
    bool empty() const { return frames == 0 || ! events || events->empty(); }
};

// wantSeconds: how much of the tune to emulate (the HVSC length + a little, or a default if unknown).
// cancel is polled by the emulation; run it from a worker thread.
std::shared_ptr<SidTrace> computeTrace (const SidInfo& si, int song, double wantSeconds, const std::function<bool()>& cancel = {},
                                        double maxEmulationSeconds = 25.0);

// builds the timeline from register events (also used for traces from other sources)
void buildTimeline (SidTrace& t, const SidInfo& si);
} // namespace sm
