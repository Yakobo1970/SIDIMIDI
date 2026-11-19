// SIDiMIDI – PPMd variant H (PPMd7) decoder with the 7z range coder, as used by 7z method 03 04 01.
// Own implementation of the published algorithm (Dmitry Shkarin's PPMd var.H, 7-Zip's 7z flavour).
#pragma once
#include <juce_core/juce_core.h>
#include <functional>

namespace sm
{
// props: order (1 byte) + memory size (UInt32 LE). Calls sink with chunks of decoded data; returns false (and error) on damaged data.
bool ppmd7Decode (const juce::uint8* in, size_t inSize, juce::uint8 order, juce::uint32 memSize, juce::uint64 outSize,
                  const std::function<bool (const juce::uint8*, size_t)>& sink, juce::String& error);
} // namespace sm
