#include "SidView.h"
#include <cmath>

namespace sm
{
juce::Colour voiceColour (int voice, bool light)
{
    static const juce::uint32 dark[3] = { 0xffff6b4a, 0xff35d6ff, 0xffb5f23a };
    static const juce::uint32 lite[3] = { 0xffd8401f, 0xff0a8fb8, 0xff6d9a0a };
    return juce::Colour ((light ? lite : dark)[juce::jlimit (0, 2, voice)]);
}

namespace
{
juce::String noteName (float note)
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "H" };
    const int n = (int) std::lround (note);
    const char* base = names[((n % 12) + 12) % 12];
    juce::String s (base);
    if (juce::String (base) == "H") s = "B";
    return s + juce::String (n / 12 - 1);
}
} // namespace

SidView::SidView()
{
    setOpaque (false);
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
}

SidView::~SidView()
{
    if (cancelFlag) *cancelFlag = true;
    pool.removeAllJobs (true, 8000);
}

const Theme& SidView::theme() const
{
    if (auto* l = dynamic_cast<ui::Look*> (&getLookAndFeel())) return l->theme();
    return themes()[0];
}

void SidView::resized() { invalidate(); }

void SidView::showNothing (const juce::String& m)
{
    if (cancelFlag) *cancelFlag = true;
    current.reset();
    shownId = 0; shownSong = 0; computing = false;
    message = m; status = {};
    invalidate();
}

void SidView::show (const Entry* e, int song)
{
    if (e == nullptr) { showNothing (T ("Keine Datei markiert")); return; }
    if (e->id == shownId && song == shownSong && (current != nullptr || computing)) return;
    if (e->has (fMissing)) { showNothing (T ("Die Datei fehlt auf der Festplatte")); shownId = e->id; return; }
    if (e->has (fError)) { showNothing (TS (e->error)); shownId = e->id; return; }
    startCompute (*e, song);
}

void SidView::startCompute (const Entry& e, int song)
{
    if (cancelFlag) *cancelFlag = true;
    cancelFlag = std::make_shared<std::atomic<bool>> (false);
    shownId = e.id; shownSong = song;
    computing = true; current.reset(); message = {}; status = {};
    startTimerHz (4);
    viewStart = 0; viewSpan = 0; playFrame = -1;
    invalidate();
    const auto path = e.path;
    const double known = e.lengthOf (song);
    auto flag = cancelFlag;
    juce::Component::SafePointer<SidView> safe (this);
    const int id = e.id;
    pool.removeAllJobs (false, 0);
    pool.addJob ([safe, path, song, known, flag, id]
    {
        const auto si = parseSidFile (juce::File (path), true);
        const double want = known > 0 ? std::min (known + 2.0, 1500.0) : 150.0;
        auto t = computeTrace (si, song, want, [flag] { return flag->load(); });
        if (flag->load()) return;
        juce::MessageManager::callAsync ([safe, t, si, flag, id, song]
        {
            if (safe == nullptr || flag->load()) return;
            auto* self = safe.getComponent();
            self->sidInfo = si;
            self->current = t;
            self->computing = false;
            self->shownId = id; self->shownSong = song;
            self->message = t->note;
            self->status = t->timedOut ? T ("Berechnung nach dem Zeitlimit abgebrochen – Verlauf unvollständig") : juce::String();
            if (si.basicTune) self->message = T ("BASIC-Programm (RSID): braucht einen echten C64 und lässt sich hier nicht darstellen");
            self->invalidate();
            if (self->onTraceReady) self->onTraceReady();
        });
    });
}

void SidView::setPlayFrame (juce::int64 frame)
{
    if (frame == playFrame) return;
    const auto oldRect = juce::Rectangle<int> ((int) lastCursorX - 7, 0, 15, getHeight());
    playFrame = frame;
    if (current == nullptr || frame < 0) { repaint (oldRect); lastCursorX = -1; return; }
    // follow the cursor when zoomed in
    if (viewSpan > 0)
    {
        const double s = current->frameSeconds ((juce::uint32) frame);
        if (s < viewStart || s > viewStart + viewSpan) { viewStart = std::max (0.0, s - viewSpan * 0.1); clampView(); invalidate(); return; }
    }
    const int x = secondsToX (current->frameSeconds ((juce::uint32) frame));
    lastCursorX = x;
    repaint (oldRect);
    repaint (juce::Rectangle<int> (x - 7, 0, 15, getHeight()));
}

