#pragma once
#include <juce_gui_basics/juce_gui_basics.h>


namespace sm
{
struct Theme
{
    juce::String name;
    juce::Colour bg, panel, panel2, text, dim, accent;
    std::array<juce::Colour, 8> track;
};

inline Theme makeTheme (const char* name, juce::uint32 bg, juce::uint32 panel, juce::uint32 panel2, juce::uint32 text,
                        juce::uint32 dim, juce::uint32 accent, std::initializer_list<juce::uint32> tracks)
{
    Theme t;
    t.name = juce::String::fromUTF8 (name);
    t.bg = juce::Colour (0xff000000 | bg); t.panel = juce::Colour (0xff000000 | panel); t.panel2 = juce::Colour (0xff000000 | panel2);
    t.text = juce::Colour (0xff000000 | text); t.dim = juce::Colour (0xff000000 | dim); t.accent = juce::Colour (0xff000000 | accent);
    int i = 0;
    for (auto c : tracks) t.track[(size_t) i++] = juce::Colour (0xff000000 | c);
    return t;
}

inline const std::array<Theme, 7>& themes()
{
    static const std::array<Theme, 7> list {
        makeTheme ("C64", 0x352879, 0x2a1f6b, 0x4639a3, 0xf0eeff, 0xaaa4ee, 0x8f82ff,
                   { 0xff6b4a, 0x35d6ff, 0xb5f23a, 0xffd43b, 0xf783ac, 0x9775fa, 0x38d9a9, 0xffa94d }),
        makeTheme ("Nacht", 0x0f1117, 0x181b24, 0x232838, 0xe6e8ef, 0x8a90a2, 0x7c9cff,
                   { 0xff5f6d, 0xffa94d, 0xffd43b, 0x69db7c, 0x38d9a9, 0x4dabf7, 0x9775fa, 0xf783ac }),
        makeTheme ("Neon", 0x0b0614, 0x150c24, 0x221438, 0xf5eaff, 0x9a86b8, 0xff2bd6,
                   { 0xff2bd6, 0x00f0ff, 0x39ff14, 0xfff01f, 0xff6b00, 0xb026ff, 0x00ff9c, 0xff3864 }),
        makeTheme ("Ozean", 0x06141c, 0x0c2230, 0x133243, 0xe0f4fa, 0x7fa3b3, 0x2ec4d6,
                   { 0x2ec4d6, 0x4895ef, 0x72efdd, 0x56cfe1, 0x80ffdb, 0x64dfdf, 0x5390d9, 0xa2d2ff }),
        makeTheme ("Abendrot", 0x1a0d12, 0x26131a, 0x351b25, 0xfde8de, 0xb38a8a, 0xff7849,
                   { 0xff7849, 0xffb347, 0xffd166, 0xef476f, 0xf78c6b, 0xe5386d, 0xff9e80, 0xffcad4 }),
        makeTheme ("Wald", 0x0c140e, 0x142019, 0x1d2e23, 0xe4efe1, 0x8aa38e, 0x8bc34a,
                   { 0x8bc34a, 0xcddc39, 0xc8a27a, 0x66bb6a, 0xd4e157, 0x4db6ac, 0xbcaaa4, 0xaed581 }),
        makeTheme ("Mono", 0x111111, 0x1b1b1b, 0x292929, 0xeeeeee, 0x8c8c8c, 0xffffff,
                   { 0xffffff, 0xd6d6d6, 0xb0b0b0, 0x8f8f8f, 0xf0f0f0, 0xc4c4c4, 0xa0a0a0, 0x7d7d7d })
    };
    return list;
}

// Light variant of any theme.
inline Theme lightVariant (const Theme& d)
{
    Theme t = d;
    t.bg = juce::Colour (0xfff2f0eb).interpolatedWith (d.accent, 0.03f);
    t.panel = juce::Colour (0xfffcfbf8);
    t.panel2 = juce::Colour (0xffe6e3dc).interpolatedWith (d.accent, 0.06f);
    t.text = juce::Colour (0xff1d1d22);
    t.dim = juce::Colour (0xff6f6f78);
    t.accent = d.accent.withMultipliedBrightness (0.75f).withMultipliedSaturation (1.1f);
    for (auto& c : t.track)
        c = c.withMultipliedBrightness (0.72f).withMultipliedSaturation (1.15f);
    if (d.name == "Mono")
    {
        t.accent = juce::Colour (0xff222222);
        int i = 0;
        for (auto& c : t.track) c = juce::Colour::greyLevel (0.08f + 0.07f * (float) (i++ % 4));
    }
    return t;
}
} // namespace sm
