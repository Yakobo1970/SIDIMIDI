#pragma once
#include "VoiceMix.h"

#include <JuceHeader.h>
#include "EmbeddedMiniplayer.h"
#include <vector>
#include <map>
#include <set>
#include <array>
#include <algorithm>
#include <cmath>

//==============================================================================
/**
    MIDI -> SID Konvertierung: Gegenrichtung zu SidToMidiCoreEngine.

    Erzeugt aus einer beliebigen MIDI-Datei eine spielbare .sid-Datei, indem die
    Playroutine "miniplayer2" von cadaver (github.com/cadaver/miniplayer2, MIT-
    lizenziert, siehe ThirdParty/miniplayer/LICENSE.txt) eingebettet wird. Statt das
    Binärformat von Hand nachzubauen, erzeugt dieser Encoder DASM-Assembler-Quelltext
    (dieselbe Tabellen-Struktur, die auch der Referenzkonverter gt2mini2.c erzeugt,
    siehe ThirdParty/miniplayer/FORMAT_NOTES.md für die vollständige Format-
    Dokumentation) und ruft das mitgelieferte tools/dasm.exe als externen Prozess auf,
    um daraus die fertige .sid-Datei zu assemblieren - DASM übernimmt dabei die
    komplette Adress-/Zeiger-Arithmetik über Labels, was Low-Level-Bugs vermeidet, die
    beim manuellen Schreiben roher Binärdaten leicht entstehen.

    v1-Einschränkungen (bewusst, um den Umfang überschaubar zu halten):
    - Kein Pulsweiten-/Filter-Modulation (insPulsePos/insFiltPos immer 0).
    - Kein Vibrato/Slide/Wavetable-Arpeggio (immer ein einzelner Wave-Schritt je Stimme).
    - Genau EIN Instrument pro Stimme (nicht pro Note) - gewählt aus dem MIDI-Programm
      der Stimme.
    - Noten länger als 128 Frames (~2.56s bei 50Hz) werden in mehrere aufeinander-
      folgende Notenereignisse derselben Tonhöhe zerlegt (siehe splitDurationFrames()) -
      dabei entsteht alle ~2.56s ein kurzes Neu-Antriggern der Hüllkurve.
*/
class MidiToSidCoreEngine
{
public:
    //==============================================================================
    // Vom Nutzer im "MIDI-SET"-Picker manuell zugewiesene Stimmenrolle (siehe MainComponent::
    // toggleChannelSelection()). Auto = bisherige Heuristik (GM-Programm + Kanal-10-Erkennung
    // in pickInstrument()) unverändert beibehalten.
    enum class VoiceRole { Auto, Lead, Bass, Pad, Drums };

    //==============================================================================
    struct ChannelSuggestion
    {
        int trackIndex = -1;   // Index des MTrk-Chunks in der Datei (0-basiert), -1 = ungültig
        int channel = -1;      // vorherrschender MIDI-Kanal 1-16 im Track (nur für Drum-Kanal-
                                // Erkennung in pickInstrument() - Auswahl selbst läuft über trackIndex)
        int noteCount = 0;
        double avgPitch = 0.0;
        juce::String label;    // Anzeigetext für die UI-ComboBox/Liste
        // 0 = die ganze Spur; 1-16 = nur dieser MIDI-Kanal der Spur. MIDI-Dateien vom Typ 0 (alles in
        // einer Spur) und manche Exporte legen mehrere Stimmen auf verschiedene Kanäle derselben Spur -
        // diese lassen sich nur mit Kanalfilter als getrennte SID-Stimmen wählen.
        int channelFilter = 0;
        bool drums = false;    // Schlagzeug: Kanal 10, der Spurname (Drums, Kick, Snare ...) oder das Notenbild (GM-Drum-Noten, kurz, wiederholt)
        // Kennung für convertMidiToSidFile(): Spurindex, bei Kanalfilter zusätzlich der Kanal in Bit 16-20
        int partId() const { return trackIndex | (channelFilter << 16); }
    };

    // Liest die MIDI-Datei, erkennt den SIDM-Rundweg-Sonderfall (siehe SidToMidiCore.h::
    // kMidiTierChannelBase) und schlägt bei "normalen" Dateien die 3 vielversprechendsten
    // Spuren vor (nach Notendichte, sortiert nach Tonhöhenlage: tiefste Stimme -> Voice 1).
    static bool analyzeMidiFile(const juce::File& midiFile,
        bool& outIsRoundTrip,
        std::array<ChannelSuggestion, 3>& outSuggested,
        juce::String& log)
    {
        outIsRoundTrip = false;
        for (auto& s : outSuggested) s = ChannelSuggestion{};

        juce::MidiFile parsedFile;
        if (!loadMidiFile(midiFile, parsedFile, log))
            return false;

        outIsRoundTrip = detectSidmSignature(parsedFile);
        if (outIsRoundTrip)
        {
            log += "SIDM-Signatur erkannt: verwende Spur 1-3 (bereits SID-gerecht vereinfacht) direkt.\n";
            outSuggested[0] = ChannelSuggestion{ 1, 1, 0, 0.0, "Spur 1 (vereinfacht)" };
            outSuggested[1] = ChannelSuggestion{ 2, 2, 0, 0.0, "Spur 2 (vereinfacht)" };
            outSuggested[2] = ChannelSuggestion{ 3, 3, 0, 0.0, "Spur 3 (vereinfacht)" };
            return true;
        }

        std::vector<ChannelSuggestion> ranked;
        if (!gatherTrackStats(midiFile, ranked, log))
            return false;
        if (ranked.empty())
        {
            log += "FEHLER: Keine Noten in der MIDI-Datei gefunden.\n";
            return false;
        }

        std::sort(ranked.begin(), ranked.end(), [](const ChannelSuggestion& a, const ChannelSuggestion& b) {
            return a.noteCount > b.noteCount;
            });
        // Gibt es Schlagzeug (Kanal 10) UND mindestens zwei Melodiestimmen, bekommt das Schlagzeug
        // eine der drei Stimmen (als Voice 3) - die übrigen zwei gehen an die notenreichsten
        // Melodiestimmen, tiefste = Voice 1. Sonst: die drei notenreichsten, nach Tonlage sortiert.
        {
            auto isDrums = [](const ChannelSuggestion& c) { return c.drums; };
            std::vector<ChannelSuggestion> melodic, drums;
            for (auto& c : ranked) (isDrums(c) ? drums : melodic).push_back(c);
            if (!drums.empty() && melodic.size() >= 2)
            {
                melodic.resize(2);
                std::sort(melodic.begin(), melodic.end(), [](const ChannelSuggestion& a, const ChannelSuggestion& b) {
                    return a.avgPitch < b.avgPitch;
                    });
                ranked = { melodic[0], melodic[1], drums[0] };
            }
            else
            {
                if (ranked.size() > 3)
                    ranked.resize(3);
                std::sort(ranked.begin(), ranked.end(), [](const ChannelSuggestion& a, const ChannelSuggestion& b) {
                    return a.avgPitch < b.avgPitch;
                    });
            }
        }

        for (size_t i = 0; i < ranked.size() && i < 3; ++i)
            outSuggested[i] = ranked[i];

        log += "Stimmen-Vorschlag: ";
        for (int i = 0; i < 3; ++i)
            log += (outSuggested[i].channel > 0 ? outSuggested[i].label : juce::String("(keiner)")) + "  ";
        log += "\n";
        return true;
    }

