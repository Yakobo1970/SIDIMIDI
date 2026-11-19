#pragma once

#include <JuceHeader.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <memory>
#include <iostream>
#include "SidCpu6502Engine.h"
#include "SidChip.h"

//==============================================================================
struct SidFileHeader
{
    char     magic[4] = { 0 };
    uint16_t version = 0;
    uint16_t dataOffset = 0;
    uint16_t loadAddress = 0;
    uint16_t initAddress = 0;
    uint16_t playAddress = 0;
    uint16_t songs = 1;
    uint16_t startSong = 1;
    uint32_t speedFlags = 0;
    uint16_t flags = 0;          // v2+: Bit 2-3 = Takt (1 PAL, 2 NTSC, 3 beide)
    char     title[32] = { 0 };
    char     author[32] = { 0 };
    char     released[32] = { 0 };
};

//==============================================================================
// MIDI-CC-Belegung für die SID->MIDI-Konvertierung. Wo möglich an GM2-Standard-
// Slots orientiert, damit generische MIDI-Synths die Werte sinnvoll interpretieren.
namespace SidMidiCC
{
    constexpr int PortamentoTime = 5;
    constexpr int ChannelVolume = 7;      // GM Standard
    constexpr int PortamentoSwitch = 65;  // GM Standard
    constexpr int PulseWidth = 70;        // GM2 "Sound Variation" (verlagert von CC74, siehe FilterCutoff)
    constexpr int Resonance = 71;         // GM2 "Timbre/Harmonic Intensity"
    constexpr int ReleaseTime = 72;       // GM2 Standard
    constexpr int AttackTime = 73;        // GM2 Standard
    constexpr int FilterCutoff = 74;      // GM2 "Brightness" - de-facto Standard-CC für Filter-Cutoff
    constexpr int FilterMode = 75;        // GM2 generischer Sound-Controller-Slot, kein Standard für Filtermodus
    constexpr int ReverbSend = 91;        // GM2 Standard
    constexpr int RingModActive = 102;    // In GM/GM2 undefiniert - best-effort, Standard-Synths reagieren nicht darauf
    constexpr int SyncActive = 103;       // In GM/GM2 undefiniert - best-effort, Standard-Synths reagieren nicht darauf
}

// Gemeinsamer Pitch-Bend-Bereich für Export und internen MIDI-Player, damit Kodierung
// (SidToMidiCoreEngine) und Dekodierung (AudioPlayerEngine) symmetrisch bleiben.
constexpr int kPitchBendRangeSemitones = 24;

// Kanal-Basis je Auflösungsstufe (Index 0 = einfachste ... 3 = komplexeste), gemeinsam
// genutzt von SidToMidiCoreEngine (Erzeugung) und AudioPlayerEngine (Wiedergabe/Stufen-
// umschaltung), damit beide niemals auseinanderlaufen. Kanalnummer steigt mit der Stufe:
// so erscheint die einfachste Stufe zuerst, egal ob ein DAW Spuren nach Datei-Reihenfolge
// oder nach MIDI-Kanal sortiert (manche DAWs gruppieren/sortieren Importe nach Kanal).
constexpr int kNumMidiTiers = 4;
constexpr int kMidiTierChannelBase[kNumMidiTiers] = { 1, 4, 7, 10 };

inline int sidNibbleTo7BitCC(uint8_t nibble)
{
    return juce::jlimit(0, 127, (int)nibble * 127 / 15);
}

//==============================================================================
class SidToMidiCoreEngine
{
public:
    SidToMidiCoreEngine() = default;