void SidView::resetZoom() { viewStart = 0; viewSpan = 0; invalidate(); }

SidView::Geometry SidView::geometry() const
{
    Geometry g;
    auto r = getLocalBounds().reduced (0, 0);
    const int gutter = 46, right = 8;
    auto plot = r.withTrimmedLeft (gutter).withTrimmedRight (right);
    g.ruler = plot.removeFromTop (20);
    const int filterH = juce::jlimit (26, 50, plot.getHeight() / 6);
    g.filter = plot.removeFromBottom (filterH);
    plot.removeFromBottom (4);
    const int laneH = (plot.getHeight() - 8) / 3;
    for (int i = 0; i < 3; ++i)
    {
        g.lanes[i] = plot.removeFromTop (laneH);
        plot.removeFromTop (4);
    }
    g.plot = getLocalBounds().withTrimmedLeft (gutter).withTrimmedRight (right);
    return g;
}

double SidView::xToSeconds (int x) const
{
    if (current == nullptr) return 0;
    const auto g = geometry();
    const double total = current->seconds();
    const double span = viewSpan > 0 ? viewSpan : total;
    const double f = (double) (x - g.plot.getX()) / std::max (1, g.plot.getWidth());
    return viewStart + f * span;
}

int SidView::secondsToX (double s) const
{
    if (current == nullptr) return 0;
    const auto g = geometry();
    const double total = current->seconds();
    const double span = viewSpan > 0 ? viewSpan : total;
    return g.plot.getX() + (int) std::lround ((s - viewStart) / std::max (0.001, span) * g.plot.getWidth());
}

void SidView::clampView()
{
    if (current == nullptr) return;
    const double total = current->seconds();
    if (viewSpan >= total * 0.999) { viewSpan = 0; viewStart = 0; return; }
    if (viewSpan > 0) viewStart = juce::jlimit (0.0, std::max (0.0, total - viewSpan), viewStart);
}