    // Für die manuelle "MIDI-SET auswählen"-Stimmenauswahl (siehe MainComponent): liefert ALLE
    // Spuren (MTrk-Chunks) mit mindestens einer Note, aufsteigend nach Spurindex (nicht nach
    // Notendichte sortiert wie analyzeMidiFile - hier soll der Nutzer selbst beurteilen können,
    // welche Spur z.B. die Hauptmelodie ist, statt sich auf die Heuristik zu verlassen).
    //
    // Gruppiert bewusst nach Spur statt nach MIDI-Kanal: DAW-Exporte (z.B. Studio One) legen
    // jede Spur als eigenen MTrk-Chunk an, belassen aber praktisch immer alle auf Kanal 1, da
    // jede Spur ein eigenes virtuelles Instrument anspricht statt sich einen Multitimbral-Kanal
    // zu teilen. Eine Kanal-basierte Gruppierung (frühere Implementierung) hätte solche Dateien
    // fälschlich zu einem einzigen Eintrag zusammengefasst.
    static bool listAllTracks(const juce::File& midiFile, std::vector<ChannelSuggestion>& outTracks, juce::String& log)
    {
        outTracks.clear();
        if (!gatherTrackStats(midiFile, outTracks, log))
            return false;
        std::sort(outTracks.begin(), outTracks.end(), [](const ChannelSuggestion& a, const ChannelSuggestion& b) {
            return a.trackIndex < b.trackIndex;
            });
        return true;
    }

    // Konvertiert die gewählten 3 MIDI-Spuren (bzw. bei useRoundTrip immer Spur-Index 1-3,
    // die Tier-1/vereinfachten Tracks aus SidToMidiCoreEngine) in eine .sid-Datei, die die
    // miniplayer2-Playroutine einbettet.
    static bool convertMidiToSidFile(const juce::File& midiFile,
        const juce::File& targetSidFile,
        std::array<int, 3> trackIndices,
        bool useRoundTrip,
        juce::String& logOutput,
        std::array<VoiceRole, 3> voiceRoles = { VoiceRole::Auto, VoiceRole::Auto, VoiceRole::Auto })
    {
        if (useRoundTrip)
            trackIndices = { 1, 2, 3 };
        sm::VoiceSources sources;
        for (size_t v = 0; v < 3; ++v)
            if (trackIndices[v] >= 0) sources[v].push_back(sm::MidiVoiceSource{ trackIndices[v], (int)voiceRoles[v], 0 });
        return convertMidiToSidFile(midiFile, targetSidFile, sources, useRoundTrip, logOutput);
    }

