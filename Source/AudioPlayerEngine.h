#pragma once

#include <JuceHeader.h>
#include "SidToMidiCore.h"
#include "SidCpu6502Engine.h"
#include <cmath>
#include <array>

//==============================================================================
/**
    AudioPlayerEngine verwaltet die Audio-Wiedergabe von SID-Spuren und MIDI-Dateien.

    "Play SID" synthetisiert den SID-Trace selbst (Oszillator/Filter/Ringmod/Hüllkurve).
    "Play MIDI" ist dagegen KEIN eigener Synth: MIDI-Dateien (egal ob von diesem Tool
    exportiert oder fremd) werden an ein echtes System-MIDI-Ausgabegerät ("Standardsynth",
    z.B. den Windows GS Wavetable Synth) gesendet - nur so klingen auch fremde MIDI-Dateien
    korrekt, die unser eigenes CC-Vokabular nicht kennen.
*/
class AudioPlayerEngine : public juce::AudioSource,
                           private juce::Timer
{
public:
    // openDevice = false: no audio device (offline rendering in the test tool via getNextAudioBlock)
    explicit AudioPlayerEngine(bool openDevice = true)
    {
        if (! openDevice) { deviceOpen = false; return; }
        auto error = audioDeviceManager.initialiseWithDefaultDevices(0, 2);
        if (error.isNotEmpty())
        {
            juce::Logger::writeToLog(juce::String(juce::CharPointer_UTF8("AudioPlayerEngine: Fehler beim Initialisieren des Audio-Ger\xc3\xa4ts: ")) + error);
        }

        audioDeviceManager.addAudioCallback(&audioSourcePlayer);
        audioSourcePlayer.setSource(this);
    }

    ~AudioPlayerEngine() override
    {
        stop();
        if (! deviceOpen) return;
        audioSourcePlayer.setSource(nullptr);
        audioDeviceManager.removeAudioCallback(&audioSourcePlayer);
    }

    void prepareToPlay(int /*samplesPerBlockExpected*/, double sampleRate) override
    {
        currentSampleRate = sampleRate;
        samplesSinceLastFrame = 0;
        currentFrameCounter = 0;
    }

    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        // Resonanz-Filterzustand kann bei leisen Decay-/Release-Ausklängen auf Denormals
        // absinken, was auf manchen CPUs sehr teuer ist.
        juce::ScopedNoDenormals noDenormals;

        // Schützt traceEvents/chip/runtime/isSidPlaying/isMidiPlaying gegen gleichzeitige
        // Änderungen durch playSidFile()/playMidiFile()/stop() auf dem Message-Thread.
        const juce::ScopedLock sl(lock);

        auto* leftBuffer = bufferToFill.buffer->getWritePointer(0, bufferToFill.startSample);
        auto* rightBuffer = bufferToFill.buffer->getNumChannels() > 1
            ? bufferToFill.buffer->getWritePointer(1, bufferToFill.startSample)
            : nullptr;

        for (int i = 0; i < bufferToFill.numSamples; ++i)
        {
            float sampleValue = 0.0f;

            if (isSidPlaying)
            {
                samplesSinceLastFrame++;
                double samplesPerFrame = currentSampleRate / framesPerSecond;

                if (samplesSinceLastFrame >= samplesPerFrame)
                {
                    samplesSinceLastFrame = 0;
                    processNextSidFrame();
                }

                sampleValue = renderNextSample();
            }
            // MIDI-Wiedergabe erzeugt hier bewusst keine eigene Audioausgabe mehr - die
            // Klangerzeugung übernimmt das externe MIDI-Ausgabegerät (siehe timerCallback()).

            if (clickLeft > 0)       // metronome click: a short, quickly fading sine burst
            {
                const float env = (float) clickLeft / (float) clickTotal;
                sampleValue += 0.18f * env * env * std::sin(clickPhase);
                clickPhase += clickStep;
                --clickLeft;
            }

            leftBuffer[i] = sampleValue;
            if (rightBuffer != nullptr)
                rightBuffer[i] = sampleValue;
        }
    }

    bool playSidFile(const juce::File& sidFile, int subsong, juce::String* log = nullptr)
    {
        stop();

        if (!sidFile.existsAsFile())
        {
            if (log) *log += "FEHLER: SID-Datei existiert nicht.\n";
            return false;
        }

        SidFileHeader header;
        std::vector<uint8_t> codeData;
        if (!SidToMidiCoreEngine::parseSidHeader(sidFile, header, codeData, log))
        {
            if (log) *log += "FEHLER: Konnte SID-Header nicht parsen.\n";
            return false;
        }

        uint16_t effectivePlayAddress = (header.playAddress != 0) ? header.playAddress : header.initAddress;

        if (log) *log += "[PLAY] Starte SID Emulation/Trace (Subsong " + juce::String(subsong) + ")...\n";

        // Die Emulation läuft absichtlich außerhalb des Locks (kann laut
        // traceSidExecution bis zu ~4 Sekunden dauern) - andernfalls würde der
        // Audio-Thread im getNextAudioBlock()-Lock solange blockiert.
        auto newTraceEvents = SidCpU6502Engine::traceSidExecution(
            codeData.data(), codeData.size(),
            header.loadAddress, header.initAddress, effectivePlayAddress,
            (uint16_t)subsong, 30000
        );

        if (newTraceEvents.empty())
        {
            if (log) *log += "WARNUNG: Keine Register-Events erhalten!\n";
            return false;
        }

        if (log)
            *log += "Aufgezeichnete Register-Schreibzugriffe: " + juce::String((int)newTraceEvents.size()) + "\n";

        {
            const juce::ScopedLock sl(lock);

            traceEvents = std::move(newTraceEvents);
            chip = SidChipState{};
            for (int i = 0; i < 3; ++i)
            {
                runtime[i] = VoiceRuntime{};
                oscHistory[i].fill(0.0f);
                oscHistoryPos[i] = 0;
            }
            filter.reset();

            currentEventIndex = 0;
            currentFrameCounter = 0;
            samplesSinceLastFrame = 0;
            isSidPlaying = true;
            isMidiPlaying = false;
        }
        return true;
    }

    //==============================================================================
    // Plays a precomputed trace (SidTrace) starting at startFrame. The tune ends after totalFrames.
    bool playTrace(const std::vector<SidCpU6502Engine::RegEvent>& events, double fps, uint32_t totalFrames, uint32_t startFrame = 0, bool ntsc = false)
    {
        stop();
        if (events.empty()) return false;
        const juce::ScopedLock sl(lock);
        traceEvents = events;
        sidClockHz = ntsc ? 1022727.0f : 985248.0f;
        framesPerSecond = juce::jlimit(10.0, 400.0, fps);
        traceTotalFrames = juce::jmax(1u, totalFrames);
        traceEndFrame = 0;
        traceLoop = false;
        metroFrames = 0;
        sidFinished = false;
        isSidPlaying = true;
        isMidiPlaying = false;
        seekLocked(startFrame);
        return true;
    }

    // Jumps to a frame of the running (or paused) SID tune: the chip state is rebuilt from the register writes.
    void seekFrame(uint32_t frame)
    {
        const juce::ScopedLock sl(lock);
        if (traceTotalFrames == 0) return;
        seekLocked(frame);
    }

    void setVoiceMuted(int voice, bool muted)
    {
        if (voice < 0 || voice > 2) return;
        int m = voiceMuteMask.load();
        m = muted ? (m | (1 << voice)) : (m & ~(1 << voice));
        voiceMuteMask = m;
    }
    void setOutputGain(float gain) { outputGain = juce::jlimit(0.0f, 2.0f, gain); }

    uint32_t getPlayFrame() const { const juce::ScopedLock sl(lock); return (uint32_t) currentFrameCounter; }
    double   getFramesPerSecond() const { const juce::ScopedLock sl(lock); return framesPerSecond; }
    bool     isSidTunePlaying() const { const juce::ScopedLock sl(lock); return isSidPlaying; }
    // true once after a tune played to its end (then reset)
    bool     takeSidFinished() { const juce::ScopedLock sl(lock); const bool f = sidFinished; sidFinished = false; return f; }
    // where the tune ends (0 = at the end of the trace) and whether it then starts again at once, inside the audio thread
    // and without a pause (repeat): the song length of the HVSC is the loop point
    // metronome for the tracker: a click on every beat (every framesPerBeat frames), the first of beatsPerBar louder;
    // startBeat / offsetFrames place the beat grid when the playback does not start on a beat
    void     setMetronome(uint32_t framesPerBeat, int beatsPerBar, int startBeat, uint32_t offsetFrames)
    {
        const juce::ScopedLock sl(lock);
        metroFrames = framesPerBeat; metroBar = juce::jmax(1, beatsPerBar); metroStartBeat = startBeat; metroOffset = offsetFrames;
    }
    void     setSidPlayRange(uint32_t endFrame, bool loop) { const juce::ScopedLock sl(lock); traceEndFrame = endFrame; traceLoop = loop; }
    // pause/resume of a SID tune started with playTrace()
    void     setSidPaused(bool paused) { const juce::ScopedLock sl(lock); if (traceTotalFrames > 0) isSidPlaying = ! paused; }
    bool     hasSidTrace() const { const juce::ScopedLock sl(lock); return traceTotalFrames > 0 && ! traceEvents.empty(); }

    bool playMidiFile(const juce::File& midiFile, juce::String* log = nullptr)
    {
        stop();
        if (!midiFile.existsAsFile()) return false;

        juce::FileInputStream inStream(midiFile);
        if (!inStream.openedOk()) return false;

        juce::MidiFile parsedFile;
        if (!parsedFile.readFrom(inStream)) return false;

        // Erkennt die von SidToMidiCoreEngine::convertSidToMidiFile() erzeugte
        // Sequencer-Specific-Signatur "SIDM" (0xFF 0x7F) auf Track 0: nur bei einem Treffer
        // enthält die Datei die 4 Auflösungsstufen (Kanal 2-4/5-7/8-10/11-13) und wird
        // gefiltert nach aktiver Stufe wiedergegeben; jede andere/fremde MIDI-Datei spielt
        // wie bisher unverändert alle Kanäle.
        bool detectedMultiRes = false;
        if (auto* track0 = parsedFile.getTrack(0))
        {
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
                    {
                        detectedMultiRes = true;
                        break;
                    }
                }
            }
        }

        parsedFile.convertTimestampTicksToSeconds();

        juce::MidiMessageSequence merged;
        for (int t = 0; t < parsedFile.getNumTracks(); ++t)
            if (auto* track = parsedFile.getTrack(t))
                merged.addSequence(*track, 0.0);
        merged.sort();

        // Nur echte Kanal-/Voice-Messages (Note/CC/PitchBend/ProgramChange/...) werden je
        // an das externe MIDI-Ausgabegerät geschickt - Meta-Events (Tempo/Text/End-of-Track)
        // und SysEx sind keine gültigen Kurznachrichten für ein Hardware-MIDI-Ausgabegerät.
        // Tempo steckt bereits korrekt in den Zeitstempeln (convertTimestampTicksToSeconds()
        // oben), wird also nicht separat gebraucht. Von Menschen/DAWs "humanisierte" Dateien
        // können hunderte Tempo-Meta-Events enthalten (z.B. mikro-Temposchwankungen) - jedes
        // davon unmodifiziert an sendMessageNow() durchzureichen lässt JUCEs interne Prüfung
        // für gültige Kurznachrichten wiederholt anschlagen (im Debugger sichtbar als
        // Haltepunkt-Serie während der Wiedergabe).
        std::vector<DecodedMidiEvent> newMidiEvents;
        newMidiEvents.reserve((size_t)merged.getNumEvents());
        for (int i = 0; i < merged.getNumEvents(); ++i)
        {
            auto* ev = merged.getEventPointer(i);
            if (ev != nullptr && !ev->message.isMetaEvent() && !ev->message.isSysEx())
                newMidiEvents.push_back({ ev->message.getTimeStamp(), ev->message });
        }

        if (newMidiEvents.empty()) return false;

        // Standard-MIDI-Ausgabegerät des Systems öffnen (z.B. "Microsoft GS Wavetable
        // Synth" unter Windows) - nur so spielen auch fremde MIDI-Dateien korrekt, die
        // unser eigenes CC-Vokabular nicht kennen. Bleibt über mehrere Wiedergaben offen.
        if (midiOutputDevice == nullptr)
        {
            auto devices = juce::MidiOutput::getAvailableDevices();
            if (!devices.isEmpty())
                midiOutputDevice = juce::MidiOutput::openDevice(devices[0].identifier);
        }
        if (midiOutputDevice == nullptr)
        {
            if (log) *log += juce::String(juce::CharPointer_UTF8("FEHLER: Kein MIDI-Ausgabeger\xc3\xa4t (Standardsynth) auf diesem System gefunden.\n"));
            return false;
        }

        {
            const juce::ScopedLock sl(lock);
            midiEvents = std::move(newMidiEvents);
            midiEventIndex = 0;
            isMultiResSidMidi = detectedMultiRes;
            currentTierIndex = 0; // immer bei Stufe 1 (Spur 1-3, vereinfacht) beginnen
            for (int v = 0; v < 3; ++v)
            {
                midiVisualGate[v] = false;
                midiVisualEnvelope[v] = 0.0f;
                midiVisualNote[v] = -1;
                midiPitchHistory[v].fill(0.0f);
                midiPitchHistoryPos[v] = 0;
            }
            isMidiPlaying = true;
            isSidPlaying = false;
        }

        if (detectedMultiRes && log != nullptr)
        {
            *log += "[MIDI] SIDIMIDI Mehrfach-Aufloesungs-Datei erkannt. Tasten 1-4 wechseln waehrend "
                    "der Wiedergabe zwischen den Stufen (Start: Stufe 1 - " + juce::String(tierDisplayName(0)) + ").\n";
        }

        midiPlaybackStartMs = juce::Time::getMillisecondCounterHiRes();
        startTimer(5); // Dispatch-Takt an das MIDI-Ausgabegerät (Message-Thread, nicht audio-kritisch)
        return true;
    }

    // Für die manuelle "MIDI-SET auswählen"-Stimmenvorschau (siehe MainComponent): spielt NUR
    // die Ereignisse einer einzelnen MIDI-Spur ab (unabhängig von isMultiResSidMidi/Tier-
    // Logik), damit der Nutzer vor der endgültigen Spurauswahl einzeln hineinhören kann. Wird
    // in der Voice-Anzeige immer auf Voice 1 (Index 0) gemappt.
    //
    // Spur- statt Kanal-basiert, da z.B. Studio-One-Exporte jede Spur als eigenen MTrk-Chunk
    // anlegen, aber meist alle auf Kanal 1 belassen (siehe MidiToSidCoreEngine::listAllTracks).
    bool previewMidiTrack(const juce::File& midiFile, int trackIndex, juce::String* log = nullptr, int channelFilter = 0)
    {
        stop();
        if (!midiFile.existsAsFile() || trackIndex < 0) return false;

        juce::FileInputStream inStream(midiFile);
        if (!inStream.openedOk()) return false;

        juce::MidiFile parsedFile;
        if (!parsedFile.readFrom(inStream)) return false;

        parsedFile.convertTimestampTicksToSeconds();

        auto* track = (trackIndex < parsedFile.getNumTracks()) ? parsedFile.getTrack(trackIndex) : nullptr;
        if (track == nullptr) return false;

        std::vector<DecodedMidiEvent> newMidiEvents;
        newMidiEvents.reserve((size_t)track->getNumEvents());
        for (int i = 0; i < track->getNumEvents(); ++i)
        {
            auto* ev = track->getEventPointer(i);
            if (ev != nullptr && !ev->message.isMetaEvent() && !ev->message.isSysEx()
                && (channelFilter == 0 || ev->message.getChannel() == channelFilter))
                newMidiEvents.push_back({ ev->message.getTimeStamp(), ev->message });
        }

        if (newMidiEvents.empty())
        {
            if (log) *log += "FEHLER: Spur " + juce::String(trackIndex) + juce::String(juce::CharPointer_UTF8(" enth\xc3\xa4lt keine Ereignisse.\n"));
            return false;
        }

        if (midiOutputDevice == nullptr)
        {
            auto devices = juce::MidiOutput::getAvailableDevices();
            if (!devices.isEmpty())
                midiOutputDevice = juce::MidiOutput::openDevice(devices[0].identifier);
        }
        if (midiOutputDevice == nullptr)
        {
            if (log) *log += juce::String(juce::CharPointer_UTF8("FEHLER: Kein MIDI-Ausgabeger\xc3\xa4t (Standardsynth) auf diesem System gefunden.\n"));
            return false;
        }

        {
            const juce::ScopedLock sl(lock);
            midiEvents = std::move(newMidiEvents);
            midiEventIndex = 0;
            isMultiResSidMidi = false;
            currentTierIndex = 0;
            previewTrackIndex = trackIndex;
            for (int v = 0; v < 3; ++v)
            {
                midiVisualGate[v] = false;
                midiVisualEnvelope[v] = 0.0f;
                midiVisualNote[v] = -1;
                midiPitchHistory[v].fill(0.0f);
                midiPitchHistoryPos[v] = 0;
            }
            isMidiPlaying = true;
            isSidPlaying = false;
        }

        midiPlaybackStartMs = juce::Time::getMillisecondCounterHiRes();
        startTimer(5);
        return true;
    }

    // Preview of several MIDI tracks at once (track picker): the tracks can be switched on and off while the
    // playback goes on - the time continues where it is. slot = the oscillator display (0-2) that follows the track.
    struct PreviewTrack { int trackIndex = 0; int channelFilter = 0; int slot = 0; };

    bool setPreviewTracks(const juce::File& midiFile, const std::vector<PreviewTrack>& tracks)
    {
        if (tracks.empty()) { stop(); return true; }

        double elapsed = 0.0;
        bool continuing = false;
        {
            const juce::ScopedLock sl(lock);
            continuing = isMidiPlaying && previewTrackIndex >= 0;
            if (continuing) elapsed = (juce::Time::getMillisecondCounterHiRes() - midiPlaybackStartMs) / 1000.0;
        }

        juce::FileInputStream inStream(midiFile);
        if (!inStream.openedOk()) return false;
        juce::MidiFile parsedFile;
        if (!parsedFile.readFrom(inStream)) return false;
        parsedFile.convertTimestampTicksToSeconds();

        std::vector<DecodedMidiEvent> newEvents;
        for (const auto& pt : tracks)
        {
            auto* track = (pt.trackIndex >= 0 && pt.trackIndex < parsedFile.getNumTracks()) ? parsedFile.getTrack(pt.trackIndex) : nullptr;
            if (track == nullptr) continue;
            for (int i = 0; i < track->getNumEvents(); ++i)
            {
                auto* ev = track->getEventPointer(i);
                if (ev != nullptr && !ev->message.isMetaEvent() && !ev->message.isSysEx()
                    && (pt.channelFilter == 0 || ev->message.getChannel() == pt.channelFilter))
                    newEvents.push_back({ ev->message.getTimeStamp(), ev->message, juce::jlimit(0, 2, pt.slot) });
            }
        }
        if (newEvents.empty()) return false;
        std::stable_sort(newEvents.begin(), newEvents.end(), [](const DecodedMidiEvent& a, const DecodedMidiEvent& b) { return a.timeSeconds < b.timeSeconds; });

        if (!continuing) stop();
        if (midiOutputDevice == nullptr)
        {
            auto devices = juce::MidiOutput::getAvailableDevices();
            if (!devices.isEmpty())
                midiOutputDevice = juce::MidiOutput::openDevice(devices[0].identifier);
        }
        if (midiOutputDevice == nullptr) return false;

        {
            const juce::ScopedLock sl(lock);
            if (continuing)       // notes that sound now stop; program changes and controllers up to now are sent again so that a new track has its sound
                for (int ch = 1; ch <= 16; ++ch)
                    midiOutputDevice->sendMessageNow(juce::MidiMessage::allNotesOff(ch));
            size_t idx = 0;
            while (idx < newEvents.size() && newEvents[idx].timeSeconds <= elapsed)
            {
                const auto& m = newEvents[idx].message;
                if (!m.isNoteOnOrOff()) midiOutputDevice->sendMessageNow(m);
                ++idx;
            }
            midiEvents = std::move(newEvents);
            midiEventIndex = idx;
            isMultiResSidMidi = false;
            currentTierIndex = 0;
            previewTrackIndex = tracks.front().trackIndex;
            if (!continuing)
                for (int v = 0; v < 3; ++v)
                {
                    midiVisualGate[v] = false; midiVisualEnvelope[v] = 0.0f; midiVisualNote[v] = -1;
                    midiPitchHistory[v].fill(0.0f); midiPitchHistoryPos[v] = 0;
                }
            isMidiPlaying = true;
            isSidPlaying = false;
            midiPlaybackStartMs = juce::Time::getMillisecondCounterHiRes() - elapsed * 1000.0;
        }
        startTimer(5);
        return true;
    }

    // seconds into the running MIDI playback (0 when nothing plays)
    double getMidiElapsedSeconds() const
    {
        const juce::ScopedLock sl(lock);
        return isMidiPlaying ? (juce::Time::getMillisecondCounterHiRes() - midiPlaybackStartMs) / 1000.0 : 0.0;
    }

    //==============================================================================
    // Anzahl/Kanal-Layout der Auflösungsstufen einer per convertSidToMidiFile() erzeugten
    // Mehrstufen-MIDI-Datei. Bewusst dieselben Konstanten wie im Erzeuger (kMidiTierChannelBase
    // in SidToMidiCore.h) statt einer eigenen Kopie - zwei unabhängig gepflegte Kanal-Tabellen
    // sind genau die Fehlerquelle, die zu einem Auseinanderlaufen von Erzeugung und Wiedergabe
    // führen kann.
    static constexpr int kNumTiers = kNumMidiTiers;
    static constexpr const int* kTierChannelBase = kMidiTierChannelBase;

    static const char* tierDisplayName(int idx)
    {
        static const char* names[kNumTiers] = {
            "Spur 1-3 (vereinfacht)",
            "Spur 4-6 (geglaettet)",
            "Spur 7-9 (ohne MPE/CC)",
            "Spur 10-12 (XXL/Detail)"
        };
        return (idx >= 0 && idx < kNumTiers) ? names[idx] : "?";
    }

    bool isCurrentFileMultiRes() const
    {
        const juce::ScopedLock sl(lock);
        return isMultiResSidMidi;
    }

    int getActiveTierIndex() const
    {
        const juce::ScopedLock sl(lock);
        return currentTierIndex;
    }

    // Wechselt bei laufender Mehrstufen-MIDI-Wiedergabe zur angegebenen Stufe (0-3). Beendet
    // hängende Noten der bisherigen Stufe am externen Synth und rekonstruiert für die neue
    // Stufe, welche Noten an der aktuellen Abspielposition bereits klingen sollten (sonst
    // bliebe die neue Stufe nach dem Umschalten bis zum nächsten Event stumm). Rückgabe false
    // bei fehlender/ungültiger Wiedergabe oder wenn die Stufe bereits aktiv ist.
    bool setActiveTier(int newTierIdx)
    {
        const juce::ScopedLock sl(lock);
        if (!isMidiPlaying || !isMultiResSidMidi) return false;
        if (newTierIdx < 0 || newTierIdx >= kNumTiers) return false;
        if (newTierIdx == currentTierIndex) return false;

        if (midiOutputDevice != nullptr)
        {
            int oldBase = kTierChannelBase[currentTierIndex];
            for (int c = oldBase; c < oldBase + 3; ++c)
                midiOutputDevice->sendMessageNow(juce::MidiMessage::allNotesOff(c));
        }

        int newBase = kTierChannelBase[newTierIdx];
        double elapsedSeconds = (juce::Time::getMillisecondCounterHiRes() - midiPlaybackStartMs) / 1000.0;

        int heldNote[3] = { -1, -1, -1 };
        for (size_t i = 0; i < midiEvents.size() && midiEvents[i].timeSeconds <= elapsedSeconds; ++i)
        {
            const auto& msg = midiEvents[i].message;
            int v = msg.getChannel() - newBase;
            if (v < 0 || v > 2) continue;
            if (msg.isNoteOn()) heldNote[v] = msg.getNoteNumber();
            else if (msg.isNoteOff()) heldNote[v] = -1;
        }

        currentTierIndex = newTierIdx;
        for (int v = 0; v < 3; ++v)
        {
            if (heldNote[v] >= 0)
            {
                if (midiOutputDevice != nullptr)
                    midiOutputDevice->sendMessageNow(juce::MidiMessage::noteOn(newBase + v, heldNote[v], (juce::uint8)100));
                midiVisualGate[v] = true;
                midiVisualEnvelope[v] = 1.0f;
                midiVisualNote[v] = heldNote[v];
            }
            else
            {
                midiVisualGate[v] = false;
                midiVisualNote[v] = -1;
            }
        }
        return true;
    }

    void stop()
    {
        stopTimer();

        if (midiOutputDevice != nullptr)
        {
            // Hängende Noten am externen Synth beenden, bevor gestoppt/gewechselt wird.
            for (int ch = 1; ch <= 16; ++ch)
                midiOutputDevice->sendMessageNow(juce::MidiMessage::allNotesOff(ch));
        }

        const juce::ScopedLock sl(lock);

        isSidPlaying = false;
        isMidiPlaying = false;
        currentEventIndex = 0;
        currentFrameCounter = 0;
        traceEvents.clear();
        traceTotalFrames = 0;
        traceEndFrame = 0;
        traceLoop = false;
        metroFrames = 0;
        framesPerSecond = 50.125;

        midiEvents.clear();
        midiEventIndex = 0;
        isMultiResSidMidi = false;
        currentTierIndex = 0;
        previewTrackIndex = -1;

        chip = SidChipState{};
        for (int i = 0; i < 3; ++i)
        {
            runtime[i] = VoiceRuntime{};
            oscHistory[i].fill(0.0f);
            oscHistoryPos[i] = 0;
            midiVisualGate[i] = false;
            midiVisualEnvelope[i] = 0.0f;
            midiVisualNote[i] = -1;
            midiPitchHistory[i].fill(0.0f);
            midiPitchHistoryPos[i] = 0;
        }
        filter.reset();
    }

    bool isPlaying() const
    {
        const juce::ScopedLock sl(lock);
        return isSidPlaying || isMidiPlaying;
    }

    // Härterer Reset für den Fall, dass ein normales stop() nicht ausreicht, um einen
    // "hängengebliebenen" Zustand zu lösen (z.B. hängende Noten an einem externen MIDI-Gerät,
    // dessen zwischengespeichertes Geräte-Handle selbst angeschlagen ist, oder ein Audiogerät,
    // das durch einen fehlerhaften SID-Trace in einem klemmenden Zustand hängt). Per Doppelklick
    // auf "Stop" ausgelöst, siehe MainComponent::mouseDoubleClick().
    void hardReset()
    {
        stop();

        // Alle verfügbaren MIDI-Ausgabegeräte (nicht nur das intern gemerkte Handle) noch
        // einmal explizit stummschalten, für den Fall, dass genau dieses gemerkte Handle
        // selbst im klemmenden Zustand steckt.
        for (auto& deviceInfo : juce::MidiOutput::getAvailableDevices())
            if (auto dev = juce::MidiOutput::openDevice(deviceInfo.identifier))
                for (int ch = 1; ch <= 16; ++ch)
                    dev->sendMessageNow(juce::MidiMessage::allNotesOff(ch));

        {
            // Gemerktes Geräte-Handle verwerfen, damit die nächste MIDI-Wiedergabe es frisch
            // neu öffnet, statt ein eventuell angeschlagenes Handle weiterzuverwenden.
            const juce::ScopedLock sl(lock);
            midiOutputDevice.reset();
        }

        // Audiogerät hart neu starten, falls der Audio-Callback selbst hängt.
        audioDeviceManager.closeAudioDevice();
        audioDeviceManager.restartLastAudioDevice();
    }

    //==============================================================================
    // Für die Voice-1-3-Anzeige in der Oberfläche: bei SID-Wiedergabe die tatsächlichen
    // Oszillator-Samples + Hüllkurve aus der internen Synthese; bei MIDI-Wiedergabe eine
    // rein visuelle Gate-Hüllkurve (der tatsächliche Klang kommt vom externen MIDI-Gerät,
    // wir haben dafür also keine echte Hüllkurve - nur Note an/aus wird nachverfolgt).
    static constexpr int kOscHistorySize = 200;
    struct VoiceDisplayInfo
    {
        bool active = false;
        bool isMidiMode = false;
        float envelopeLevel = 0.0f;
        std::array<float, kOscHistorySize> oscHistory{};
        int oscHistoryWritePos = 0;
    };

    void getVoiceDisplaySnapshot(VoiceDisplayInfo (&out)[3]) const
    {
        const juce::ScopedLock sl(lock);
        for (int i = 0; i < 3; ++i)
        {
            out[i].isMidiMode = isMidiPlaying;
            if (isMidiPlaying)
            {
                out[i].active = midiVisualGate[i] || midiVisualEnvelope[i] > 0.001f;
                out[i].envelopeLevel = midiVisualEnvelope[i];
                // Bei MIDI-Wiedergabe zeigt oscHistory keine echte Wellenform (der Klang kommt
                // vom externen Gerät), sondern eine Tonhöhen-Verlaufslinie - optisch bewusst
                // dieselbe grüne Linie wie beim SID-Oszillator (siehe VoiceDisplay::paint()).
                out[i].oscHistory = midiPitchHistory[i];
                out[i].oscHistoryWritePos = midiPitchHistoryPos[i];
            }
            else
            {
                out[i].active = isSidPlaying;
                out[i].envelopeLevel = runtime[i].envelopeLevel;
                out[i].oscHistory = oscHistory[i];
                out[i].oscHistoryWritePos = oscHistoryPos[i];
            }
        }
    }

