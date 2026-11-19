// SIDiMIDI – user interface languages (Deutsch, English, Français).
// Texts are written in German in the code; T() looks up the translation. Placeholders %1 … %9.
#pragma once
#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace sm
{
enum class Lang { de = 0, en = 1, fr = 2 };

Lang language();
void setLanguage (Lang);
juce::String langCode (Lang);                        // "de", "en", "fr"
bool langFromCode (const juce::String& code, Lang&); // also "german", "english", "french" (Inno Setup)
void initLanguage (juce::ValueTree& settings);       // test override, setup choice (registry), stored choice, Windows language
void chooseLanguage (juce::ValueTree& settings, Lang);

juce::String T (const char* germanUtf8);
juce::String TS (const juce::String& german);        // runtime strings (stored remarks ...)

template <typename... Args>
juce::String T (const char* germanUtf8, const Args&... args)
{
    auto s = T (germanUtf8);
    int i = 1;
    ((s = s.replace ("%" + juce::String (i++), juce::String (args))), ...);
    return s;
}

// shorthand for literals with umlauts: always UTF-8
inline juce::String U (const char* utf8) { return juce::String::fromUTF8 (utf8); }
} // namespace sm