    // Several tracks can play one voice: where they sound together, the one with the higher priority wins and the
    // others fill its pauses (see mergeByPriority). The sound of the voice is that of its first (highest) track.
    static bool convertMidiToSidFile(const juce::File& midiFile,
        const juce::File& targetSidFile,
        sm::VoiceSources sources,
        bool /*useRoundTrip*/,
        juce::String& logOutput)
    {
        juce::File toolsDir, playerSourceFile;
        if (!locateMiniplayerFiles(toolsDir, playerSourceFile, logOutput))
            return false;

        juce::FileInputStream inStream(midiFile);
        if (!inStream.openedOk())
        {
            logOutput += juce::String(juce::CharPointer_UTF8("FEHLER: Konnte MIDI-Datei nicht \xc3\xb6" "ffnen.\n"));
            return false;
        }
        juce::MidiFile parsedFile;
        if (!parsedFile.readFrom(inStream))
        {
            logOutput += juce::String(juce::CharPointer_UTF8("FEHLER: Ung\xc3\xbcltige MIDI-Datei.\n"));
            return false;
        }

        parsedFile.convertTimestampTicksToSeconds();

        // Pro Quelle wird NUR die eigene Spur ausgewertet (kein Zusammenmischen ueber MIDI-Kanal): DAW-Exporte legen
        // jede Spur als eigenen MTrk-Chunk an, belassen dabei aber haeufig alle Spuren auf demselben Kanal.
        std::array<std::vector<NoteEvent>, 3> rawNotes;
        std::array<int, 3> voiceProgram = { 0, 0, 0 };
        std::array<bool, 3> voiceHasProgram = { false, false, false };
        std::array<int, 3> voiceChannel = { -1, -1, -1 };
        std::array<VoiceRole, 3> voiceRoles = { VoiceRole::Auto, VoiceRole::Auto, VoiceRole::Auto };

        for (int v = 0; v < 3; ++v)
        {
            auto srcs = sources[(size_t)v];
            std::stable_sort(srcs.begin(), srcs.end(), [](const sm::MidiVoiceSource& x, const sm::MidiVoiceSource& y) { return x.priority < y.priority; });
            std::vector<std::vector<NoteEvent>> levels;
            for (const auto& src : srcs)
            {
                // Kennung = Spurindex (Bit 0-15) + optional Kanalfilter (Bit 16-20), siehe ChannelSuggestion::partId()
                const int part = src.part;
                const int idx = part < 0 ? -1 : (part & 0xFFFF);
                const int chanFilter = part < 0 ? 0 : ((part >> 16) & 0x1F);
                auto* track = (idx >= 0 && idx < parsedFile.getNumTracks()) ? parsedFile.getTrack(idx) : nullptr;
                if (track == nullptr) continue;

                std::vector<NoteEvent> notes;
                int program = 0, channel = -1;
                bool hasProgram = false;
                for (int i = 0; i < track->getNumEvents(); ++i)
                {
                    auto* ev = track->getEventPointer(i);
                    if (ev == nullptr) continue;
                    if (chanFilter > 0 && ev->message.getChannel() != chanFilter) continue;

                    if (ev->message.isProgramChange() && !hasProgram)
                    {
                        program = ev->message.getProgramChangeNumber();
                        hasProgram = true;
                    }
                    else if (ev->message.isNoteOn())
                    {
                        if (channel < 0)
                            channel = ev->message.getChannel();
                        NoteEvent n;
                        n.startSeconds = ev->message.getTimeStamp();
                        n.endSeconds = n.startSeconds + 0.05; // vorlaeufig, wird unten per Note-Off korrigiert
                        n.pitch = ev->message.getNoteNumber();
                        notes.push_back(n);
                    }
                }
                // Note-Off-Zeiten nachtragen: "letzte offene Note derselben Tonhoehe in dieser Spur"
                resolveNoteOffs(*track, notes, chanFilter);
                if (notes.empty()) continue;
                if (levels.empty())      // the voice sounds like its first track
                {
                    voiceProgram[(size_t)v] = program;
                    voiceHasProgram[(size_t)v] = hasProgram;
                    voiceChannel[(size_t)v] = channel;
                    voiceRoles[(size_t)v] = (VoiceRole)juce::jlimit(0, 4, src.role);
                }
                levels.push_back(std::move(notes));
            }
            rawNotes[(size_t)v] = levels.size() <= 1 ? (levels.empty() ? std::vector<NoteEvent>() : levels[0])
                                                     : sm::mergeByPriority(levels, &NoteEvent::startSeconds, &NoteEvent::endSeconds);
        }

        juce::String title = midiFile.getFileNameWithoutExtension();

        std::array<std::vector<NoteEvent>, 3> voiceNotes;
        std::array<int, 3> instrumentControlByte{};
        std::array<int, 3> instrumentAD{};
        std::array<int, 3> instrumentSR{};

        for (int v = 0; v < 3; ++v)
        {
            auto mono = monophonize(rawNotes[(size_t)v]);
            voiceNotes[(size_t)v] = quantizeToFrames(mono);

            int program = voiceHasProgram[(size_t)v] ? voiceProgram[(size_t)v] : 0;
            bool isDrumChannel = (voiceChannel[(size_t)v] == 10);
            pickInstrument(program, isDrumChannel, voiceRoles[(size_t)v], instrumentControlByte[(size_t)v],
                instrumentAD[(size_t)v], instrumentSR[(size_t)v]);
        }

        int totalNotes = 0;
        for (auto& vn : voiceNotes) totalNotes += (int)vn.size();
        if (totalNotes == 0)
        {
            logOutput += juce::String(juce::CharPointer_UTF8("FEHLER: Keine Noten auf den gew\xc3\xa4hlten Spuren gefunden.\n"));
            return false;
        }

        juce::String asmSource = generateAssemblySource(voiceNotes, instrumentControlByte, instrumentAD,
            instrumentSR, title);

        return assembleWithDasm(asmSource, toolsDir, playerSourceFile, targetSidFile, logOutput);
    }

private:
    //==============================================================================
    struct NoteEvent
    {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        int    pitch = 0;
        int    startFrame = 0;
        int    durationFrames = 0;
    };

    static constexpr double kFramesPerSecond = 50.0; // PAL, wie der Rest der App (siehe ticksPerFrame anderswo)
    static constexpr int    kMaxDurationFrames = 128; // Format-Obergrenze, siehe FORMAT_NOTES.md
    static constexpr int    kMaxEventsPerPattern = 80; // konservativ, damit ein Pattern sicher < 256 Bytes bleibt

    static bool loadMidiFile(const juce::File& midiFile, juce::MidiFile& outParsed, juce::String& log)
    {
        juce::FileInputStream inStream(midiFile);
        if (!inStream.openedOk())
        {
            log += juce::String(juce::CharPointer_UTF8("FEHLER: Konnte MIDI-Datei nicht \xc3\xb6" "ffnen.\n"));
            return false;
        }
        if (!outParsed.readFrom(inStream))
        {
            log += juce::String(juce::CharPointer_UTF8("FEHLER: Ung\xc3\xbcltige MIDI-Datei.\n"));
            return false;
        }
        return true;
    }

    // Schlagzeug erkennen, wo sich ein MIDI nicht an "Kanal 10" haelt (viele neuere Dateien: jede Spur ein Drum-VST): am Spurnamen
    // oder am Notenbild - fast nur GM-Drum-Noten (35-81), wenige verschiedene Toene, kurze Noten, Kick und Snare / Hi-Hat dabei.
    static bool looksLikeDrums(const juce::String& rawName, int noteCount, const std::map<int, int>& pitches, std::vector<double> durations)
    {
        const auto name = rawName.toLowerCase();
        for (auto* bad : { "bass", "lead", "synth", "pad", "piano", "string", "guitar", "gitarre", "organ", "brass", "arp" })
            if (name.contains(bad) && !name.contains("drum")) return false;
        for (auto* hint : { "drum", "drm", "perc", "kick", "snare", "hihat", "hi-hat", "hi hat", "hat", "cymbal", "crash", "ride", "clap", "conga", "bongo", "tamb", "shaker",
                            "schlagzeug", "batterie", "beat", "tom", "808", "909", "kit" })
            if (name.contains(hint)) return true;
        const auto trimmed = name.trim();
        for (auto* shortName : { "dr", "drs", "hh", "bd", "sd", "cp", "oh", "ch" })
            if (trimmed == shortName) return true;
        // a track of one or two kinds of drum (only a hi-hat, only a snare ...): all notes are GM drum notes and short; a bass
        // pedal on the kick note does not count, there has to be a note that a bass does not play
        if (noteCount >= 8 && pitches.size() <= 3 && !durations.empty())
        {
            static const std::set<int> drumSet { 35, 36, 37, 38, 39, 40, 42, 44, 46, 49, 51, 54, 56, 69, 70 };
            static const std::set<int> notBass { 38, 39, 40, 42, 44, 46, 49, 51, 54, 56, 69, 70 };
            bool all = true, other = false;
            for (auto& p : pitches) { if (!drumSet.count(p.first)) all = false; if (notBass.count(p.first)) other = true; }
            std::sort(durations.begin(), durations.end());
            if (all && other && durations[durations.size() / 2] <= 0.2) return true;
        }
        if (noteCount < 8 || pitches.size() < 3 || pitches.size() > 16) return false;
        int inRange = 0;
        for (auto& p : pitches) if (p.first >= 35 && p.first <= 81) inRange += p.second;
        if ((double)inRange / (double)noteCount < 0.95) return false;
        if (durations.empty()) return false;
        std::sort(durations.begin(), durations.end());
        if (durations[durations.size() / 2] > 0.35) return false;
        const bool kick = pitches.count(35) || pitches.count(36);
        const bool snare = pitches.count(38) || pitches.count(40);
        const bool hat = pitches.count(42) || pitches.count(44) || pitches.count(46);
        return (kick && (snare || hat)) || (snare && hat);
    }