private:
    //==============================================================================
    // Laufzeitzustand pro Voice (nur für "Play SID"): Oszillatorphase, Hüllkurve,
    // Rauschgenerator, sowie das "hat im vorherigen Sample gewrappt"-Flag, das die
    // zirkuläre Modulator-Abhängigkeit bei Hard-Sync auflöst (entspricht dem tatsächlichen
    // Hardware-Verhalten: jede Voice sieht den Modulator-Wrap einen Sample-Takt später).
    struct VoiceRuntime
    {
        double phase = 0.0;
        float  envelopeLevel = 0.0f;
        enum class EnvPhase { Idle, Attack, Decay, Sustain, Release } envPhase = EnvPhase::Idle;
        uint32_t noiseLfsr = 0x7ffff8u;
        float  noiseHold = 0.0f;
        bool   prevWrapped = false;
    };

    // Handgeschriebenes trapezoidal-integriertes (TPT/Zavalishin) State-Variable-Filter -
    // unconditionally stable, kein zusätzliches JUCE-Modul nötig (juce_dsp ist aktuell nicht
    // im Projekt eingebunden). Deckt Low-/Band-/High-Pass ab, kombinierbar wie die SID-Modusbits.
    struct TptSvf
    {
        float ic1eq = 0.0f, ic2eq = 0.0f;

        void reset() { ic1eq = ic2eq = 0.0f; }

        void process(float input, float g, float k, float& low, float& band, float& high)
        {
            float a1 = 1.0f / (1.0f + g * (g + k));
            float a2 = g * a1;
            float a3 = g * a2;
            float v3 = input - ic2eq;
            float v1 = a1 * ic1eq + a2 * v3;
            float v2 = ic2eq + a2 * ic1eq + a3 * v3;
            ic1eq = 2.0f * v1 - ic1eq;
            ic2eq = 2.0f * v2 - ic2eq;
            low = v2;
            band = v1;
            high = input - k * v1 - v2;
        }
    };

    struct DecodedMidiEvent
    {
        double timeSeconds = 0.0;
        juce::MidiMessage message;
        int slot = 0;                  // which of the three displays shows the event (track preview)
    };

    // Applies a register write of the trace. A gate that goes 0 -> 1 restarts the envelope even when the
    // tune switches the gate off and on again inside one frame (hard restart, e.g. "Testcard"): the sample
    // renderer only sees the state at the end of the frame and would otherwise never notice the new note.
    void applyTraceEvent(const SidCpU6502Engine::RegEvent& e)
    {
        int vi = -1;
        if (e.reg <= 0x06) vi = 0;
        else if (e.reg >= 0x07 && e.reg <= 0x0D) vi = 1;
        else if (e.reg >= 0x0E && e.reg <= 0x14) vi = 2;
        const bool control = vi >= 0 && (e.reg - vi * 7) == 4;
        const bool wasGate = control && chip.voices[vi].isGateActive();
        chip.applyRegisterEvent(e.reg, e.value);
        if (control && ! wasGate && chip.voices[vi].isGateActive())
            runtime[vi].envPhase = VoiceRuntime::EnvPhase::Attack;
    }

    void seekLocked(uint32_t frame)
    {
        chip = SidChipState{};
        for (int i = 0; i < 3; ++i)
        {
            runtime[i] = VoiceRuntime{};
            oscHistory[i].fill(0.0f);
            oscHistoryPos[i] = 0;
        }
        filter.reset();
        currentEventIndex = 0;
        while (currentEventIndex < traceEvents.size() && traceEvents[currentEventIndex].frameIndex <= frame)
        {
            applyTraceEvent(traceEvents[currentEventIndex]);
            ++currentEventIndex;
        }
        currentFrameCounter = frame;
        samplesSinceLastFrame = 0;
    }

    //==============================================================================
    void processNextSidFrame()
    {
        if (metroFrames > 0 && ((uint32_t) currentFrameCounter + metroOffset) % metroFrames == 0)
        {
            const int beat = (int) (((uint32_t) currentFrameCounter + metroOffset) / metroFrames) + metroStartBeat;
            const bool accent = beat % metroBar == 0;
            clickTotal = clickLeft = (int) (currentSampleRate * 0.035);
            clickPhase = 0.0f;
            clickStep = (float) (2.0 * juce::MathConstants<double>::pi * (accent ? 1800.0 : 1100.0) / currentSampleRate);
        }
        currentFrameCounter++;

        while (currentEventIndex < traceEvents.size() && traceEvents[currentEventIndex].frameIndex <= currentFrameCounter)
        {
            const auto& evt = traceEvents[currentEventIndex];
            applyTraceEvent(evt);
            currentEventIndex++;
        }

        if (traceTotalFrames > 0)
        {
            // a precomputed trace (playTrace): the tune ends at its known length
            const uint32_t endFrame = traceEndFrame > 0 ? juce::jmin(traceEndFrame, traceTotalFrames) : traceTotalFrames;
            if ((uint32_t) currentFrameCounter >= endFrame)
            {
                if (traceLoop) seekLocked(0);               // repeat: straight back to the beginning
                else
                {
                    isSidPlaying = false;
                    sidFinished = true;
                }
            }
        }
        else if (currentEventIndex >= traceEvents.size())
        {
            currentEventIndex = 0;
            currentFrameCounter = 0;
        }
    }

    // MIDI-Dispatch an das externe Ausgabegerät. Läuft als JUCE-Timer auf dem Message-Thread,
    // absichtlich NICHT im Audio-Callback (MidiOutput::sendMessageNow ist nicht realtime-sicher).
    void timerCallback() override
    {
        const juce::ScopedLock sl(lock);

        if (!isMidiPlaying || midiOutputDevice == nullptr)
        {
            stopTimer();
            return;
        }

        double elapsedSeconds = (juce::Time::getMillisecondCounterHiRes() - midiPlaybackStartMs) / 1000.0;

        // Bei einer erkannten Mehrstufen-Datei (isMultiResSidMidi) enthält die Sequenz alle 4
        // Auflösungsstufen gleichzeitig (Kanal 2-13) - nur die Kanäle der aktuell aktiven Stufe
        // (kTierChannelBase[currentTierIndex]..+3) werden an den externen Synth geschickt und in
        // der Voice-Anzeige berücksichtigt; Events anderer Stufen werden übersprungen, ohne die
        // Zeitleiste zu blockieren. Bei einer fremden/alten Datei (nicht erkannt) bleibt das
        // Verhalten unverändert: alle Kanäle werden gespielt, Voice-Zuordnung über Kanal 2-4.
        int activeChannelBase = isMultiResSidMidi ? kTierChannelBase[currentTierIndex] : 2;

        while (midiEventIndex < midiEvents.size() && midiEvents[midiEventIndex].timeSeconds <= elapsedSeconds)
        {
            const auto& msg = midiEvents[midiEventIndex].message;
            int channel = msg.getChannel();

            // Einzelspur-Vorschau (previewMidiTrack): midiEvents enthält bereits nur die
            // vorgehörte Spur - immer als Voice 1 (Index 0) anzeigen, unabhängig von der
            // sonstigen Tier-/Kanalbasis-Logik.
            bool passThrough = previewTrackIndex >= 0
                ? true
                : (!isMultiResSidMidi || (channel >= activeChannelBase && channel < activeChannelBase + 3));
            int v = previewTrackIndex >= 0 ? midiEvents[midiEventIndex].slot : (channel - activeChannelBase);

            if (passThrough)
            {
                midiOutputDevice->sendMessageNow(msg);

                if (v >= 0 && v <= 2)
                {
                    if (msg.isNoteOn()) { midiVisualGate[v] = true; midiVisualEnvelope[v] = 1.0f; midiVisualNote[v] = msg.getNoteNumber(); }
                    else if (msg.isNoteOff()) midiVisualGate[v] = false;
                }
            }
            midiEventIndex++;
        }

        for (int v = 0; v < 3; ++v)
        {
            if (midiVisualGate[v])
                midiVisualEnvelope[v] = juce::jmin(1.0f, midiVisualEnvelope[v] + 0.25f);
            else
                midiVisualEnvelope[v] = juce::jmax(0.0f, midiVisualEnvelope[v] - 0.02f);

            // Tonhöhen-Verlaufspuffer für die Voice-Anzeige (analog zu oscHistory bei SID-
            // Wiedergabe): normiert MIDI-Note 24-108 auf -1..1, geklammert; ohne aktive Note
            // (Pause) wird 0 geschrieben -> ruhige Mittellinie wie beim SID im Leerlauf.
            float norm = 0.0f;
            if (midiVisualGate[v] && midiVisualNote[v] >= 0)
                norm = juce::jlimit(-1.0f, 1.0f, juce::jmap((float)midiVisualNote[v], 24.0f, 108.0f, -1.0f, 1.0f));
            midiPitchHistory[v][(size_t)midiPitchHistoryPos[v]] = norm;
            midiPitchHistoryPos[v] = (midiPitchHistoryPos[v] + 1) % kOscHistorySize;
        }

        if (midiEventIndex >= midiEvents.size())
        {
            stopTimer();
            isMidiPlaying = false;
        }
    }

    float voiceFrequencyHz(int v) const
    {
        return chip.voices[v].frequency * (sidClockHz / 16777216.0f);
    }

    void updateEnvelope(VoiceRuntime& rt, const SIDVoiceData& voice)
    {
        static const float attackMs[16] = { 2,8,16,24,38,56,68,80,100,250,500,800,1000,3000,5000,8000 };
        static const float decayReleaseMs[16] = { 6,24,48,72,114,168,204,240,300,750,1500,2400,3000,9000,15000,24000 };

        bool gate = voice.isGateActive();

        if (gate)
        {
            if (rt.envPhase == VoiceRuntime::EnvPhase::Idle || rt.envPhase == VoiceRuntime::EnvPhase::Release)
                rt.envPhase = VoiceRuntime::EnvPhase::Attack;

            if (rt.envPhase == VoiceRuntime::EnvPhase::Attack)
            {
                rt.envelopeLevel += (float)(1000.0 / ((double)attackMs[voice.getAttackNibble()] * currentSampleRate));
                if (rt.envelopeLevel >= 1.0f)
                {
                    rt.envelopeLevel = 1.0f;
                    rt.envPhase = VoiceRuntime::EnvPhase::Decay;
                }
            }
            else if (rt.envPhase == VoiceRuntime::EnvPhase::Decay)
            {
                float sustainLevel = voice.getSustainNibble() / 15.0f;
                rt.envelopeLevel -= (float)(1000.0 / ((double)decayReleaseMs[voice.getDecayNibble()] * currentSampleRate));
                if (rt.envelopeLevel <= sustainLevel)
                {
                    rt.envelopeLevel = sustainLevel;
                    rt.envPhase = VoiceRuntime::EnvPhase::Sustain;
                }
            }
            else
            {
                rt.envelopeLevel = voice.getSustainNibble() / 15.0f;
            }
        }
        else if (rt.envPhase != VoiceRuntime::EnvPhase::Idle)
        {
            rt.envPhase = VoiceRuntime::EnvPhase::Release;
            rt.envelopeLevel -= (float)(1000.0 / ((double)decayReleaseMs[voice.getReleaseNibble()] * currentSampleRate));
            if (rt.envelopeLevel <= 0.0f)
            {
                rt.envelopeLevel = 0.0f;
                rt.envPhase = VoiceRuntime::EnvPhase::Idle;
            }
        }
    }

    float generateWaveform(VoiceRuntime& rt, const SIDVoiceData& voice)
    {
        uint8_t wf = voice.getWaveform();

        if (wf & 0x8) // Rauschen: neuer Zufallswert bei jedem Phasenwrap (grobe Annäherung
                       // an den SID-LFSR-Rauschgenerator, nicht bit-exakt), siehe renderNextSample()
        {
            return rt.noiseHold;
        }
        if (wf & 0x4) // Puls
        {
            float pw = juce::jlimit(0.01f, 0.99f, voice.pulseWidth / 4095.0f);
            return (float)(rt.phase < pw ? 1.0 : -1.0);
        }
        if (wf & 0x2) // Sägezahn
        {
            return (float)(rt.phase * 2.0 - 1.0);
        }
        if (wf & 0x1) // Dreieck
        {
            return (float)(1.0 - 4.0 * std::abs(rt.phase - 0.5));
        }
        return 0.0f;
    }

    float applyFilter(float input)
    {
        if (chip.filterModeBits == 0)
            return input; // Kein Modus aktiv -> Filter wirkt auf echter Hardware nicht

        float cutoffHz = juce::jlimit(30.0f, (float)(currentSampleRate * 0.45),
            30.0f + (chip.filterCutoff11 / 2047.0f) * 8000.0f);
        float g = std::tan(juce::MathConstants<float>::pi * cutoffHz / (float)currentSampleRate);
        float resonanceNorm = chip.resonanceNibble / 15.0f;
        // k-Untergrenze bewusst nicht zu klein gewählt (TPT-SVF kann bei sehr kleinem k in
        // Kombination mit hohem Cutoff hörbar aufschaukeln/knacken statt sauber zu resonieren).
        float k = juce::jmap(resonanceNorm, 0.0f, 1.0f, 1.4f, 0.3f);

        float low = 0.0f, band = 0.0f, high = 0.0f;
        filter.process(input, g, k, low, band, high);

        float out = 0.0f;
        if (chip.filterModeBits & 0x1) out += low;
        if (chip.filterModeBits & 0x2) out += band;
        if (chip.filterModeBits & 0x4) out += high;
        return out;
    }

    float renderNextSample()
    {
        static const int modulatorOf[3] = { 2, 0, 1 }; // SID-Verdrahtung: 1<-3, 2<-1, 3<-2

        float voiceOut[3] = { 0.0f, 0.0f, 0.0f };
        bool  wrappedThisSample[3] = { false, false, false };

        // 1) Hüllkurven aktualisieren, Oszillatorphase vorantreiben, Wrap erkennen.
        for (int i = 0; i < 3; ++i)
        {
            auto& voice = chip.voices[i];
            auto& rt = runtime[i];

            updateEnvelope(rt, voice);

            float freqHz = voiceFrequencyHz(i);
            if (freqHz > 0.0f)
            {
                rt.phase += (double)freqHz / currentSampleRate;
                if (rt.phase >= 1.0)
                {
                    rt.phase -= 1.0;
                    wrappedThisSample[i] = true;
                }
            }
        }

        // 2) Hard-Sync: Phase resetten, wenn der Modulator im *vorherigen* Sample gewrappt
        //    ist (One-Sample-Delay löst die zirkuläre Modulatorkette 1<-3<-2<-1 auf).
        for (int i = 0; i < 3; ++i)
        {
            if (chip.voices[i].isSyncActive() && runtime[modulatorOf[i]].prevWrapped)
                runtime[i].phase = 0.0;
        }

        // 3) Rauschgenerator bei Bedarf neu würfeln (bei jedem Phasenwrap dieser Voice).
        for (int i = 0; i < 3; ++i)
        {
            auto& rt = runtime[i];
            if (wrappedThisSample[i] && (chip.voices[i].getWaveform() & 0x8))
            {
                rt.noiseLfsr ^= (rt.noiseLfsr << 13);
                rt.noiseLfsr ^= (rt.noiseLfsr >> 17);
                rt.noiseLfsr ^= (rt.noiseLfsr << 5);
                rt.noiseHold = ((float)(rt.noiseLfsr & 0xFFFF) / 32768.0f) - 1.0f;
            }
        }

        // 4) Wellenform erzeugen, Ringmodulation anwenden (nur wirksam auf dem
        //    Dreieckgenerator - reale SID-Hardware-Eigenheit), in der Historie für die
        //    Voice-Anzeige ablegen, dann Hüllkurve multiplizieren.
        for (int i = 0; i < 3; ++i)
        {
            auto& voice = chip.voices[i];
            auto& rt = runtime[i];

            float osc = generateWaveform(rt, voice);

            if (voice.isRingModActive() && (voice.getWaveform() & 0x1) != 0)
            {
                if (runtime[modulatorOf[i]].phase >= 0.5)
                    osc = -osc;
            }

            oscHistory[i][(size_t)oscHistoryPos[i]] = osc;
            oscHistoryPos[i] = (oscHistoryPos[i] + 1) % kOscHistorySize;

            voiceOut[i] = osc * rt.envelopeLevel;
        }

        // 5) Voice 3 auf echter Hardware stummschalten, wenn $D418 Bit7 gesetzt ist.
        if (chip.voice3Disconnected)
            voiceOut[2] = 0.0f;
        // Nutzer-Stummschaltung einzelner Stimmen (Klick auf den Stimmennamen in der Darstellung)
        for (int i = 0; i < 3; ++i)
            if ((voiceMuteMask.load() >> i) & 1)
                voiceOut[i] = 0.0f;

        for (int i = 0; i < 3; ++i)
            runtime[i].prevWrapped = wrappedThisSample[i];

        // 6) Summe geroutet (durch den Filter) vs. Bypass, wie auf echter SID-Hardware.
        float routedSum = 0.0f, bypassSum = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            if (chip.isVoiceRoutedThroughFilter(i)) routedSum += voiceOut[i];
            else bypassSum += voiceOut[i];
        }

        float filtered = applyFilter(routedSum);
        float masterVol = chip.masterVolumeNibble / 15.0f;
        float output = (filtered + bypassSum) * masterVol * 0.3f * outputGain.load();

        return juce::jlimit(-1.0f, 1.0f, output);
    }

    juce::AudioDeviceManager audioDeviceManager;
    bool deviceOpen = true;
    juce::AudioSourcePlayer audioSourcePlayer;

    // Schützt traceEvents/chip/runtime/isSidPlaying/isMidiPlaying/midiEvents/midiVisual*, die
    // sowohl vom Audio-Thread (getNextAudioBlock) als auch vom Message-Thread
    // (playSidFile/playMidiFile/stop/timerCallback) angefasst werden.
    mutable juce::CriticalSection lock;

    std::vector<SidCpU6502Engine::RegEvent> traceEvents;
    size_t   currentEventIndex = 0;
    int64_t  currentFrameCounter = 0;
    double   framesPerSecond = 50.125;   // play routine calls per second of the current tune
    float    sidClockHz = 985248.0f;     // PAL or NTSC system clock (pitch of the SID oscillators)
    std::atomic<int>   voiceMuteMask { 0 };   // bit n = voice n+1 muted
    std::atomic<float> outputGain { 1.0f };   // user volume
    uint32_t metroFrames = 0, metroOffset = 0;   // metronome (0 = off)
    int      metroBar = 4, metroStartBeat = 0, clickLeft = 0, clickTotal = 1;
    float    clickPhase = 0.0f, clickStep = 0.0f;
    uint32_t traceEndFrame = 0;          // 0 = traceTotalFrames
    bool     traceLoop = false;
    uint32_t traceTotalFrames = 0;       // > 0: tune from playTrace(), ends after this many frames
    bool     sidFinished = false;
    double   currentSampleRate = 44100.0;
    double   samplesSinceLastFrame = 0;

    std::vector<DecodedMidiEvent> midiEvents;
    size_t   midiEventIndex = 0;
    double   midiPlaybackStartMs = 0.0;
    std::unique_ptr<juce::MidiOutput> midiOutputDevice;
    bool     midiVisualGate[3] = { false, false, false };
    float    midiVisualEnvelope[3] = { 0.0f, 0.0f, 0.0f };
    int      midiVisualNote[3] = { -1, -1, -1 };
    std::array<float, kOscHistorySize> midiPitchHistory[3];
    int      midiPitchHistoryPos[3] = { 0, 0, 0 };

    // Erkennung/Zustand für Mehrstufen-MIDI-Dateien (siehe playMidiFile()/setActiveTier()).
    bool isMultiResSidMidi = false;
    int  currentTierIndex = 0;

    // >= 0 während previewMidiTrack()-Wiedergabe: welche Spur (Track-Index) gerade vorgehört wird.
    int  previewTrackIndex = -1;

    bool isSidPlaying = false;
    bool isMidiPlaying = false;

    SidChipState chip;
    VoiceRuntime runtime[3];
    TptSvf       filter;
    std::array<float, kOscHistorySize> oscHistory[3];
    int oscHistoryPos[3] = { 0, 0, 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPlayerEngine)
};
