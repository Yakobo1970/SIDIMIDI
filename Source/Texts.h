// SIDiMIDI – long texts (About, tips, MIDI -> SID description); language 0 = de, 1 = en, 2 = fr. nullptr if the key is unknown.
#pragma once
#include <juce_core/juce_core.h>

namespace sm
{
const char* longText (const char* key, int language);
}