    // Von analyzeMidiFile() UND listAllTracks() genutzte gemeinsame Notenstatistik: alle
    // Spuren (MTrk-Chunks) mit mindestens einer Note, unsortiert (Reihenfolge/Sortierung
    // obliegt dem jeweiligen Aufrufer - analyzeMidiFile sortiert nach Notendichte,
    // listAllTracks nach Spurindex).
    static bool gatherTrackStats(const juce::File& midiFile, std::vector<ChannelSuggestion>& outTracks, juce::String& log)
    {
        outTracks.clear();

        juce::MidiFile parsedFile;
        if (!loadMidiFile(midiFile, parsedFile, log))
            return false;

        parsedFile.convertTimestampTicksToSeconds();

        for (int t = 0; t < parsedFile.getNumTracks(); ++t)
        {
            auto* track = parsedFile.getTrack(t);
            if (track == nullptr) continue;

            struct ChanStat { int noteCount = 0; double pitchSum = 0.0; int program = -1; std::map<int, int> pitches; std::vector<double> durations; };
            std::array<ChanStat, 17> chans;
            juce::String name;
            for (int i = 0; i < track->getNumEvents(); ++i)
            {
                auto* ev = track->getEventPointer(i);
                if (ev == nullptr) continue;
                const auto& msg = ev->message;
                if (name.isEmpty() && msg.isTrackNameEvent())
                    name = msg.getTextFromTextMetaEvent();
                else if (msg.isNoteOn())
                {
                    auto& c = chans[(size_t)juce::jlimit(1, 16, msg.getChannel())];
                    c.noteCount++;
                    c.pitchSum += msg.getNoteNumber();
                    c.pitches[msg.getNoteNumber()]++;
                }
                else if (msg.isProgramChange())
                {
                    auto& c = chans[(size_t)juce::jlimit(1, 16, msg.getChannel())];
                    if (c.program < 0) c.program = msg.getProgramChangeNumber();
                }
            }

            {
                juce::MidiMessageSequence seq(*track);
                seq.updateMatchedPairs();
                for (int i = 0; i < seq.getNumEvents(); ++i)
                {
                    auto* ev = seq.getEventPointer(i);
                    if (ev != nullptr && ev->message.isNoteOn() && ev->noteOffObject != nullptr)
                        chans[(size_t)juce::jlimit(1, 16, ev->message.getChannel())].durations.push_back(ev->noteOffObject->message.getTimeStamp() - ev->message.getTimeStamp());
                }
            }
            int channelsWithNotes = 0, onlyChannel = -1;
            for (int ch = 1; ch <= 16; ++ch)
                if (chans[(size_t)ch].noteCount > 0) { ++channelsWithNotes; onlyChannel = ch; }
            if (channelsWithNotes == 0) continue;

            juce::String trackLabel = name.isNotEmpty() ? name : ("Spur " + juce::String(t));
            for (int ch = 1; ch <= 16; ++ch)
            {
                auto& c = chans[(size_t)ch];
                if (c.noteCount <= 0) continue;
                ChannelSuggestion cs;
                cs.trackIndex = t;
                cs.channel = ch;
                cs.channelFilter = channelsWithNotes > 1 ? ch : 0;
                cs.noteCount = c.noteCount;
                cs.avgPitch = c.pitchSum / (double)c.noteCount;
                cs.drums = ch == 10 || looksLikeDrums(name, c.noteCount, c.pitches, c.durations);
                juce::String l = trackLabel;
                if (channelsWithNotes > 1)
                    l << ", " << juce::String(juce::CharPointer_UTF8("Kanal ")) << ch << (ch == 10 ? " (Schlagzeug)" : juce::String());
                if (cs.drums && ch != 10) l << " (Schlagzeug erkannt)";
                cs.label = l + " (" + juce::String(c.noteCount)
                    + juce::String::fromUTF8(" Noten, Ø-Tonhöhe ")
                    + juce::String(cs.avgPitch, 1) + ")";
                outTracks.push_back(cs);
            }
            (void)onlyChannel;
        }
        return true;
    }

    static bool detectSidmSignature(juce::MidiFile& parsedFile)
    {
        auto* track0 = parsedFile.getTrack(0);
        if (track0 == nullptr) return false;
        for (int i = 0; i < track0->getNumEvents(); ++i)
        {
            auto* ev = track0->getEventPointer(i);
            if (ev == nullptr) continue;
            const auto& msg = ev->message;
            if (msg.isMetaEvent() && msg.getMetaEventType() == 0x7f)
            {
                auto* data = msg.getMetaEventData();
                int len = msg.getMetaEventLength();
                if (len >= 4 && data[0] == 'S' && data[1] == 'I' && data[2] == 'D' && data[3] == 'M')
                    return true;
            }
        }
        return false;
    }

