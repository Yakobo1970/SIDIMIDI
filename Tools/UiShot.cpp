// UiShot – renders the SIDiMIDI window into a PNG (UI checks without clicking).
// Usage: UiShot shot <out.png> [width height] [tab n] [select <row>] [wait <ms>] [about|tips] [light] [theme n] [scale s]
//                              [play <frame>] [loupe x y] [filter <text>] [zoom <x0%> <span%>] [clean]
// Use SIDIMIDI_DB to point at a copy of a database; SIDIMIDI_LANG=de|en|fr for the language.
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Source/ScaleHost.h"
#include "../Source/SidTab.h"
#include "../Source/MidiTab.h"

using namespace sm;

class UiShotApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "UiShot"; }
    const juce::String getApplicationVersion() override { return "1"; }

    static void writePng (const juce::Image& img, const juce::File& out)
    {
        out.deleteFile();
        juce::FileOutputStream os (out);
        juce::PNGImageFormat().writeImageToStream (img, os);
    }

    static void wait (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

    void initialise (const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();
        if (args.size() >= 2 && args[0] == "shot")
        {
            const juce::File out (args[1]);
            int w = 1440, h = 880;
            if (args.size() >= 4 && args[2].containsOnly ("0123456789") && args[3].containsOnly ("0123456789")) { w = args[2].getIntValue(); h = args[3].getIntValue(); }
            auto arg = [&] (const char* name, int offset = 1) { const int i = args.indexOf (name); return i > 0 && i + offset < args.size() ? args[i + offset] : juce::String(); };

            ScaleHost host;
            host.setSize (w, h);
            auto& main = host.main();
            auto& st = main.archive().settings;
            if (args.contains ("light")) st.setProperty ("light", true, nullptr);
            if (const auto t = arg ("theme"); t.isNotEmpty()) st.setProperty ("theme", t.getIntValue(), nullptr);
            if (const auto s = arg ("scale"); s.isNotEmpty()) st.setProperty ("uiScale", s.getDoubleValue(), nullptr);
            main.applyTheme();
            host.resized();
            if (const auto t = arg ("tab"); t.isNotEmpty()) main.selectTab (t.getIntValue());
            if (const auto f = arg ("filter"); f.isNotEmpty()) main.sidTab().table().setGlobalSearch (f);
            wait (400);
            if (const auto s = arg ("select"); s.isNotEmpty())
            {
                main.sidTab().selectRowForTest (s.getIntValue());
                wait (args.contains ("wait") ? arg ("wait").getIntValue() : 1200);
            }
            else wait (args.contains ("wait") ? arg ("wait").getIntValue() : 300);
            if (const auto p = arg ("play"); p.isNotEmpty()) main.sidTab().view().setPlayFrame (p.getIntValue());
            if (args.contains ("about")) main.showAbout (true);
            if (args.contains ("tips")) { main.showAbout (true); }
            if (const int i = args.indexOf ("loupe"); i > 0) main.showLoupeAt ({ args[i + 1].getIntValue(), args[i + 2].getIntValue() });
            wait (200);
            if (! args.contains ("clean")) main.setStatus ("UiShot");
            host.resized();
            writePng (host.createComponentSnapshot (host.getLocalBounds(), true, 1.0f), out);
        }
        quit();
    }
    void shutdown() override {}
};

START_JUCE_APPLICATION (UiShotApp)