void SidView::renderCache()
{
    const auto& th = theme();
    const bool light = th.bg.getBrightness() > 0.5f;
    float scale = 1.0f;
    if (auto* peer = getPeer()) scale = (float) peer->getPlatformScaleFactor();
    scale = std::max (1.0f, scale * (float) juce::Component::getApproximateScaleFactorForComponent (this));
    cacheScale = scale;
    cache = juce::Image (juce::Image::ARGB, std::max (1, (int) std::ceil ((float) getWidth() * scale)), std::max (1, (int) std::ceil ((float) getHeight() * scale)), true);
    juce::Graphics g (cache);
    g.addTransform (juce::AffineTransform::scale (scale));
    const auto geo = geometry();

    // lane panels
    auto lanePanel = th.panel.interpolatedWith (th.bg, 0.25f);
    for (int i = 0; i < 3; ++i)
    {
        g.setColour (lanePanel);
        g.fillRoundedRectangle (geo.lanes[i].toFloat(), 5.0f);
        g.setColour (voiceColour (i, light).withAlpha (muteLanes[i] ? 0.25f : 0.9f));
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) geo.lanes[i].getX(), (float) geo.lanes[i].getY(), 3.0f, (float) geo.lanes[i].getHeight()), 1.5f);
    }
    g.setColour (lanePanel);
    g.fillRoundedRectangle (geo.filter.toFloat(), 5.0f);

    if (current == nullptr || current->frames == 0)
        return;

    const auto& t = *current;
    const double total = t.seconds();
    const double span = viewSpan > 0 ? viewSpan : total;
    const double fps = t.fps;
    const double pxPerSec = (double) geo.plot.getWidth() / std::max (0.001, span);

    // ruler
    {
        static const double steps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
        double step = 600;
        for (double s : steps) if (s * pxPerSec >= 56.0) { step = s; break; }
        g.setFont (ui::font (11.0f));
        for (double s = std::ceil (viewStart / step) * step; s <= viewStart + span + 0.001; s += step)
        {
            const float x = (float) (geo.plot.getX() + (s - viewStart) * pxPerSec);
            g.setColour (th.dim.withAlpha (0.35f));
            g.drawVerticalLine ((int) x, (float) geo.ruler.getBottom() - 4.0f, (float) geo.filter.getBottom());
            g.setColour (th.dim);
            g.drawText (formatTime (s), (int) x + 3, geo.ruler.getY(), 56, geo.ruler.getHeight(), juce::Justification::centredLeft, false);
        }
    }

    const double f0d = viewStart * fps, f1d = (viewStart + span) * fps;
    const juce::uint32 fa = (juce::uint32) std::max (0.0, std::floor (f0d)), fb = (juce::uint32) std::min ((double) t.frames, std::ceil (f1d) + 1);
    const float range = std::max (12.0f, t.maxNote - t.minNote);

    for (int v = 0; v < 3; ++v)
    {
        const auto lane = geo.lanes[v];
        const auto vc = voiceColour (v, light);
        const float top = (float) lane.getY() + 4.0f, h = (float) lane.getHeight() - 8.0f;
        // octave lines (C notes) with names in the gutter
        g.setFont (ui::font (10.0f));
        for (int n = (int) std::ceil (t.minNote / 12.0f) * 12; n <= t.maxNote; n += 12)
        {
            const float y = top + (t.maxNote - (float) n) / range * h;
            g.setColour (th.dim.withAlpha (0.18f));
            g.drawHorizontalLine ((int) y, (float) lane.getX() + 4.0f, (float) lane.getRight());
            g.setColour (th.dim.withAlpha (0.75f));
            g.drawText (noteName ((float) n), lane.getX() - 40, (int) y - 6, 36, 12, juce::Justification::centredRight, false);
        }
        // voice label
        g.setColour (vc.withAlpha (muteLanes[v] ? 0.4f : 1.0f));
        g.setFont (ui::font (11.5f, true));
        g.drawText (T ("Stimme %1", v + 1), lane.getX() + 8, lane.getY() + 1, 90, 14, juce::Justification::centredLeft, false);

        const float barH = juce::jlimit (3.0f, 11.0f, h / range * 1.15f);
        const auto& voice = t.v[v];
        const float alphaMul = muteLanes[v] ? 0.25f : 1.0f;
        juce::uint32 runStart = fa;
        auto flushRun = [&] (juce::uint32 a, juce::uint32 b)
        {
            if (b <= a) return;
            const float note = voice.note[a];
            if (note < 0) return;
            int lvl = 0;
            for (juce::uint32 f = a; f < b; ++f) lvl = std::max<int> (lvl, voice.level[f]);
            const auto w = voice.wave[a];
            const float x0 = (float) (geo.plot.getX() + ((double) a / fps - viewStart) * pxPerSec);
            const float x1 = (float) (geo.plot.getX() + ((double) b / fps - viewStart) * pxPerSec);
            const float cx0 = std::max (x0, (float) lane.getX()), cx1 = std::min (x1, (float) lane.getRight());
            if (cx1 <= cx0) return;
            const float ww = std::max (1.2f, cx1 - cx0);
            const float y = top + (t.maxNote - note) / range * h;
            auto c = vc.withAlpha ((0.30f + 0.70f * (float) lvl / 255.0f) * alphaMul);
            float bh = barH;
            if (w == wTriangle) bh = barH * 0.55f;
            if (w == wNoise) c = c.interpolatedWith (juce::Colours::white, 0.45f);
            g.setColour (c);
            g.fillRect (cx0, y - bh * 0.5f, ww, bh);
            if (w == wSaw && ww > 2.0f)
            {
                g.setColour (juce::Colours::white.withAlpha (0.55f * alphaMul));
                g.fillRect (cx0, y - bh * 0.5f, ww, 1.0f);
            }
            else if ((w & 0xF0) == 0 && (w & (w - 1)) != 0)   // combined waveforms: striped
            {
                g.setColour (th.bg.withAlpha (0.45f));
                g.fillRect (cx0, y - 0.5f, ww, 1.0f);
            }
        };
        for (juce::uint32 f = fa + 1; f <= fb; ++f)
        {
            const bool end = f >= fb;
            bool split = end;
            if (! end)
            {
                const float n0 = voice.note[f - 1], n1 = voice.note[f];
                split = (n0 < 0) != (n1 < 0) || (n0 >= 0 && std::abs (n1 - n0) > 0.55f) || voice.wave[f] != voice.wave[f - 1]
                    || (n0 >= 0 && voice.level[f] > voice.level[f - 1] + 40);   // new attack
            }
            if (split) { flushRun (runStart, f); runStart = f; }
        }
    }

    // filter and volume
    {
        const auto fl = geo.filter;
        bool any = false;
        for (juce::uint32 f = fa; f < fb && ! any; ++f) any = t.routing[f] != 0 && t.filterMode[f] != 0;
        g.setFont (ui::font (11.5f, true));
        g.setColour (th.track[3].withAlpha (any ? 1.0f : 0.45f));
        g.drawText (T ("Filter"), fl.getX() + 8, fl.getY() + 1, 70, 14, juce::Justification::centredLeft, false);
        g.setColour (th.dim);
        g.setFont (ui::font (10.0f));
        g.drawText (T ("Lautstärke"), fl.getX() - 44, fl.getBottom() - 14, 42, 12, juce::Justification::centredRight, false);
        const float top = (float) fl.getY() + 4.0f, h = (float) fl.getHeight() - 8.0f;
        juce::Path cut, vol;
        bool cutOpen = false, volOpen = false;
        const int step = std::max (1, (int) ((fb - fa) / (juce::uint32) std::max (1, geo.plot.getWidth() * 2)));
        for (juce::uint32 f = fa; f < fb; f += (juce::uint32) step)
        {
            const float x = (float) (geo.plot.getX() + ((double) f / fps - viewStart) * pxPerSec);
            if (x < (float) fl.getX() || x > (float) fl.getRight()) continue;
            const float vy = top + (1.0f - (float) t.volume[f] / 15.0f) * h;
            if (! volOpen) { vol.startNewSubPath (x, vy); volOpen = true; } else vol.lineTo (x, vy);
            if (t.routing[f] != 0 && t.filterMode[f] != 0)
            {
                const float cy = top + (1.0f - (float) t.cutoff[f] / 2047.0f) * h;
                if (! cutOpen) { cut.startNewSubPath (x, cy); cutOpen = true; } else cut.lineTo (x, cy);
            }
            else cutOpen = false;
        }
        g.setColour (th.dim.withAlpha (0.55f));
        g.strokePath (vol, juce::PathStrokeType (1.0f));
        g.setColour (th.track[3]);
        g.strokePath (cut, juce::PathStrokeType (1.8f));
    }
}