    // Sucht ThirdParty/miniplayer relativ zur .exe (mehrere Verzeichnisebenen nach oben,
    // passend zum Projucer-Build-Layout Builds/VisualStudio2026/<config>/App/*.exe). Das
    // funktioniert nur im Quellbaum eines Entwickler-Checkouts. Für eine installierte App
    // (z.B. unter Program Files, ohne ThirdParty daneben) greift extractEmbeddedMiniplayer()
    // als Fallback auf die fest eingebetteten Kopien (EmbeddedMiniplayer.h/.cpp) zurück.
    static bool locateMiniplayerFiles(juce::File& outToolsDir, juce::File& outPlayerSource, juce::String& log)
    {
        auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        juce::File dir = exeFile.getParentDirectory();
        for (int level = 0; level < 8 && dir.exists(); ++level)
        {
            auto candidate = dir.getChildFile("ThirdParty").getChildFile("miniplayer");
            if (candidate.isDirectory() && candidate.getChildFile("player.s").existsAsFile())
            {
                outToolsDir = candidate.getChildFile("tools");
                outPlayerSource = candidate.getChildFile("player.s");
                return true;
            }
            dir = dir.getParentDirectory();
        }
        return extractEmbeddedMiniplayer(outToolsDir, outPlayerSource, log);
    }

    // Schreibt dasm.exe und player.s aus den eingebetteten Ressourcen (EmbeddedMiniplayer)
    // in einen festen Cache-Ordner im Temp-Verzeichnis und liefert dessen Pfade zurück.
    // Wird nur als Fallback verwendet, wenn locateMiniplayerFiles() nichts relativ zur
    // .exe findet (z.B. bei einer Installation ohne mitgelieferten ThirdParty-Ordner).
    static bool extractEmbeddedMiniplayer(juce::File& outToolsDir, juce::File& outPlayerSource, juce::String& log)
    {
        auto cacheDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("SIDIMIDI_embedded_miniplayer");
        auto toolsDir = cacheDir.getChildFile("tools");
        auto dasmExe = toolsDir.getChildFile("dasm.exe");
        auto playerS = cacheDir.getChildFile("player.s");

        auto needsWrite = [](const juce::File& f, int expectedSize)
        {
            return !f.existsAsFile() || (int)f.getSize() != expectedSize;
        };

        if (needsWrite(dasmExe, EmbeddedMiniplayer::dasmExeDataSize)
            || needsWrite(playerS, EmbeddedMiniplayer::playerSDataSize))
        {
            toolsDir.createDirectory();
            if (!dasmExe.replaceWithData(EmbeddedMiniplayer::dasmExeData, (size_t)EmbeddedMiniplayer::dasmExeDataSize)
                || !playerS.replaceWithData(EmbeddedMiniplayer::playerSData, (size_t)EmbeddedMiniplayer::playerSDataSize))
            {
                log += "FEHLER: Eingebettete miniplayer-Ressourcen konnten nicht nach "
                    + cacheDir.getFullPathName() + " entpackt werden.\n";
                return false;
            }
        }

        outToolsDir = toolsDir;
        outPlayerSource = playerS;
        return true;
    }

    // Ordnet jeder offenen Note (nur Startzeit bekannt) eine Endzeit über den passenden
    // Note-Off-Event derselben Tonhöhe in dieser Spur zu (erste noch offene Note zuerst
    // beendet - korrekt für den Normalfall nicht verschachtelter Wiederholungen derselben
    // Tonhöhe).
    static void resolveNoteOffs(const juce::MidiMessageSequence& track, std::vector<NoteEvent>& notes, int channelFilter = 0)
    {
        std::sort(notes.begin(), notes.end(), [](const NoteEvent& a, const NoteEvent& b) { return a.startSeconds < b.startSeconds; });

        // Baut eine Liste aller Note-Off-Zeitpunkte (inkl. NoteOn mit Velocity 0) in der Spur.
        struct OffEvt { double time; int pitch; };
        std::vector<OffEvt> offs;
        for (int i = 0; i < track.getNumEvents(); ++i)
        {
            auto* ev = track.getEventPointer(i);
            if (ev == nullptr) continue;
            if (ev->message.isNoteOff() && (channelFilter == 0 || ev->message.getChannel() == channelFilter))
                offs.push_back({ ev->message.getTimeStamp(), ev->message.getNoteNumber() });
        }
        std::sort(offs.begin(), offs.end(), [](const OffEvt& a, const OffEvt& b) { return a.time < b.time; });

        // Für jede Note: den ersten Note-Off derselben Tonhöhe NACH ihrem Start verwenden,
        // der noch keiner früheren Note zugeordnet wurde.
        std::vector<bool> offUsed(offs.size(), false);
        for (auto& n : notes)
        {
            bool found = false;
            for (size_t i = 0; i < offs.size(); ++i)
            {
                if (offUsed[i] || offs[i].pitch != n.pitch || offs[i].time < n.startSeconds) continue;
                n.endSeconds = offs[i].time;
                offUsed[i] = true;
                found = true;
                break;
            }
            if (!found)
                n.endSeconds = n.startSeconds + 0.3; // Fallback: kein passendes NoteOff gefunden
        }
    }

    // Reduziert überlappende Noten auf eine monophone Linie: eine neue Note beendet eine
    // noch klingende sofort (letzte Note gewinnt) - Standard-Verhalten für monophone Stimmen.
    static std::vector<NoteEvent> monophonize(std::vector<NoteEvent> notes)
    {
        std::sort(notes.begin(), notes.end(), [](const NoteEvent& a, const NoteEvent& b) { return a.startSeconds < b.startSeconds; });
        std::vector<NoteEvent> out;
        for (auto& n : notes)
        {
            if (!out.empty() && out.back().endSeconds > n.startSeconds)
                out.back().endSeconds = n.startSeconds;
            if (n.endSeconds > n.startSeconds)
                out.push_back(n);
        }
        return out;
    }

    static std::vector<NoteEvent> quantizeToFrames(const std::vector<NoteEvent>& notes)
    {
        std::vector<NoteEvent> out;
        for (auto n : notes)
        {
            n.startFrame = (int)std::round(n.startSeconds * kFramesPerSecond);
            int endFrame = (int)std::round(n.endSeconds * kFramesPerSecond);
            n.durationFrames = juce::jmax(1, endFrame - n.startFrame);
            out.push_back(n);
        }
        return out;
    }

