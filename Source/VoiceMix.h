// SIDiMIDI – several MIDI tracks on one voice: which tracks, in which role, and who wins when they sound together.
#pragma once
#include <array>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <limits>

namespace sm
{
// one MIDI track (as in MidiToSidCoreEngine::ChannelSuggestion::partId(); -2 = automatic, -1 = none) that plays a voice
struct MidiVoiceSource
{
    int part = -1;
    int role = 0;          // 0 automatic, 1 lead, 2 bass, 3 pad, 4 drums
    int priority = 0;      // 0 = highest: it wins when tracks of the same voice sound together
    std::string kit;       // key of a drum kit (InstrumentPool::keyOf): every drum of the track gets the preset of its kind
    std::string preset;    // key of an instrument pool preset (InstrumentPool::keyOf); empty = the sound follows the role
};
using VoiceSources = std::array<std::vector<MidiVoiceSource>, 3>;

inline VoiceSources autoVoiceSources()
{
    VoiceSources s;
    for (auto& v : s) v.push_back ({ -2, 0, 0 });
    return s;
}

// Notes of several tracks on one voice: the first level (the highest priority) is kept as it is. A note of a lower level that
// begins while a higher one sounds is dropped; one that begins in a gap is cut where the next higher note begins. The
// lower levels fill the pauses of the higher ones. Levels are ordered from the highest priority to the lowest.
template <class N>
std::vector<N> mergeByPriority (const std::vector<std::vector<N>>& levels, double N::*start, double N::*end)
{
    if (levels.empty()) return {};
    std::vector<N> out = levels[0];
    std::map<double, double> covered;                    // disjoint intervals that a higher level already uses
    auto add = [&] (double a, double b)
    {
        if (b <= a) b = a + 1.0e-3;
        auto it = covered.lower_bound (a);
        if (it != covered.begin())
        {
            auto p = std::prev (it);
            if (p->second >= a) { a = p->first; b = std::max (b, p->second); it = covered.erase (p); }
        }
        while (it != covered.end() && it->first <= b) { b = std::max (b, it->second); it = covered.erase (it); }
        covered[a] = b;
    };
    for (auto& n : levels[0]) add (n.*start, n.*end);
    for (size_t l = 1; l < levels.size(); ++l)
    {
        std::vector<N> sorted = levels[l];
        std::stable_sort (sorted.begin(), sorted.end(), [&] (const N& a, const N& b) { return a.*start < b.*start; });
        std::vector<std::pair<double, double>> taken;    // used only after the whole level is done (chords start together)
        for (auto& n : sorted)
        {
            const double a = n.*start;
            auto it = covered.upper_bound (a);
            if (it != covered.begin() && std::prev (it)->second > a + 1.0e-4) continue;      // a higher note sounds
            N m = n;
            if (it != covered.end() && m.*end > it->first) m.*end = it->first;                 // the next higher note begins
            if (m.*end <= a) continue;
            out.push_back (m);
            taken.push_back ({ a, m.*end });
        }
        for (auto& t : taken) add (t.first, t.second);
    }
    std::stable_sort (out.begin(), out.end(), [&] (const N& a, const N& b) { return a.*start < b.*start; });
    return out;
}
} // namespace sm
