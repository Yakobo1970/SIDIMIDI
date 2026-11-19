// UiShot – renders the SIDiMIDI window into a PNG (UI checks without clicking).
// Usage: UiShot shot <out.png> [width height] [tab n] [select <row>] [wait <ms>] [about|tips] [light] [theme n] [scale s]
//                              [play <frame>] [loupe x y] [filter <text>] [zoom <x0%> <span%>] [clean]
// Use SIDIMIDI_DB to point at a copy of a database; SIDIMIDI_LANG=de|en|fr for the language.
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Source/ScaleHost.h"
#include "../Source/SidTab.h"
#include "../Source/MidiTab.h"
#include "../Source/AudioPlayerEngine.h"

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
            if (const auto f = arg ("midi"); f.isNotEmpty()) { main.selectTab (1); main.midiTab().openFolder (juce::File (f)); wait (2500); }
            if (const auto f = arg ("targeted"); f.isNotEmpty()) { main.midiTab().openTrackPanelForTest (f.getIntValue()); wait (300); }
            wait (400);
            if (const auto s = arg ("select"); s.isNotEmpty())
            {
                main.sidTab().selectRowForTest (s.getIntValue());
                wait (args.contains ("wait") ? arg ("wait").getIntValue() : 1200);
            }
            else wait (args.contains ("wait") ? arg ("wait").getIntValue() : 300);
            if (const auto d = arg ("dialog"); d.isNotEmpty())
            {
                // opens the real dialog of the app (folder / save) and photographs the window
                if (d == "folder") main.chooseFolder ("Test", [] (const juce::File&) {}, arg ("pattern").isNotEmpty() ? arg ("pattern") : juce::String ("*.sid"));
                else main.chooseSaveFile ("Test", juce::File (arg ("dir")).getChildFile ("Beispiel.mid"), "*.mid", [] (const juce::File&) {});
                wait (1500);
                for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
                    if (auto* c = juce::Desktop::getInstance().getComponent (i); c != nullptr && c != &host && c->isShowing() && c->getWidth() > 200)
                        writePng (c->createComponentSnapshot (c->getLocalBounds(), true, 1.0f), juce::File (out.getFullPathName().replace (".png", "_" + juce::String (i) + ".png")));
                quit();
                return;
            }
            if (const auto d = arg ("folderdialog"); d.isNotEmpty())
            {
                // the folder dialog of chooseFolder() on its own: it lists the files of the folder too
                juce::WildcardFileFilter filter (arg ("pattern", 1).isNotEmpty() ? arg ("pattern") : juce::String ("*.sid"), "*", "files");
                juce::FileBrowserComponent browser (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectFiles,
                                                    juce::File (d), &filter, nullptr);
                browser.setLookAndFeel (&host.getLookAndFeel());
                browser.setSize (760, 520);
                wait (1500);
                writePng (browser.createComponentSnapshot (browser.getLocalBounds(), true, 1.0f), out);
                quit();
                return;
            }
            if (args.contains ("prime")) { host.createComponentSnapshot (host.getLocalBounds(), true, 1.0f); wait (arg ("prime").getIntValue()); }   // paints once so lazy content (miniatures) starts loading
            if (args.contains ("tracker"))   // the tracker window: tracker <tune file or -> <row> <voice> [play]
            {
                main.openTracker();
                if (auto* tw = main.trackerWindow())
                {
                    const auto f = arg ("tracker"), row = arg ("tracker", 2), voice = arg ("tracker", 3);
                    tw->setupForTest (f == "-" ? juce::File() : juce::File (f), row.getIntValue(), voice.getIntValue(), arg ("tracker", 4) == "play", arg ("keys"), juce::File (arg ("export")));
                    wait (arg ("tracker", 4) == "play" ? 1500 : 400);
                    writePng (tw->createComponentSnapshot (tw->getLocalBounds(), true, 1.0f), out);
                    main.setStatus ({});
                }
                quit();
                return;
            }
            if (const auto r = arg ("edit"); r.isNotEmpty())   // opens the cell editor: edit <row> <column id> [typed text]
            {
                main.sidTab().table().list().grabKeyboardFocus();
                main.sidTab().table().startEdit (r.getIntValue(), arg ("edit", 2).getIntValue(), arg ("edit", 3).startsWith ("-") ? juce::String() : arg ("edit", 3));
                wait (400);
            }
            if (const auto m = arg ("sound"); m.isNotEmpty())   // really plays the selected tune very quietly (oscillators in the header)
            {
                main.player().setOutputGain (0.03f);
                main.sidTab().play (main.sidTab().selected(), 0);
                wait (m.getIntValue());
            }
            if (const auto p = arg ("play"); p.isNotEmpty()) main.sidTab().view().setPlayFrame (p.getIntValue());
            if (args.contains ("about")) main.showAbout (true);
            if (args.contains ("tips")) { main.showAbout (true); }
            if (args.contains ("hvscpanel")) { main.showOverlay (std::make_unique<HvscPanel> (main)); wait (args.contains ("netwait") ? arg ("netwait").getIntValue() : 300); }
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