    static bool parseSidHeader(const juce::File& file, SidFileHeader& outHeader, std::vector<uint8_t>& outCodeData, juce::String* log = nullptr)
    {
        if (!file.existsAsFile()) return false;

        juce::FileInputStream stream(file);
        if (!stream.openedOk()) return false;

        if (stream.read(&outHeader.magic, 4) != 4) return false;

        if (std::memcmp(outHeader.magic, "PSID", 4) != 0 &&
            std::memcmp(outHeader.magic, "RSID", 4) != 0)
        {
            return false;
        }

        outHeader.version = stream.readShortBigEndian();
        outHeader.dataOffset = stream.readShortBigEndian();
        outHeader.loadAddress = stream.readShortBigEndian();
        outHeader.initAddress = stream.readShortBigEndian();
        outHeader.playAddress = stream.readShortBigEndian();
        outHeader.songs = stream.readShortBigEndian();
        outHeader.startSong = stream.readShortBigEndian();
        outHeader.speedFlags = (uint32_t)stream.readIntBigEndian();

        stream.read(&outHeader.title, 32);
        stream.read(&outHeader.author, 32);
        stream.read(&outHeader.released, 32);
        if (outHeader.version >= 2 && outHeader.dataOffset >= 0x78)
            outHeader.flags = (uint16_t)stream.readShortBigEndian();

        // Erzwingt Null-Terminierung: die Datei kann 32 Bytes ohne einziges
        // Null-Byte liefern, wonach juce::String(const char*) sonst über das
        // Array hinaus im Speicher weiterlesen würde (Out-of-Bounds Read).
        outHeader.title[31] = '\0';
        outHeader.author[31] = '\0';
        outHeader.released[31] = '\0';

        stream.setPosition(outHeader.dataOffset);

        // Robuster Fallback für Lade-Adressen
        if (outHeader.loadAddress == 0)
        {
            uint8_t low = (uint8_t)stream.readByte();
            uint8_t high = (uint8_t)stream.readByte();
            outHeader.loadAddress = ((uint16_t)high << 8) | low;
        }
        // (PSID-Spezifikation: nur wenn die Ladeadresse im Kopf 0 ist, stehen die ersten zwei
        // Datenbytes als Ladeadresse davor. Eine frühere Heuristik, die bei gesetzter Kopf-
        // Adresse trotzdem die ersten Datenbytes als Adresse deutete, zerstörte Tunes, deren
        // Code mit JMP beginnt.)

        if (outHeader.initAddress == 0)
        {
            outHeader.initAddress = outHeader.loadAddress;
        }

        // Header-Infos für das Diagnose-Fenster und die Konsole aufbereiten
        juce::String headerInfo = "[SID PARSE] Load: 0x" + juce::String::toHexString(outHeader.loadAddress) +
            " | Init: 0x" + juce::String::toHexString(outHeader.initAddress) +
            " | Play: 0x" + juce::String::toHexString(outHeader.playAddress) + "\n";

        if (log != nullptr)
            *log += headerInfo;

        std::cerr << headerInfo.toStdString();

        outCodeData.clear();

        // Mehr als 64 KB Code kann ohnehin nie in den 6502-Adressraum passen
        // (siehe traceSidExecution), daher deckelt das gleichzeitig eine
        // übergroße/präparierte Datei von unnötig vielen Reallokationen.
        constexpr size_t maxCodeSize = 65536;

        auto remaining = stream.getTotalLength() - stream.getPosition();
        if (remaining > 0)
            outCodeData.reserve((size_t)juce::jmin<int64_t>(remaining, (int64_t)maxCodeSize));

        while (!stream.isExhausted() && outCodeData.size() < maxCodeSize)
        {
            outCodeData.push_back((uint8_t)stream.readByte());
        }

        return true;
    }

    static void sidFreqToMidiNoteAndPitch(uint16_t sidFreq, int& midiNote, int& pitchBend14Bit)
    {
        if (sidFreq == 0)
        {
            midiNote = 0;
            pitchBend14Bit = 8192;
            return;
        }

        double freqHz = (double)sidFreq * 985248.0 / 16777216.0;

        if (freqHz < 8.1758)
        {
            midiNote = 0;
            pitchBend14Bit = 8192;
            return;
        }

        double noteNum = 69.0 + 12.0 * std::log2(freqHz / 440.0);
        midiNote = juce::jlimit(0, 127, (int)std::round(noteNum));

        double devInSemitones = noteNum - (double)midiNote;
        pitchBend14Bit = juce::jlimit(0, 16383, 8192 + (int)(devInSemitones * (8192.0 / kPitchBendRangeSemitones)));
    }

    // Liefert die unquantisierte (nicht gerundete) MIDI-Notenzahl. Für kontinuierliche
    // Glides/Portamento muss der Pitch-Bend relativ zur bereits gehaltenen Note verankert
    // bleiben statt relativ zu einer bei jedem Aufruf neu gerundeten "nächstgelegenen" Note
    // - sonst entsteht ein unstetiger/sägezahnartiger Bend statt eines glatten Glides.
    static bool sidFreqToContinuousNote(uint16_t sidFreq, double& outContinuousNote, double clockHz = 985248.0)
    {
        if (sidFreq == 0) return false;
        double freqHz = (double)sidFreq * clockHz / 16777216.0;
        if (freqHz < 8.1758) return false;
        outContinuousNote = 69.0 + 12.0 * std::log2(freqHz / 440.0);
        return true;
    }

