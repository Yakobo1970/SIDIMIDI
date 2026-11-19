#include "SidTrace.h"
#include "SidChip.h"
#include <cmath>

namespace sm
{
namespace
{
const float kAttackMs[16] = { 2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000 };
const float kDecayMs[16] = { 6, 24, 48, 72, 114, 168, 204, 240, 300, 750, 1500, 2400, 3000, 9000, 15000, 24000 };

struct Env
{
    enum Phase { idle, attack, decay, sustain, release } phase = idle;
    float level = 0;
    bool gate = false;

    void step (const SIDVoiceData& v, double dtMs)
    {
        if (v.isTestActive()) { level = 0; phase = idle; return; }
        if (gate)
        {
            if (phase == idle || phase == release) phase = attack;
            if (phase == attack)
            {
                level += (float) (dtMs / kAttackMs[v.getAttackNibble()]);
                if (level >= 1.0f) { level = 1.0f; phase = decay; }
            }
            if (phase == decay)
            {
                const float sus = v.getSustainNibble() / 15.0f;
                level -= (float) (dtMs / kDecayMs[v.getDecayNibble()]);
                if (level <= sus) { level = sus; phase = sustain; }
            }
            if (phase == sustain) level = v.getSustainNibble() / 15.0f;
        }
        else
        {
            if (phase != idle) phase = release;
            level -= (float) (dtMs / kDecayMs[v.getReleaseNibble()]);
            if (level <= 0) { level = 0; phase = idle; }
        }
    }
};
} // namespace

void buildTimeline (SidTrace& t, const SidInfo& si)
{
    const auto n = (size_t) t.frames;
    for (auto& v : t.v)
    {
        v.note.assign (n, -1.0f); v.wave.assign (n, 0); v.level.assign (n, 0); v.pulse.assign (n, 0); v.mods.assign (n, 0);
    }
    t.cutoff.assign (n, 0); t.filterMode.assign (n, 0); t.routing.assign (n, 0); t.resonance.assign (n, 0); t.volume.assign (n, 15);
    if (! t.events) return;

    const double clockHz = si.ntsc() ? 1022727.0 : 985248.0;
    const double dtMs = 1000.0 / std::max (1.0, t.fps);
    SidChipState chip;
    Env env[3];
    size_t ei = 0;
    const auto& ev = *t.events;
    float lo = 1000, hi = -1000;

    for (juce::uint32 f = 0; f < t.frames; ++f)
    {
        while (ei < ev.size() && ev[ei].frameIndex <= f)
        {
            const auto& e = ev[ei++];
            const int vi = chip.applyRegisterEvent (e.reg, e.value);
            if (vi >= 0 && (e.reg - vi * 7) == 4)   // control register: gate edges happen at the event, not at the frame end
            {
                const bool g = chip.voices[vi].isGateActive();
                if (g && ! env[vi].gate) { env[vi].phase = Env::attack; }
                else if (! g && env[vi].gate && env[vi].phase != Env::idle) { env[vi].phase = Env::release; }
                env[vi].gate = g;
            }
        }
        for (int i = 0; i < 3; ++i)
        {
            auto& vc = chip.voices[i];
            env[i].gate = vc.isGateActive();
            env[i].step (vc, dtMs);
            auto& out = t.v[i];
            const bool audible = vc.getWaveform() != 0 && vc.frequency > 0 && env[i].level > 0.004f && ! (i == 2 && chip.voice3Disconnected && false);
            if (audible)
            {
                const double hz = (double) vc.frequency * clockHz / 16777216.0;
                const float note = (float) (69.0 + 12.0 * std::log2 (hz / 440.0));
                out.note[f] = note;
                if (env[i].level > 0.02f) { lo = std::min (lo, note); hi = std::max (hi, note); }
            }
            out.wave[f] = vc.getWaveform();
            out.level[f] = (juce::uint8) juce::jlimit (0, 255, (int) (env[i].level * 255.0f));
            out.pulse[f] = vc.pulseWidth;
            out.mods[f] = (juce::uint8) ((vc.isRingModActive() ? 1 : 0) | (vc.isSyncActive() ? 2 : 0) | (vc.isGateActive() ? 4 : 0));
        }
        t.cutoff[f] = chip.filterCutoff11;
        t.filterMode[f] = chip.filterModeBits;
        t.routing[f] = chip.routingMask;
        t.resonance[f] = chip.resonanceNibble;
        t.volume[f] = chip.masterVolumeNibble;
    }
    if (hi > lo) { t.minNote = std::floor (lo) - 1; t.maxNote = std::ceil (hi) + 1; }
    if (t.maxNote - t.minNote < 24) { const float mid = (t.maxNote + t.minNote) * 0.5f; t.minNote = mid - 12; t.maxNote = mid + 12; }
}

std::shared_ptr<SidTrace> computeTrace (const SidInfo& si, int song, double wantSeconds, const std::function<bool()>& cancel, double maxEmulationSeconds)
{
    auto t = std::make_shared<SidTrace>();
    t->song = song;
    if (! si.valid || si.code.empty()) { t->note = "Keine Daten"; return t; }

    SidCpU6502Engine::TraceOptions o;
    o.ciaSpeed = si.cia (song);
    o.ntsc = si.ntsc();
    o.maxSeconds = maxEmulationSeconds;
    o.cancel = cancel;
    // rough upper bound of the frame rate (CIA timers can be fast) just to size the request
    const double rate = o.ciaSpeed ? 200.0 : (o.ntsc ? 60.0 : 50.2);
    const juce::uint32 want = (juce::uint32) juce::jlimit (50.0, 80000.0, std::ceil (wantSeconds * rate));
    const juce::uint16 play = si.playAddr != 0 ? si.playAddr : si.effInit();

    auto r = SidCpU6502Engine::traceSidExecutionEx (si.code.data(), si.code.size(), si.effLoad, si.effInit(), play, (juce::uint16) song, want, o);
    t->fps = r.framesPerSecond;
    t->timedOut = r.timedOut;
    t->cancelled = r.cancelled;
    t->noPlay = r.noPlay;
    t->events = std::make_shared<std::vector<SidCpU6502Engine::RegEvent>> (std::move (r.events));
    if (r.noPlay) t->note = "Keine Play-Routine gefunden";
    else if (t->events->empty()) t->note = "Der Tune schreibt nichts in die SID-Register";
    if (r.cancelled) return t;

    // the timeline covers the requested time (a tune may be silent at the end), but at most what was emulated
    const juce::uint32 emulated = std::max (r.frames, t->events->empty() ? 0u : t->events->back().frameIndex + 1);
    t->frames = std::min<juce::uint32> (std::max<juce::uint32> (emulated, 1u), (juce::uint32) std::ceil (wantSeconds * t->fps));
    if (t->frames == 0) t->frames = 1;
    buildTimeline (*t, si);
    return t;
}
} // namespace sm