    // Wählt Wellenform (SID-Steuerregister-Byte inkl. Gate-Bit) + ADSR für eine Stimme.
    // Bei role != Auto (vom Nutzer im "MIDI-SET"-Picker per Rechtsklick zugewiesen, siehe
    // MainComponent::toggleChannelSelection()) übersteuert die Rolle die automatische
    // GM-Programm/Kanal-10-Heuristik - das behebt u.a. Drum-Spuren, die mangels MIDI-Kanal 10
    // (z.B. bei manchen DAW-Exporten) fälschlich nicht als Rauschen programmiert wurden.
    // Bei role == Auto greift die bisherige Logik unverändert (Umkehrung der Grundidee von
    // SidToMidiCoreEngine::waveformToProgramChange in SidToMidiCore.h). Pulswellenform wird
    // in v1 bewusst vermieden, da sie ohne Pulsweiten-Tabelle (siehe Klassendoku) lautlos
    // bliebe.
    static void pickInstrument(int gmProgram, bool isDrumChannel, VoiceRole role,
        int& outControlByte, int& outAD, int& outSR)
    {
        switch (role)
        {
        case VoiceRole::Drums:
            outControlByte = 0x81; // Rauschen + Gate
            outAD = 0x02;          // Attack 0, Decay 2 - perkussiv kurz
            outSR = 0x03;          // Sustain 0, Release 3
            return;
        case VoiceRole::Bass:
            outControlByte = 0x11; // Dreieck + Gate
            outAD = 0x09;          // Attack 0, Decay 9 - knackiger Einsatz (Startwert, ggf. nachjustieren)
            outSR = 0x21;          // Sustain 2, Release 1 - kurz ausklingend, nicht dauerhaft gehalten
            return;
        case VoiceRole::Pad:
            outControlByte = 0x21; // Sägezahn + Gate
            outAD = 0x9a;          // Attack 9, Decay a - langsam einschwingend (Startwert, ggf. nachjustieren)
            outSR = 0xf7;          // Sustain f, Release 7 - lang gehalten und ausklingend
            return;
        case VoiceRole::Lead:
            outControlByte = 0x21; // Sägezahn + Gate
            outAD = 0x17;          // Attack 1, Decay 7 - schneller Einsatz (Startwert, ggf. nachjustieren)
            outSR = 0x94;          // Sustain 9, Release 4 - gut hörbar gehalten, moderat ausklingend
            return;
        case VoiceRole::Auto:
        default:
            break;
        }

        if (isDrumChannel)
        {
            outControlByte = 0x81; // Rauschen + Gate
            outAD = 0x02;          // Attack 0, Decay 2 - perkussiv kurz
            outSR = 0x03;          // Sustain 0, Release 3
            return;
        }
        if (gmProgram >= 16 && gmProgram < 80)
        {
            outControlByte = 0x21; // Sägezahn + Gate - Orgel/Gitarre/Blech/Holzblas/Streicher
            outAD = 0x06;
            outSR = 0xc5;
        }
        else
        {
            outControlByte = 0x11; // Dreieck + Gate - Klavier/Bass/weiche Klänge, Standardfall
            outAD = 0x28;
            outSR = 0xa6;
        }
    }

    // Teilt eine Notendauer (in Frames) in Häppchen <= kMaxDurationFrames auf - das
    // Datenformat kann pro Notenereignis maximal 128 Frames am Stück kodieren (siehe
    // FORMAT_NOTES.md). Jedes Häppchen wird als eigenes Notenereignis derselben Tonhöhe
    // ausgegeben (kurzes Neu-Antriggern der Hüllkurve alle ~2.56s bei sehr langen Noten).
    static std::vector<int> splitDurationFrames(int totalFrames)
    {
        std::vector<int> parts;
        while (totalFrames > 0)
        {
            int chunk = juce::jmin(totalFrames, kMaxDurationFrames);
            parts.push_back(chunk);
            totalFrames -= chunk;
        }
        if (parts.empty()) parts.push_back(1);
        return parts;
    }

    // Note-Tabellenindex des miniplayer2-Formats (C0=1 .. H4=60, siehe FORMAT_NOTES.md).
    // Bildet MIDI-Notennummer 0-127 linear ab und klemmt auf den darstellbaren Bereich.
    static int midiPitchToPlayerNoteIndex(int midiPitch)
    {
        // MIDI 36 (C2) auf den Player-Notenindex 1 (C0) gemappt - grobe, aber plausible
        // Zuordnung, die den 5-Oktaven-Bereich des Players auf den üblichen musikalischen
        // Kernbereich legt statt auf den SID-Hardware-Nullpunkt.
        int idx = (midiPitch - 36) + 1;
        return juce::jlimit(1, 60, idx);
    }