    static bool convertSidToMidiFile(const juce::File& sidFile,
        const juce::File& targetMidiFile,
        juce::String& logOutput,
        int subsongIndex = 1,
        double maxSeconds = 0.0)    // 0 = default (4 minutes); pass the HVSC length to export exactly the tune
    {
        std::cerr << "[DEBUG] convertSidToMidiFile gestartet f\xc3\xbcr: " << sidFile.getFileName().toStdString() << std::endl;

        SidFileHeader header;
        std::vector<uint8_t> codeData;

        if (!parseSidHeader(sidFile, header, codeData, &logOutput))
        {
            logOutput += juce::String(juce::CharPointer_UTF8("FEHLER: Ung\xc3\xbcltiger PSID/RSID Header.\n"));
            return false;
        }

        logOutput += "Titel: " + juce::String(header.title) + "\n";
        logOutput += "Autor: " + juce::String(header.author) + "\n";

        uint16_t effectivePlayAddress = (header.playAddress != 0) ? header.playAddress : header.initAddress;

        // Bildrate des Tunes: Video (PAL/NTSC) oder CIA-1-Timer; daraus ergibt sich die MIDI-Zeit
        SidCpU6502Engine::TraceOptions topt;
        topt.ntsc = ((header.flags >> 2) & 3) == 2;
        topt.ciaSpeed = subsongIndex >= 1 && subsongIndex <= 32 ? ((header.speedFlags >> (subsongIndex - 1)) & 1u) != 0 : ((header.speedFlags >> 31) & 1u) != 0;
        topt.maxSeconds = 60.0;
        const double wantSeconds = maxSeconds > 0.0 ? maxSeconds : 240.0;
        const double rateGuess = topt.ciaSpeed ? 200.0 : (topt.ntsc ? 60.0 : 50.2);
        auto traceResult = SidCpU6502Engine::traceSidExecutionEx(
            codeData.data(), codeData.size(),
            header.loadAddress, header.initAddress, effectivePlayAddress,
            (uint16_t)subsongIndex, (uint32_t)std::ceil(wantSeconds * rateGuess), topt
        );
        const double framesPerSecond = traceResult.framesPerSecond;
        const double clockHz = topt.ntsc ? 1022727.0 : 985248.0;
        auto traceEvents = std::move(traceResult.events);
        {
            const uint32_t lastFrame = (uint32_t)std::ceil(wantSeconds * framesPerSecond);
            while (!traceEvents.empty() && traceEvents.back().frameIndex > lastFrame)
                traceEvents.pop_back();
        }

        logOutput += "Aufgezeichnete Register-Schreibzugriffe: " + juce::String((int)traceEvents.size()) + "\n";

        if (traceEvents.empty())
        {
            logOutput += "WARNUNG: Keine Registerzugriffe erkannt.\n";
            return false;
        }

        juce::MidiFile midiFile;
        midiFile.setTicksPerQuarterNote(960);

        // 960 Ticks je Viertel bei 500000 us (120 BPM) = 1920 Ticks je Sekunde. (Früher stand hier 19.2
        // = 38.4/2: die MIDI-Datei lief dadurch doppelt so schnell wie der SID.)
        const double ticksPerFrame = 1920.0 / framesPerSecond;
        const double cyclesPerFrame = (double)SidCpU6502Engine::kCyclesPerFrame;

        // Track 0: nur noch Tempo/Titel/Autor als Meta-Events plus die Signatur (s.u.) - keine
        // feste MPE-Zonen-Deklaration mehr auf einem eigens reservierten Kanal 1: MPE-Zonen
        // müssen laut Spec direkt an den Manager-Kanal angrenzen, was sich mit 4 nach Kanal
        // aufsteigend sortierten Stufen (siehe kMidiTierChannelBase) nicht mehr sauber
        // vereinbaren lässt. Der pro Kanal gesetzte Pitch-Bend-Bereich (RPN 0,0, siehe unten)
        // bleibt die funktional relevante Einstellung.
        juce::MidiMessageSequence metaTrack;
        metaTrack.addEvent(juce::MidiMessage::tempoMetaEvent(500000), 0);
        juce::String title = juce::String(header.title).trim();
        juce::String author = juce::String(header.author).trim();
        if (title.isNotEmpty())
            metaTrack.addEvent(juce::MidiMessage::textMetaEvent(3, title), 0);
        if (author.isNotEmpty())
            metaTrack.addEvent(juce::MidiMessage::textMetaEvent(1, "von " + author), 0);
        // Sequencer-Specific-Meta-Event (0xFF 0x7F) als Signatur "SIDM"+Version 1: erlaubt
        // AudioPlayerEngine beim Laden zu erkennen, dass diese Datei die 4 Auflösungsstufen
        // (Spur 1-3/4-6/7-9/10-12, siehe unten) enthält, ohne fremde/ältere MIDI-Dateien
        // fälschlich als solche zu behandeln. Generische MIDI-Software ignoriert diesen
        // Meta-Typ spezifikationsgemäß folgenlos.
        metaTrack.addEvent(juce::MidiMessage(0xff, 0x7f, 5, 'S', 'I', 'D', 'M', 0x01), 0);

        // Voice-Tracks für die 4 Auflösungsstufen (jeweils 3 Voices), aufsteigend nach
        // Komplexität UND nach Kanalnummer angeordnet (siehe kMidiTierChannelBase oben):
        //  - Stufe 1 "vereinfacht" (Kanal 1-3): ultrakurze Noten verschmolzen, zusätzlich
        //    Arpeggios zu Akkorden zusammengefasst.
        //  - Stufe 2 "geglättet" (Kanal 4-6): wie Stufe 3, ultrakurze Nicht-Arpeggio-Noten
        //    verschmolzen, Arpeggios unangetastet.
        //  - Stufe 3 "ohne MPE/CC" (Kanal 7-9): quantisierte Noten + Program-Change, sonst nichts.
        //  - Stufe 4 "XXL" (voiceTracks, Kanal 10-12): unverändertes heutiges Verhalten mit
        //    durchgehendem Pitch-Bend-Glide (MPE-artig) und vollem CC-Vokabular.
        // Stufe 1/2/3 werden aus quantNotes/waveformChanges nach der Event-Schleife abgeleitet
        // (siehe segmentRuns()/buildTier1Notes()/buildTier2Notes()/buildPlainNoteTrack() unten).
        juce::MidiMessageSequence voiceTracks[3];
        std::vector<NoteEvent> quantNotes[3];
        std::vector<std::pair<double, int>> waveformChanges[3];
        int     activeNotes[3] = { -1, -1, -1 };
        uint8_t lastWaveform[3] = { 0xFF, 0xFF, 0xFF };
        uint8_t lastAttackNibble[3] = { 0xFF, 0xFF, 0xFF };
        uint8_t lastReleaseNibble[3] = { 0xFF, 0xFF, 0xFF };
        bool    lastRingMod[3] = { false, false, false };
        bool    lastSync[3] = { false, false, false };
        int     lastMasterVolSent = -1;

        SidChipState chip;

        // Erster von 3 Kanälen für Stufe 4 "XXL" (siehe kMidiTierChannelBase oben) - alle
        // Kanalnummern der XXL-Erzeugung leiten sich von hier ab, statt Kanal 2 hart zu kodieren.
        const int tier4ChannelBase = kMidiTierChannelBase[3];

        // Pro Voice-Kanal einmalig zu Beginn: weiter Pitch-Bend-Bereich (RPN 0,0) - nötig,
        // damit mehrsemitonige Glides/Portamento als durchgehender Bend statt als abgehackte
        // Notenfolge ankommen -, sowie Portamento an (CC5/CC65) und ein dezenter Hall-Send
        // (CC91): SID selbst hat keinen Hall-Register, das ist eine bewusste kreative Ergänzung
        // für eine angenehmere Raumwirkung bei der MIDI-Wiedergabe.
        for (int v = 0; v < 3; ++v)
        {
            int channel = tier4ChannelBase + v;
            voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, 101, 0), 0);
            voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, 100, 0), 0);
            voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, 6, kPitchBendRangeSemitones), 0);
            voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, 38, 0), 0);
            voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, 101, 127), 0);
            voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, 100, 127), 0);
            voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::PortamentoTime, 40), 0);
            voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::PortamentoSwitch, 127), 0);
            voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::ReverbSend, 32), 0);
        }

        auto tickForEvent = [&](const SidCpU6502Engine::RegEvent& evt) -> double {
            double subFrame = juce::jlimit(0.0, 0.999, (double)evt.cycleInFrame / cyclesPerFrame);
            return (double)evt.frameIndex * ticksPerFrame + subFrame * ticksPerFrame;
            };

        // Note-On/Off + Pitch-Bend für einen Voice. Der Bend wird relativ zur bereits
        // gehaltenen Note verankert (nicht relativ zu einer bei jedem Aufruf neu gerundeten
        // "nächsten" Note) - nur so ergeben mehrsemitonige Glides einen glatten statt
        // sägezahnartigen Bend. Ein neues NoteOn passiert nur bei echtem Gate-Aus->Ein
        // (activeNotes[vIdx] < 0) oder wenn der nötige Bend außerhalb des konfigurierten
        // Pitch-Bend-Bereichs läge.
        auto updateVoiceNote = [&](int vIdx, double tick) {
            auto& voice = chip.voices[vIdx];
            int channel = tier4ChannelBase + vIdx;
            bool isGate = voice.isGateActive();

            if (isGate && voice.frequency > 0)
            {
                double continuousNote;
                if (sidFreqToContinuousNote(voice.frequency, continuousNote, clockHz))
                {
                    if (activeNotes[vIdx] < 0)
                    {
                        int midiNote = juce::jlimit(0, 127, (int)std::round(continuousNote));
                        if (midiNote > 0)
                        {
                            activeNotes[vIdx] = midiNote;
                            double dev = continuousNote - (double)midiNote;
                            int bend = juce::jlimit(0, 16383, 8192 + (int)(dev * (8192.0 / kPitchBendRangeSemitones)));
                            voiceTracks[vIdx].addEvent(juce::MidiMessage::pitchWheel(channel, bend), tick);
                            voiceTracks[vIdx].addEvent(juce::MidiMessage::noteOn(channel, midiNote, (juce::uint8)100), tick);
                            quantNotes[vIdx].push_back({ tick, -1.0, midiNote });
                        }
                    }
                    else
                    {
                        double dev = continuousNote - (double)activeNotes[vIdx];
                        if (std::abs(dev) > kPitchBendRangeSemitones)
                        {
                            voiceTracks[vIdx].addEvent(juce::MidiMessage::noteOff(channel, activeNotes[vIdx]), tick);
                            closeOpenNote(quantNotes[vIdx], tick);
                            int midiNote = juce::jlimit(0, 127, (int)std::round(continuousNote));
                            activeNotes[vIdx] = midiNote;
                            dev = continuousNote - (double)midiNote;
                            int bend = juce::jlimit(0, 16383, 8192 + (int)(dev * (8192.0 / kPitchBendRangeSemitones)));
                            voiceTracks[vIdx].addEvent(juce::MidiMessage::pitchWheel(channel, bend), tick);
                            voiceTracks[vIdx].addEvent(juce::MidiMessage::noteOn(channel, midiNote, (juce::uint8)100), tick);
                            quantNotes[vIdx].push_back({ tick, -1.0, midiNote });
                        }
                        else
                        {
                            int bend = juce::jlimit(0, 16383, 8192 + (int)(dev * (8192.0 / kPitchBendRangeSemitones)));
                            voiceTracks[vIdx].addEvent(juce::MidiMessage::pitchWheel(channel, bend), tick);
                        }
                    }
                }
            }
            else if (activeNotes[vIdx] >= 0)
            {
                voiceTracks[vIdx].addEvent(juce::MidiMessage::noteOff(channel, activeNotes[vIdx]), tick);
                activeNotes[vIdx] = -1;
                closeOpenNote(quantNotes[vIdx], tick);
            }
            };

        // Voice 3 ist auf echter Hardware unhörbar, solange $D418 Bit7 gesetzt ist (häufig
        // genutzt, um Voice 3 rein als Filter-Sweep-Modulator zu missbrauchen, den der 6502-Code
        // per OSC3-Readback selbst ausliest). Beim Übergang wird eine noch klingende Note sofort
        // beendet; danach werden Note/PitchBend-Events für Kanal 4 unterdrückt, bis das Bit
        // wieder gelöscht wird (kein erzwungenes NoteOn beim Wiedereinschalten).
        auto muteVoice3IfDisconnected = [&](double tick) {
            if (chip.voice3Disconnected && activeNotes[2] >= 0)
            {
                voiceTracks[2].addEvent(juce::MidiMessage::noteOff(tier4ChannelBase + 2, activeNotes[2]), tick);
                activeNotes[2] = -1;
                closeOpenNote(quantNotes[2], tick);
            }
            };

        auto sendFilterCCsForRoutedVoices = [&](double tick) {
            int cutoffCC = juce::jlimit(0, 127, (int)(chip.filterCutoff11 >> 4));
            int resCC = sidNibbleTo7BitCC(chip.resonanceNibble);
            int modeCC = juce::jlimit(0, 127, (int)chip.filterModeBits * 127 / 7);
            for (int v = 0; v < 3; ++v)
            {
                if (!chip.isVoiceRoutedThroughFilter(v)) continue;
                int channel = tier4ChannelBase + v;
                voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::FilterCutoff, cutoffCC), tick);
                voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::Resonance, resCC), tick);
                voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::FilterMode, modeCC), tick);
            }
            };

        for (const auto& evt : traceEvents)
        {
            double tick = tickForEvent(evt);
            int voiceIdx = chip.applyRegisterEvent(evt.reg, evt.value);

            if (voiceIdx >= 0)
            {
                auto& voice = chip.voices[voiceIdx];
                int channel = tier4ChannelBase + voiceIdx;
                int regOffset = evt.reg - (voiceIdx * 7);

                if (regOffset == 2 || regOffset == 3)
                {
                    int ccVal = juce::jlimit(0, 127, (int)(voice.pulseWidth / 32));
                    voiceTracks[voiceIdx].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::PulseWidth, ccVal), tick);
                }
                else if (regOffset == 4)
                {
                    uint8_t wf = voice.getWaveform();
                    if (wf != lastWaveform[voiceIdx])
                    {
                        lastWaveform[voiceIdx] = wf;
                        int programNumber = waveformToProgramChange(wf);
                        voiceTracks[voiceIdx].addEvent(juce::MidiMessage::programChange(channel, programNumber), tick);
                        waveformChanges[voiceIdx].push_back({ tick, programNumber });
                    }

                    bool ring = voice.isRingModActive();
                    bool sync = voice.isSyncActive();
                    if (ring != lastRingMod[voiceIdx])
                    {
                        lastRingMod[voiceIdx] = ring;
                        voiceTracks[voiceIdx].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::RingModActive, ring ? 127 : 0), tick);
                    }
                    if (sync != lastSync[voiceIdx])
                    {
                        lastSync[voiceIdx] = sync;
                        voiceTracks[voiceIdx].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::SyncActive, sync ? 127 : 0), tick);
                    }
                }
                else if (regOffset == 5 || regOffset == 6)
                {
                    uint8_t atk = voice.getAttackNibble();
                    uint8_t rel = voice.getReleaseNibble();
                    if (atk != lastAttackNibble[voiceIdx])
                    {
                        lastAttackNibble[voiceIdx] = atk;
                        voiceTracks[voiceIdx].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::AttackTime, sidNibbleTo7BitCC(atk)), tick);
                    }
                    if (rel != lastReleaseNibble[voiceIdx])
                    {
                        lastReleaseNibble[voiceIdx] = rel;
                        voiceTracks[voiceIdx].addEvent(juce::MidiMessage::controllerEvent(channel, SidMidiCC::ReleaseTime, sidNibbleTo7BitCC(rel)), tick);
                    }
                }

                if (voiceIdx == 2 && chip.voice3Disconnected)
                    muteVoice3IfDisconnected(tick);
                else
                    updateVoiceNote(voiceIdx, tick);
            }
            else
            {
                // Filter-/Lautstärke-Event ($D415-$D418)
                sendFilterCCsForRoutedVoices(tick);

                if (chip.masterVolumeNibble != lastMasterVolSent)
                {
                    lastMasterVolSent = chip.masterVolumeNibble;
                    int volCC = sidNibbleTo7BitCC(chip.masterVolumeNibble);
                    for (int v = 0; v < 3; ++v)
                        voiceTracks[v].addEvent(juce::MidiMessage::controllerEvent(tier4ChannelBase + v, SidMidiCC::ChannelVolume, volCC), tick);
                }

                muteVoice3IfDisconnected(tick);
            }
        }

        // Finaler NoteOff für noch klingende Voices am tatsächlichen Ende des Traces
        // (vorher hartkodiert auf 3000 Frames, obwohl traceSidExecution je nach Song
        // deutlich mehr oder weniger Frames liefern kann).
        double finalTick = traceEvents.empty() ? 0.0 : ((double)traceEvents.back().frameIndex + 1) * ticksPerFrame;
        for (int v = 0; v < 3; ++v)
        {
            if (activeNotes[v] >= 0)
            {
                voiceTracks[v].addEvent(juce::MidiMessage::noteOff(tier4ChannelBase + v, activeNotes[v]), finalTick);
                closeOpenNote(quantNotes[v], finalTick);
            }
        }

        // Stufen 1-3 aus den mitgeführten quantisierten Noten ableiten (siehe Konstanten/
        // Hilfsfunktionen unten). Kanal-Basis je Stufe siehe kMidiTierChannelBase oben.
        juce::MidiMessageSequence tier1Tracks[3];
        juce::MidiMessageSequence tier2Tracks[3];
        juce::MidiMessageSequence tier3Tracks[3];
        for (int v = 0; v < 3; ++v)
        {
            tier1Tracks[v] = buildPlainNoteTrack(buildTier1Notes(quantNotes[v]), waveformChanges[v], kMidiTierChannelBase[0] + v);
            tier2Tracks[v] = buildPlainNoteTrack(buildTier2Notes(quantNotes[v]), waveformChanges[v], kMidiTierChannelBase[1] + v);
            tier3Tracks[v] = buildPlainNoteTrack(quantNotes[v], waveformChanges[v], kMidiTierChannelBase[2] + v);
        }

        midiFile.addTrack(metaTrack);
        for (int v = 0; v < 3; ++v) midiFile.addTrack(tier1Tracks[v]);
        for (int v = 0; v < 3; ++v) midiFile.addTrack(tier2Tracks[v]);
        for (int v = 0; v < 3; ++v) midiFile.addTrack(tier3Tracks[v]);
        for (int v = 0; v < 3; ++v) midiFile.addTrack(voiceTracks[v]);

        juce::FileOutputStream outStream(targetMidiFile);
        if (outStream.openedOk())
        {
            midiFile.writeTo(outStream);
            logOutput += "MIDI-Konvertierung erfolgreich abgeschlossen: " + targetMidiFile.getFileName() + "\n";
            logOutput += "4 Aufloesungsstufen geschrieben: Spur 1-3 (vereinfacht), 4-6 (geglaettet), "
                          "7-9 (ohne MPE/CC), 10-12 (XXL/Detail).\n";
            return true;
        }

        return false;
    }