void SidView::paint (juce::Graphics& g)
{
    const auto& th = theme();
    if (cache.isNull()) renderCache();
    g.drawImage (cache, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
    const auto geo = geometry();

    if (current == nullptr || current->frames == 0)
    {
        g.setColour (th.dim);
        g.setFont (ui::font (14.0f));
        auto box = getLocalBounds().withTrimmedLeft (46);
        if (computing)
        {
            const int dots = (int) ((juce::Time::getMillisecondCounter() / 300) % 4);
            g.drawText (T ("Verlauf wird berechnet") + juce::String ("...").substring (0, dots), box, juce::Justification::centred);
        }
        else
            g.drawFittedText (message.isNotEmpty() ? message : T ("Keine Datei markiert"), box.reduced (20), juce::Justification::centred, 3);
        return;
    }

    if (message.isNotEmpty() || status.isNotEmpty())
    {
        g.setColour (th.track[3]);
        g.setFont (ui::font (12.0f));
        g.drawText (message.isNotEmpty() ? TS (message) : status, geo.plot.getX() + 120, geo.ruler.getY(), geo.plot.getWidth() - 130, geo.ruler.getHeight(), juce::Justification::centredRight, true);
    }
    // cursor
    if (playFrame >= 0)
    {
        const int x = secondsToX (current->frameSeconds ((juce::uint32) playFrame));
        if (x >= geo.plot.getX() && x <= geo.plot.getRight())
        {
            g.setColour (th.text.withAlpha (0.9f));
            g.fillRect (x - 1, geo.ruler.getBottom() - 2, 2, geo.filter.getBottom() - geo.ruler.getBottom() + 2);
            juce::Path tri;
            tri.addTriangle ((float) x - 5.0f, (float) geo.ruler.getY() + 6.0f, (float) x + 5.0f, (float) geo.ruler.getY() + 6.0f, (float) x, (float) geo.ruler.getBottom() - 1.0f);
            g.fillPath (tri);
        }
    }
    if (hoverX >= geo.plot.getX() && ! dragging)
    {
        g.setColour (th.text.withAlpha (0.25f));
        g.drawVerticalLine (hoverX, (float) geo.ruler.getBottom(), (float) geo.filter.getBottom());
    }
}

juce::String SidView::getTooltip()
{
    if (current == nullptr || hoverX < 0) return {};
    const auto geo = geometry();
    if (hoverX < geo.plot.getX()) return T ("Klick auf den Stimmennamen schaltet die Stimme stumm");
    const double s = xToSeconds (hoverX);
    if (s < 0 || s > current->seconds()) return {};
    const auto f = std::min<juce::uint32> (current->frames - 1, current->frameAt (s));
    juce::String text = formatTime (s, true);
    static const char* waves[16] = { "–", "Dreieck", "Säge", "Dreieck+Säge", "Puls", "Puls+Dreieck", "Puls+Säge", "Puls+Dreieck+Säge", "Rauschen", "", "", "", "", "", "", "" };
    for (int v = 0; v < 3; ++v)
    {
        const float n = current->v[v].note[f];
        text << "\n" << T ("Stimme %1", v + 1) << ": " << (n >= 0 ? noteName (n) + "  " + T (waves[current->v[v].wave[f] & 15]) : juce::String ("–"));
    }
    return text;
}

void SidView::mouseMove (const juce::MouseEvent& e)
{
    const int old = hoverX;
    hoverX = e.x;
    if (old != hoverX) repaint();
}

void SidView::mouseDown (const juce::MouseEvent& e)
{
    if (current == nullptr) return;
    const auto geo = geometry();
    if (e.x < geo.plot.getX())
    {
        for (int v = 0; v < 3; ++v)
            if (e.y >= geo.lanes[v].getY() && e.y < geo.lanes[v].getBottom())
            {
                if (onLaneToggle) onLaneToggle (v);
                return;
            }
        return;
    }
    dragging = true;
    mouseDrag (e);
}

void SidView::mouseDrag (const juce::MouseEvent& e)
{
    if (current == nullptr || ! dragging) return;
    const double s = juce::jlimit (0.0, current->seconds(), xToSeconds (e.x));
    if (onSeek) onSeek (current->frameAt (s));
}

void SidView::mouseUp (const juce::MouseEvent&) { dragging = false; repaint(); }

void SidView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.x >= geometry().plot.getX()) resetZoom();
}

void SidView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (current == nullptr) return;
    const double total = current->seconds();
    const double span = viewSpan > 0 ? viewSpan : total;
    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        const double at = xToSeconds (e.x);
        const double factor = std::pow (0.8, (double) w.deltaY * 6.0);
        double ns = juce::jlimit (3.0, total, span * factor);
        const double rel = (at - viewStart) / std::max (0.001, span);
        viewSpan = ns >= total * 0.999 ? 0 : ns;
        viewStart = at - rel * ns;
        clampView();
        invalidate();
    }
    else if (viewSpan > 0)
    {
        viewStart -= (double) (w.deltaY != 0 ? w.deltaY : w.deltaX) * span * 0.5;
        clampView();
        invalidate();
    }
}
} // namespace sm
