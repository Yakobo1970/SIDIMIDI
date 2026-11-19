#include "Lang.h"
#include "Texts.h"
#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>

namespace sm
{
static std::atomic<int> gLang { (int) Lang::de };

Lang language() { return (Lang) gLang.load(); }
void setLanguage (Lang l) { gLang = (int) l; }

juce::String langCode (Lang l) { return l == Lang::en ? "en" : l == Lang::fr ? "fr" : "de"; }

bool langFromCode (const juce::String& code, Lang& l)
{
    const auto c = code.trim().toLowerCase();
    if (c.startsWith ("de") || c == "german") { l = Lang::de; return true; }
    if (c.startsWith ("en")) { l = Lang::en; return true; }
    if (c.startsWith ("fr")) { l = Lang::fr; return true; }
    return false;
}

void initLanguage (juce::ValueTree& settings)
{
    Lang l = Lang::de;
    if (langFromCode (juce::SystemStats::getEnvironmentVariable ("SIDIMIDI_LANG", {}), l)) { setLanguage (l); return; }
    juce::String reg;
   #if JUCE_WINDOWS
    reg = juce::WindowsRegistry::getValue ("HKEY_LOCAL_MACHINE\\Software\\Yakobo\\SIDIMIDI\\Language");
    if (reg.isEmpty()) reg = juce::WindowsRegistry::getValue ("HKEY_CURRENT_USER\\Software\\Yakobo\\SIDIMIDI\\Language");
   #endif
    // a (new) choice in the setup wins once over the stored one
    if (reg.isNotEmpty() && reg != settings.getProperty ("langSetup").toString() && langFromCode (reg, l))
    {
        settings.setProperty ("lang", langCode (l), nullptr);
        settings.setProperty ("langSetup", reg, nullptr);
    }
    if (! langFromCode (settings.getProperty ("lang").toString(), l))
        if (! langFromCode (juce::SystemStats::getUserLanguage(), l)) l = Lang::en;
    setLanguage (l);
}

void chooseLanguage (juce::ValueTree& settings, Lang l)
{
    settings.setProperty ("lang", langCode (l), nullptr);
    setLanguage (l);
}

namespace
{
struct Row { const char* de; const char* en; const char* fr; };

// ---- table begin
const Row kRows[] = {
    { "Ordner", "Folders", "Dossiers" },
    // ---- table end
};

const Row* find (const char* de)
{
    static const auto map = [] {
        std::unordered_map<std::string, const Row*> m;
        for (auto& r : kRows) m[r.de] = &r;
        return m;
    }();
    auto it = map.find (de);
    return it == map.end() ? nullptr : it->second;
}

juce::String lookup (const char* de)
{
    const auto l = language();
    if (de[0] == '@')
        if (auto* text = longText (de, (int) l))
            return juce::String::fromUTF8 (text);
    if (l != Lang::de)
        if (auto* r = find (de))
            return juce::String::fromUTF8 (l == Lang::en ? r->en : r->fr);
    return juce::String::fromUTF8 (de);
}
} // namespace

juce::String T (const char* germanUtf8) { return lookup (germanUtf8); }
juce::String TS (const juce::String& german) { return lookup (german.toRawUTF8()); }
} // namespace sm