private:
    //==============================================================================
    // Eine quantisierte (ungebendete) Note für die Stufen 1-3: startTick/endTick in MIDI-
    // Ticks, pitch als gerundete MIDI-Notennummer. endTick<0 bedeutet "noch offen" (wird
    // beim zugehörigen Gate-Aus bzw. am Trace-Ende geschlossen, siehe closeOpenNote()).
    struct NoteEvent
    {
        double startTick = 0.0;
        double endTick = -1.0;
        int    pitch = 0;
    };

    static void closeOpenNote(std::vector<NoteEvent>& notes, double tick)
    {
        if (!notes.empty() && notes.back().endTick < 0.0)
            notes.back().endTick = tick;
    }

    // Schwellwerte für die Vereinfachungsstufen 2/3 (Werte in MIDI-Ticks, 960 Ticks/Viertel
    // bei 120 BPM -> 1 Tick ~ 0.52ms; 1 SID-Frame [50Hz PAL] entspricht ~19.2 Ticks).
    // Bewusst als benannte Konstanten statt Magic Numbers, da rein empirisch/musikalisch
    // gewählt (nicht aus SID-Hardware ableitbar) und ggf. nach Hörtest nachjustiert werden.
    static constexpr double kShortNoteTicks = 96.0;           // ~50ms: Schwelle "ultrakurze" Note
    static constexpr double kMergeGapTicks = 40.0;             // ~20ms: max. Lücke fürs Verschmelzen
    static constexpr int    kArpeggioMinRunNotes = 4;          // min. Notenzahl für "Arpeggio"-Lauf
    static constexpr int    kArpeggioMinDistinctPitches = 2;   // min. Tonhöhen für "Arpeggio"-Lauf

    // Ein zusammenhängender Lauf innerhalb einer Voice-Notenliste: entweder eine einzelne
    // "normale" (nicht ultrakurze) Note, oder eine Kette ultrakurzer, lückenloser Noten -
    // Letztere gilt als Arpeggio, wenn sie lang genug ist und mehrere Tonhöhen durchläuft.
    struct NoteRun
    {
        int  firstIdx = 0;
        int  lastIdx = 0;
        bool isArpeggio = false;
    };

    static std::vector<NoteRun> segmentRuns(const std::vector<NoteEvent>& notes)
    {
        std::vector<NoteRun> runs;
        size_t i = 0;
        while (i < notes.size())
        {
            double dur = notes[i].endTick - notes[i].startTick;
            if (dur >= kShortNoteTicks)
            {
                runs.push_back({ (int)i, (int)i, false });
                ++i;
                continue;
            }

            size_t j = i;
            while (j + 1 < notes.size())
            {
                double nextDur = notes[j + 1].endTick - notes[j + 1].startTick;
                double gap = notes[j + 1].startTick - notes[j].endTick;
                if (nextDur < kShortNoteTicks && gap <= kMergeGapTicks)
                    ++j;
                else
                    break;
            }

            std::vector<int> distinctPitches;
            for (size_t k = i; k <= j; ++k)
                if (std::find(distinctPitches.begin(), distinctPitches.end(), notes[k].pitch) == distinctPitches.end())
                    distinctPitches.push_back(notes[k].pitch);

            bool isArp = ((int)(j - i + 1) >= kArpeggioMinRunNotes) && ((int)distinctPitches.size() >= kArpeggioMinDistinctPitches);
            runs.push_back({ (int)i, (int)j, isArp });
            i = j + 1;
        }
        return runs;
    }

    // Stufe 2 "geglättet": Nicht-Arpeggio-Läufe aus mehreren ultrakurzen Noten werden zu
    // einer Note verschmolzen (Tonhöhe = letzte Note des Laufs); Arpeggio-Läufe bleiben
    // unangetastet, damit bewusste Broken-Chord-Effekte nicht verloren gehen.
    static std::vector<NoteEvent> buildTier2Notes(const std::vector<NoteEvent>& src)
    {
        std::vector<NoteEvent> out;
        for (const auto& run : segmentRuns(src))
        {
            if (run.isArpeggio)
            {
                for (int k = run.firstIdx; k <= run.lastIdx; ++k)
                    out.push_back(src[(size_t)k]);
            }
            else if (run.lastIdx > run.firstIdx)
            {
                out.push_back({ src[(size_t)run.firstIdx].startTick,
                                 src[(size_t)run.lastIdx].endTick,
                                 src[(size_t)run.lastIdx].pitch });
            }
            else
            {
                out.push_back(src[(size_t)run.firstIdx]);
            }
        }
        return out;
    }

    // Stufe 1 "vereinfacht": wie Stufe 2, zusätzlich werden Arpeggio-Läufe zu einem
    // gehaltenen Akkord aufgelöst - eine Note je in dem Lauf vorkommender Tonhöhe, alle
    // gleichzeitig klingend über die Dauer des gesamten Laufs (keine Tonhöhe geht verloren).
    static std::vector<NoteEvent> buildTier1Notes(const std::vector<NoteEvent>& src)
    {
        std::vector<NoteEvent> out;
        for (const auto& run : segmentRuns(src))
        {
            if (run.isArpeggio)
            {
                std::vector<int> distinctPitches;
                for (int k = run.firstIdx; k <= run.lastIdx; ++k)
                    if (std::find(distinctPitches.begin(), distinctPitches.end(), src[(size_t)k].pitch) == distinctPitches.end())
                        distinctPitches.push_back(src[(size_t)k].pitch);

                double start = src[(size_t)run.firstIdx].startTick;
                double end = src[(size_t)run.lastIdx].endTick;
                for (int p : distinctPitches)
                    out.push_back({ start, end, p });
            }
            else if (run.lastIdx > run.firstIdx)
            {
                out.push_back({ src[(size_t)run.firstIdx].startTick,
                                 src[(size_t)run.lastIdx].endTick,
                                 src[(size_t)run.lastIdx].pitch });
            }
            else
            {
                out.push_back(src[(size_t)run.firstIdx]);
            }
        }
        return out;
    }

    // Baut aus einer Notenliste + Waveform-Programmwechseln eine reine Note-On/Off-Spur
    // ohne Pitch-Bend/MPE und ohne jegliche CC-Automation (für Stufen 1-3).
    static juce::MidiMessageSequence buildPlainNoteTrack(const std::vector<NoteEvent>& notes,
        const std::vector<std::pair<double, int>>& waveformChanges,
        int channel)
    {
        juce::MidiMessageSequence seq;
        for (const auto& n : notes)
        {
            if (n.pitch <= 0) continue;
            double endTick = n.endTick >= 0.0 ? n.endTick : n.startTick;
            if (endTick <= n.startTick) endTick = n.startTick + 1.0;
            seq.addEvent(juce::MidiMessage::noteOn(channel, n.pitch, (juce::uint8)100), n.startTick);
            seq.addEvent(juce::MidiMessage::noteOff(channel, n.pitch), endTick);
        }
        for (const auto& wc : waveformChanges)
            seq.addEvent(juce::MidiMessage::programChange(channel, wc.second), wc.first);

        seq.sort();
        return seq;
    }

    // Grobe, best-effort Zuordnung SID-Wellenform -> GM-Program, damit ein empfangender
    // MIDI-Synth zumindest eine plausible Klangfarbe wählt. Kombinierte/ungewöhnliche
    // Wellenform-Bits (auf echter Hardware kein simples Gemisch) fallen auf einen
    // Pad-Klang zurück.
    static int waveformToProgramChange(uint8_t waveformNibble)
    {
        switch (waveformNibble)
        {
        case 0x1: return 73;  // Dreieck -> Flute (GM #74)
        case 0x2: return 81;  // Sägezahn -> Lead 2 (sawtooth) (GM #82)
        case 0x4: return 80;  // Puls -> Lead 1 (square) (GM #81)
        case 0x8: return 127; // Rauschen -> Gunshot (GM #128)
        default:  return 88;  // Kombiniert/ungewöhnlich -> Pad 1 (new age) (GM #89)
        }
    }
};