    //==============================================================================
    // Baut den DASM-Assembler-Quelltext: PSID-Header, Musikdaten-Tabellen (ein Pattern-
    // Satz je Stimme, ggf. auf mehrere Patterns aufgeteilt), Player-Einbindung, Init-Stub.
    static juce::String generateAssemblySource(const std::array<std::vector<NoteEvent>, 3>& voiceNotes,
        const std::array<int, 3>& controlByte,
        const std::array<int, 3>& adByte,
        const std::array<int, 3>& srByte,
        const juce::String& title)
    {
        juce::String s;
        s << "; Auto-generiert von MidiToSidCoreEngine - siehe ThirdParty/miniplayer/FORMAT_NOTES.md\n";
        s << "                processor 6502\n";
        s << "                org $0000\n\n";
        s << "SUBTUNES        = 1\n\n";
        s << "                dc.b \"PSID\"\n";
        s << "                dc.b 0,2\n";
        s << "                dc.b 0,$7c\n";
        s << "                dc.b $00,$00\n";
        s << "                dc.b >Init,<Init\n";
        s << "                dc.b >PlayRoutine,<PlayRoutine\n";
        s << "                dc.b 0,SUBTUNES\n";
        s << "                dc.b 0,1\n";
        s << "                dc.b 0,0,0,0\n\n";
        // Titel/Autor/Veroeffentlicht: je 32 Bytes ASCII, mit Nullen aufgefuellt (DASM fuellt
        // Luecken sonst mit $FF - das zeigte sich in SID-Playern als "ÿÿÿ" hinter dem Titel).
        auto field = [](const juce::String& text)
        {
            juce::String ascii;
            for (auto c : text)
            {
                if (c >= 32 && c < 127 && c != '"') ascii << juce::String::charToString(c);
                else if (c == 0xe4 || c == 0xc4) ascii << "a";
                else if (c == 0xf6 || c == 0xd6) ascii << "o";
                else if (c == 0xfc || c == 0xdc) ascii << "u";
                else if (c == 0xdf) ascii << "s";
                else ascii << "_";
            }
            ascii = ascii.substring(0, 31);
            return "                dc.b \"" + ascii + "\"\n                ds.b " + juce::String(32 - ascii.length()) + ",0\n";
        };
        s << "                org $0016\n";
        s << field(title);
        s << field("SIDiMIDI (MIDI to SID)");
        s << field(juce::String(juce::Time::getCurrentTime().getYear()) + " SIDiMIDI");
        s << "                dc.b 0,4\n";          // Flags: PAL
        s << "                dc.b 0,0,0,0\n";      // startPage, pageLength, 2./3. SID
        s << "                org $007c\n";
        s << "                dc.b $00,$10\n";
        s << "                rorg $1000\n\n";

        // Pro Stimme: Notenliste in einzelne Pattern-Blöcke (<= kMaxEventsPerPattern
        // Ereignisse) aufteilen; jedes Pattern-Label ist page-aligned (ALIGN 256), damit
        // keines der Muster über eine Seitengrenze läuft (Format-Vorgabe, siehe Klassendoku).
        //
        // Zweiphasig, bewusst getrennt: zuerst (Phase 1) eine flache Ereignisliste für die
        // GESAMTE Stimme aufbauen (unabhängig von Pattern-Grenzen), danach (Phase 2) rein
        // mechanisch in Patterns zerschneiden. Bei einer verschränkten Version (Pause/Note
        // live während des Pattern-Füllens aufteilen) würde ein Abbruch mitten in einer
        // gesplitteten Pause/Note beim nächsten Pattern denselben Rest noch einmal von
        // vorne beginnen und die Dauer verdoppeln - das umgeht die Trennung strukturell.
        struct PatternRef { juce::String label; };
        struct PatternEvent { int durationFrames; int pitch; bool isRest; bool sendInstrument; };
        std::array<std::vector<PatternRef>, 3> voicePatterns;
        juce::String patternBodies;

        for (int v = 0; v < 3; ++v)
        {
            const auto& notes = voiceNotes[(size_t)v];

            std::vector<PatternEvent> flatEvents;
            bool instrumentSentForVoice = false;
            int lastEndFrame = 0;
            for (auto& n : notes)
            {
                int gapFrames = n.startFrame - lastEndFrame;
                for (int g : splitDurationFrames(juce::jmax(0, gapFrames)))
                    flatEvents.push_back({ g, -1, true, false });

                for (int chunk : splitDurationFrames(n.durationFrames))
                {
                    bool sendInstrument = !instrumentSentForVoice;
                    instrumentSentForVoice = true;
                    flatEvents.push_back({ chunk, n.pitch, false, sendInstrument });
                }
                lastEndFrame = n.startFrame + n.durationFrames;
            }

            size_t evIdx = 0;
            int pattCounter = 0;
            do
            {
                juce::String label = "v" + juce::String(v) + "patt" + juce::String(pattCounter++);
                voicePatterns[(size_t)v].push_back({ label });
                patternBodies << "                align $100\n";
                patternBodies << label << ":\n";

                int emitted = 0;
                while (evIdx < flatEvents.size() && emitted < kMaxEventsPerPattern)
                {
                    const auto& e = flatEvents[evIdx];
                    if (e.isRest)
                    {
                        patternBodies << "                dc.b " << durationByte(e.durationFrames) << ",REST\n";
                    }
                    else
                    {
                        int noteVal = midiPitchToPlayerNoteIndex(e.pitch) * 2;
                        if (!e.sendInstrument) noteVal += 1; // Bit0=1: vorheriges Instrument beibehalten
                        patternBodies << "                dc.b " << durationByte(e.durationFrames) << "," << noteVal;
                        if (e.sendInstrument)
                            patternBodies << "," << v; // Instrumente sind 0-basiert (siehe FORMAT_NOTES.md)
                        patternBodies << "\n";
                    }
                    evIdx++;
                    emitted++;
                }
                patternBodies << "                dc.b 0\n\n";
            } while (evIdx < flatEvents.size());
        }

        // songTbl + gemeinsame Orderlist-Tabelle: Kanal 0/1/2 teilen sich EINE Tabelle,
        // jeweils mit eigenem Start-Offset (siehe FORMAT_NOTES.md).
        s << "songTbl:        dc.w song0\n";
        {
            juce::String offsets = "                dc.b ";
            int runningOffset = 0;
            juce::StringArray offsetStrs;
            for (int v = 0; v < 3; ++v)
            {
                offsetStrs.add(juce::String(runningOffset));
                runningOffset += (int)voicePatterns[(size_t)v].size() + 2; // +2 für SONGJUMP-Paar
            }
            offsets << offsetStrs.joinIntoString(",");
            s << offsets << "\n\n";
        }

        // pattTblLo/Hi: ALLE Patterns aller 3 Stimmen fortlaufend nummeriert (1-basiert).
        juce::StringArray allPatternLabels;
        for (int v = 0; v < 3; ++v)
            for (auto& p : voicePatterns[(size_t)v])
                allPatternLabels.add(p.label);

        // Orderlist-Bytes 1-127 sind direkte Pattern-Nummern, >=128 wird als Transpose-
        // Kommando gelesen (siehe FORMAT_NOTES.md) - mehr als 127 Patterns insgesamt würden
        // also falsch interpretiert. Für sehr lange/dichte Songs (v1-Grenze) nur loggen statt
        // hart abzubrechen, da das in der Praxis bei normal langen MIDI-Dateien nicht greift.
        if (allPatternLabels.size() > 127)
            s << "; WARNUNG: " << allPatternLabels.size() << " Patterns insgesamt - Format erlaubt nur 127!\n";

        s << "pattTblLo:      ";
        for (auto& lbl : allPatternLabels) s << "dc.b <" << lbl << "\n                ";
        s << "\npattTblHi:      ";
        for (auto& lbl : allPatternLabels) s << "dc.b >" << lbl << "\n                ";
        s << "\n\n";

        // Instrumententabellen: genau 3 Einträge, einer je Stimme (0-basiert indiziert).
        s << "insAD:          dc.b " << adByte[0] << "," << adByte[1] << "," << adByte[2] << "\n";
        s << "insWavePos:     dc.b 1,2,3\n";
        s << "insPulsePos:    dc.b 0,0,0\n";
        s << "insFiltPos:     dc.b 0,0,0\n\n";

        s << "waveTbl:        dc.b " << controlByte[0] << "," << controlByte[1] << "," << controlByte[2] << "\n";
        s << "waveSRTbl:      dc.b " << srByte[0] << "," << srByte[1] << "," << srByte[2] << "\n";
        s << "waveNextTbl:    dc.b 0,0,0\n";
        s << "noteTbl:        dc.b 0,0,0\n\n";

        s << "pulseLimitTbl:  dc.b 0\npulseSpdTbl:    dc.b 0\npulseNextTbl:   dc.b 0\n";
        s << "filtLimitTbl:   dc.b 0\nfiltSpdTbl:     dc.b 0\nfiltNextTbl:    dc.b 0\n\n";

        // Gemeinsame Orderlist-Tabelle: pro Stimme ihre Pattern-Nummern (1-basiert, laufend
        // über alle 3 Stimmen durchnummeriert wie pattTblLo/Hi oben), danach SONGJUMP zum
        // eigenen Start (Endlosschleife - üblich für Chiptune-Player, die vom Aufrufer
        // zeitlich begrenzt werden, siehe SidCpU6502Engine::traceSidExecution).
        s << "song0:\n";
        int patternNumberBase = 1;
        for (int v = 0; v < 3; ++v)
        {
            s << "                ; Stimme " << v << "\n";
            for (size_t i = 0; i < voicePatterns[(size_t)v].size(); ++i)
                s << "                dc.b " << (patternNumberBase + (int)i) << "\n";
            int loopTarget = 0;
            for (int vv = 0; vv < v; ++vv)
                loopTarget += (int)voicePatterns[(size_t)vv].size() + 2;
            s << "                dc.b 0," << loopTarget << "\n";
            patternNumberBase += (int)voicePatterns[(size_t)v].size();
        }
        s << "\n";

        s << patternBodies;

        s << "        ; ---- Player-Konfiguration ----\n";
        s << "PLAYER_ZPBASE   = $20\n";
        s << "PLAYER_ZPOPT    = 1\n";
        s << "PLAYER_SFX      = 0\n";
        s << "PLAYER_MODULES  = 0\n\n";
        s << "                include \"player.s\"\n\n";
        s << "Init:           tax\n";
        s << "                inx\n";
        s << "                stx PlayRoutine+1\n";
        s << "                rts\n";

        return s;
    }

    static juce::String durationByte(int frames)
    {
        frames = juce::jlimit(1, kMaxDurationFrames, frames);
        int byteVal = (256 - frames) & 0xFF;
        return "$" + juce::String::toHexString(byteVal);
    }

    static juce::String asDasmString(const juce::String& text)
    {
        juce::String cleaned;
        for (auto c : text)
            cleaned << (juce::CharacterFunctions::isPrintable(c) && c != '"' ? juce::String::charToString(c) : juce::String(" "));
        return "\"" + cleaned + "\"";
    }

    //==============================================================================
    static bool assembleWithDasm(const juce::String& sourceText, const juce::File& toolsDir,
        const juce::File& playerSourceFile, const juce::File& targetSidFile, juce::String& log)
    {
        auto dasmExe = toolsDir.getChildFile("dasm.exe");
        if (!dasmExe.existsAsFile())
        {
            log += "FEHLER: dasm.exe nicht gefunden unter " + dasmExe.getFullPathName() + "\n";
            return false;
        }

        // Temporäres Arbeitsverzeichnis: dasm's "include" sucht relativ zum Quelldatei-
        // Verzeichnis, player.s muss also direkt daneben liegen.
        auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("SIDIMIDI_m2s_" + juce::String((juce::int64)juce::Time::getMillisecondCounterHiRes()));
        tempDir.createDirectory();

        auto sourceFile = tempDir.getChildFile("song.s");
        auto playerCopy = tempDir.getChildFile("player.s");
        auto outFile = tempDir.getChildFile("out.sid");

        if (!sourceFile.replaceWithText(sourceText) || !playerSourceFile.copyFileTo(playerCopy))
        {
            log += juce::String(juce::CharPointer_UTF8("FEHLER: Konnte tempor\xc3\xa4re Assembler-Dateien nicht schreiben ("))
                + tempDir.getFullPathName() + ").\n";
            return false;
        }

        juce::StringArray args;
        args.add(dasmExe.getFullPathName());
        args.add(sourceFile.getFullPathName());
        args.add("-o" + outFile.getFullPathName());
        // "include \"player.s\"" resolves relative to DASM's include search path, which is
        // NOT necessarily this process's working directory - -I explicitly points it at the
        // temp dir where player.s was just copied to (same as INCDIR, see DASM manual).
        args.add("-I" + tempDir.getFullPathName());
        args.add("-p3");
        args.add("-f3");

        juce::ChildProcess proc;
        if (!proc.start(args))
        {
            log += "FEHLER: dasm.exe konnte nicht gestartet werden.\n";
            return false;
        }
        juce::String dasmOutput = proc.readAllProcessOutput();
        bool finished = proc.waitForProcessToFinish(15000);
        int exitCode = finished ? proc.getExitCode() : -1;

        log += "[DASM] " + dasmOutput.trim() + "\n";

        if (!finished || exitCode != 0 || !outFile.existsAsFile())
        {
            log += "FEHLER: Assemblierung fehlgeschlagen (exitCode=" + juce::String(exitCode) + ").\n";
            tempDir.deleteRecursively();
            return false;
        }

        bool copiedOk = outFile.copyFileTo(targetSidFile);
        tempDir.deleteRecursively();

        if (!copiedOk)
        {
            log += "FEHLER: Konnte assemblierte .sid-Datei nicht nach " + targetSidFile.getFullPathName() + " kopieren.\n";
            return false;
        }

        log += "MIDI-zu-SID-Konvertierung erfolgreich: " + targetSidFile.getFileName() + "\n";
        return true;
    }
};
