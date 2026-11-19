#include "SidTable.h"
#include "SidTrace.h"
#include "SidView.h"
#include <algorithm>

namespace sm
{
namespace
{
const ColDef kCols[] = {
    { cFav,      "♥",              34,  false, true,  "Favorit (Klick)" },
    { cRating,   "Bewertung",      92,  false, true,  "Bewertung 1–5 Sterne (Klick auf einen Stern; erneut klicken nimmt ihn zurück)" },
    { cTitle,    "Titel",          230, false, true,  "Titel aus dem SID-Kopf" },
    { cAuthor,   "Autor",          170, false, true,  "Autor aus dem SID-Kopf" },
    { cReleased, "Veröffentlicht", 150, false, true,  "Jahr und Firma aus dem SID-Kopf" },
    { cSongs,    "Songs",          52,  true,  true,  "Anzahl der Songs (Subtunes)" },
    { cLength,   "Länge",          58,  true,  true,  "Länge des Start-Songs (Songlängen-Datenbank der HVSC)" },
    { cLandscape, "Verlauf",       170, false, true,  "Miniatur des Ablaufs: die drei Stimmen als Stapellandschaft (wird im Hintergrund für die sichtbaren Zeilen berechnet)" },
    { cType,     "Typ",            76,  false, true,  "PSID oder RSID mit Versionsnummer" },
    { cSid,      "SID",            74,  false, true,  "SID-Chip-Typ laut Kopf: 6581 oder 8580" },
    { cClock,    "Takt",           68,  false, true,  "PAL oder NTSC laut Kopf" },
    { cChips,    "Chips",          54,  true,  false, "Anzahl der SID-Chips (2 oder 3 bei Stereo-/Triple-SID)" },
    { cComposer, "Komponist",      150, false, true,  "Komponist laut Ordnername in der HVSC (mit Musikerliste)" },
    { cCountry,  "Land",           130, false, true,  "Land des Komponisten (HVSC-Musikerliste)" },
    { cArea,     "Bereich",        80,  false, false, "MUSICIANS, GAMES oder DEMOS in der HVSC" },
    { cLoad,     "Ladeadresse",    84,  true,  false, "Adresse, an die der Tune geladen wird" },
    { cSize,     "Größe",          66,  true,  false, "Dateigröße" },
    { cFile,     "Datei",          200, false, false, "Dateiname" },
    { cPath,     "Pfad",           280, false, true,  "Pfad in der HVSC bzw. der Ordner der Datei" },
    { cStil,     "STIL",           260, false, true,  "Erste Zeile des STIL-Eintrags (Hintergrundinfos der HVSC)" },
    { cNote,     "Hinweis",        120, false, true,  "Bug-Hinweis der HVSC, identische Dateien, fehlende Datei" } };

double numericValue (const Entry& e, int col)
{
    switch (col)
    {
        case cFav: return e.favourite ? 1 : 0;
        case cRating: return e.rating;
        case cSongs: return e.songs;
        case cLength: return e.seconds;
        case cChips: return e.sidCount;
        case cLoad: return e.load;
        case cSize: return (double) e.size;
        case cReleased: return e.year;
        default: return 0;
    }
}

bool isNumericMaskColumn (int col)
{
    return col == cSongs || col == cLength || col == cChips || col == cSize || col == cRating || col == cLoad;
}

// "3" (equal), ">120", "<=60", "=5", "2-5" ; time columns accept m:ss
bool numericTerm (const juce::String& term, double v, int col)
{
    auto num = [col] (juce::String s) -> double
    {
        s = s.trim();
        if (col == cLength && s.containsChar (':')) return s.upToFirstOccurrenceOf (":", false, false).getIntValue() * 60.0 + s.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
        if (col == cSize) { if (s.endsWithIgnoreCase ("k")) return s.getDoubleValue() * 1024.0; return s.getDoubleValue(); }
        if (col == cLoad && s.startsWithChar ('$')) return (double) s.substring (1).getHexValue32();
        return s.getDoubleValue();
    };
    if (term.startsWith (">=")) return v >= num (term.substring (2));
    if (term.startsWith ("<=")) return v <= num (term.substring (2));
    if (term.startsWithChar ('>')) return v > num (term.substring (1));
    if (term.startsWithChar ('<')) return v < num (term.substring (1));
    if (term.startsWithChar ('=')) return std::abs (v - num (term.substring (1))) < 0.5;
    const int dash = term.indexOfChar (1, '-');
    if (dash > 0) return v >= num (term.substring (0, dash)) && v <= num (term.substring (dash + 1));
    return std::abs (v - num (term)) < (col == cLength ? 1.0 : 0.5);
}

bool matchesMask (const Entry& e, int col, const juce::String& mask)
{
    if (mask.isEmpty()) return true;
    juce::StringArray terms;
    terms.addTokens (mask, " ", "\"");
    terms.removeEmptyStrings();
    const bool numeric = isNumericMaskColumn (col);
    const auto text = numeric ? juce::String() : cellText (e, col);
    for (auto t : terms)
    {
        bool neg = false;
        if (t.startsWithChar ('!')) { neg = true; t = t.substring (1); }
        t = t.unquoted();
        if (t.isEmpty()) continue;
        const bool ok = numeric ? numericTerm (t, numericValue (e, col), col) : text.containsIgnoreCase (t);
        if (ok == neg) return false;
    }
    return true;
}
} // namespace

const ColDef& colDef (int id)
{
    for (auto& c : kCols) if (c.id == id) return c;
    return kCols[0];
}

juce::String niceCountry (const juce::String& upper)
{
    juce::String out;
    for (const auto& word : juce::StringArray::fromTokens (upper, " ", {}))
    {
        if (word.isEmpty()) continue;
        juce::String w;
        const bool bracket = word.startsWithChar ('(');
        const auto core = bracket ? word.substring (1) : word;
        if (core.length() <= 3) w = core;   // USA, UK
        else w = core.substring (0, 1).toUpperCase() + core.substring (1).toLowerCase();
        out << (out.isEmpty() ? "" : " ") << (bracket ? "(" : "") << w;
    }
    return out;
}

juce::String cellText (const Entry& e, int col)
{
    switch (col)
    {
        case cTitle: return e.title.isNotEmpty() && e.title != "<?>" ? e.title : e.name.upToLastOccurrenceOf (".", false, false);
        case cAuthor: return e.author == "<?>" ? juce::String() : e.author;
        case cReleased: return e.released == "<?>" ? juce::String() : e.released;
        case cSongs: return juce::String (e.songs);
        case cLength: return lengthText (e);
        case cType: return e.has (fError) ? juce::String() : juce::String (e.has (fRsid) ? "RSID" : "PSID") + " v" + juce::String (e.version);
        case cSid: { switch (e.model) { case 1: return "6581"; case 2: return "8580"; case 3: return "6581/8580"; default: return {}; } }
        case cClock: { switch (e.clock) { case 1: return "PAL"; case 2: return "NTSC"; case 3: return "PAL/NTSC"; default: return {}; } }
        case cChips: return e.sidCount > 1 ? juce::String (e.sidCount) + "x" : juce::String();
        case cComposer: return e.composer;
        case cCountry: return niceCountry (e.country);
        case cArea: return e.area == "MUSICIANS" ? T ("Musiker") : e.area == "GAMES" ? T ("Spiele") : e.area == "DEMOS" ? T ("Demos") : e.area;
        case cLoad: return e.load != 0 ? "$" + juce::String::toHexString ((int) e.load).toUpperCase().paddedLeft ('0', 4) : juce::String();
        case cSize: return e.size >= 1024 ? juce::String ((double) e.size / 1024.0, 1) + " KB" : juce::String (e.size) + " B";
        case cFile: return e.name;
        case cPath: return e.rel.isNotEmpty() ? e.rel.upToLastOccurrenceOf ("/", false, false) : e.folder;
        case cStil: return e.stilFirst;
        case cNote:
        {
            juce::StringArray n;
            if (e.has (fMissing)) n.add (T ("Datei fehlt"));
            if (e.has (fError)) n.add (TS (e.error));
            if (e.has (fHasBug)) n.add (T ("Bug-Liste"));
            if (e.has (fSameFile)) n.add (T ("identisch ×%1", e.sameGroup));
            if (e.has (fBasic)) n.add ("BASIC");
            if (e.has (fMus)) n.add ("MUS");
            return n.joinIntoString (", ");
        }
        default: return {};
    }
}

// ================================================================================================ table
struct HeaderWatcher : juce::TableHeaderComponent::Listener, juce::ScrollBar::Listener
{
    std::function<void()> changed;
    void tableColumnsChanged (juce::TableHeaderComponent*) override { if (changed) changed(); }
    void tableColumnsResized (juce::TableHeaderComponent*) override { if (changed) changed(); }
    void tableSortOrderChanged (juce::TableHeaderComponent*) override {}
    void scrollBarMoved (juce::ScrollBar*, double) override { if (changed) changed(); }
};
static HeaderWatcher* watcherOf (void* p) { return static_cast<HeaderWatcher*> (p); }

SidTable::SidTable (Archive& a) : arch (a), table ("sids", this)
{
    auto* w = new HeaderWatcher();
    w->changed = [this] { layoutFooter(); };
    watcher = w;
    addAndMakeVisible (table);
    addAndMakeVisible (footer);
    table.setRowHeight (24);
    table.setHeaderHeight (28);
    table.setMultipleSelectionEnabled (true);
    table.getHeader().setStretchToFitActive (false);
    table.setWantsKeyboardFocus (true);
    table.addKeyListener (&navKeys);
    clickTimer.cb = [this] { startPendingEdit(); };
    auto& hdr = table.getHeader();
    int flags = juce::TableHeaderComponent::resizable | juce::TableHeaderComponent::sortable | juce::TableHeaderComponent::draggable | juce::TableHeaderComponent::appearsOnColumnMenu;
    for (auto& c : kCols)
    {
        int f = c.visible ? (flags | juce::TableHeaderComponent::visible) : flags;
        if (c.id == cLandscape) f &= ~juce::TableHeaderComponent::sortable;
        hdr.addColumn (T (c.name), c.id, c.width, 24, 600, f);
    }
    hdr.setSortColumnId (sortCol, sortForward);
    hdr.addListener (watcherOf (watcher));
    table.getViewport()->getHorizontalScrollBar().addListener (watcherOf (watcher));

    for (auto& c : kCols)
    {
        if (c.id == cFav || c.id == cLandscape) continue;
        auto ed = std::make_unique<juce::TextEditor>();
        ed->setFont (ui::font (12.5f));
        ed->setIndents (4, 3);
        ed->setBorder (juce::BorderSize<int> (1));
        const int id = c.id;
        ed->onTextChange = [this, id] { masksChanged = true; startTimer (200); (void) id; };
        footer.addAndMakeVisible (*ed);
        masks[c.id] = std::move (ed);
    }
    applyLanguage();
}

SidTable::~SidTable()
{
    stopTimer();
    clickTimer.stopTimer();
    table.removeKeyListener (&navKeys);
    editor.reset();
    lsState->alive = false;
    landscapePool.removeAllJobs (true, 5000);
    table.getHeader().removeListener (watcherOf (watcher));
    table.getViewport()->getHorizontalScrollBar().removeListener (watcherOf (watcher));
    delete watcherOf (watcher);
}

const Theme& SidTable::theme() const
{
    if (auto* l = dynamic_cast<ui::Look*> (&getLookAndFeel())) return l->theme();
    return themes()[0];
}

void SidTable::applyLanguage()
{
    auto& hdr = table.getHeader();
    for (auto& c : kCols)
    {
        hdr.setColumnName (c.id, T (c.name));
        if (auto it = masks.find (c.id); it != masks.end())
        {
            it->second->setTextToShowWhenEmpty (c.numeric ? ">3" : T ("Filter …"), juce::Colours::grey);
            it->second->setTooltip (T (c.tip) + juce::String (c.numeric ? T (" – Maske: 5, >3, <=10, 2-4") : T (" – Maske: Text; mehrere Wörter = und; !Wort = ohne")));
        }
    }
    table.repaint();
}

void SidTable::refresh()
{
    inRefresh = true;
    const int keepId = lastSelectedId;
    auto sel = selectedIds();
    rows.clear();
    rows.reserve (arch.entries().size());
    for (auto& e : arch.entries())
        if (passes (*e)) rows.push_back (e.get());
    sortRows();
    table.updateContent();
    table.deselectAllRows();
    for (int id : sel)
    {
        const int r = rowOf (id);
        if (r >= 0) table.selectRow (r, false, false);
    }
    if (sel.empty() && keepId != 0)
        if (const int r = rowOf (keepId); r >= 0) table.selectRow (r, false, true);
    table.repaint();
    inRefresh = false;
    if (onListChanged) onListChanged();
}

bool SidTable::passes (const Entry& e) const
{
    if (onlyFav && ! e.favourite) return false;
    if (minRating > 0 && e.rating < minRating) return false;
    switch (onlyType)
    {
        case 1: if (! e.has (fHvsc)) return false; break;
        case 2: if (e.has (fHvsc)) return false; break;
        case 3: if (! e.has (fHasStil)) return false; break;
        case 4: if (! e.has (fHasBug)) return false; break;
        case 5: if (! e.has (fSameFile)) return false; break;
        default: break;
    }
    if (globalSearch.isNotEmpty())
    {
        juce::StringArray terms;
        terms.addTokens (globalSearch, " ", "\"");
        terms.removeEmptyStrings();
        for (auto t : terms)
        {
            bool neg = false;
            if (t.startsWithChar ('!')) { neg = true; t = t.substring (1); }
            t = t.unquoted();
            if (t.isEmpty()) continue;
            const bool hit = e.title.containsIgnoreCase (t) || e.author.containsIgnoreCase (t) || e.released.containsIgnoreCase (t) || e.composer.containsIgnoreCase (t)
                          || e.country.containsIgnoreCase (t) || e.name.containsIgnoreCase (t) || e.rel.containsIgnoreCase (t) || e.stilFirst.containsIgnoreCase (t)
                          || (e.area.containsIgnoreCase (t));
            if (hit == neg) return false;
        }
    }
    for (auto& m : masks)
    {
        const auto text = m.second->getText();
        if (text.isEmpty()) continue;
        if (! matchesMask (e, m.first, text)) return false;
    }
    return true;
}

void SidTable::sortRows()
{
    const int col = sortCol;
    const bool fwd = sortForward;
    const bool numeric = colDef (col).numeric || col == cFav || col == cRating || col == cReleased;
    auto cmpText = [] (const juce::String& a, const juce::String& b) { return a.compareNatural (b, false); };
    std::stable_sort (rows.begin(), rows.end(), [&] (const Entry* a, const Entry* b)
    {
        int c = 0;
        if (numeric)
        {
            const double x = numericValue (*a, col), y = numericValue (*b, col);
            c = x < y ? -1 : (x > y ? 1 : 0);
            if (col == cFav || col == cRating) c = -c;   // best first when ascending
        }
        else
            c = cmpText (cellText (*a, col), cellText (*b, col));
        if (c == 0) c = cmpText (cellText (*a, cTitle), cellText (*b, cTitle));
        return fwd ? c < 0 : c > 0;
    });
}

void SidTable::setGlobalSearch (const juce::String& s) { globalSearch = s.trim(); refresh(); }
void SidTable::setOnlyFavourites (bool b) { onlyFav = b; refresh(); }
void SidTable::setMinRating (int stars) { minRating = stars; refresh(); }
void SidTable::setOnlyType (int t) { onlyType = t; refresh(); }

void SidTable::clearMasks()
{
    for (auto& m : masks) m.second->clear();
    globalSearch = {};
    onlyFav = false; minRating = 0; onlyType = 0;
    refresh();
}

bool SidTable::hasFilters() const
{
    if (onlyFav || minRating > 0 || onlyType != 0 || globalSearch.isNotEmpty()) return true;
    for (auto& m : masks) if (m.second->getText().isNotEmpty()) return true;
    return false;
}

Entry* SidTable::selectedEntry() const { return rowEntry (table.getLastRowSelected()); }

std::vector<int> SidTable::selectedIds() const
{
    std::vector<int> ids;
    auto sel = table.getSelectedRows();
    for (int i = 0; i < sel.size(); ++i)
        if (auto* e = rowEntry (sel[i])) ids.push_back (e->id);
    return ids;
}

std::vector<Entry*> SidTable::selectedEntries() const
{
    std::vector<Entry*> v;
    auto sel = table.getSelectedRows();
    for (int i = 0; i < sel.size(); ++i)
        if (auto* e = rowEntry (sel[i])) v.push_back (e);
    return v;
}

int SidTable::rowOf (int id) const
{
    for (size_t i = 0; i < rows.size(); ++i) if (rows[i]->id == id) return (int) i;
    return -1;
}

void SidTable::selectEntry (int id)
{
    const int r = rowOf (id);
    if (r >= 0) { table.selectRow (r); table.scrollToEnsureRowIsOnscreen (r); }
}

void SidTable::selectRow (int row)
{
    if (row < 0 || row >= (int) rows.size()) return;
    table.selectRow (row);
    table.scrollToEnsureRowIsOnscreen (row);
}

int SidTable::totalDurationOfShown (double& seconds) const
{
    seconds = 0;
    int n = 0;
    for (auto* e : rows) if (e->seconds > 0) { seconds += e->seconds; ++n; }
    return n;
}

juce::String SidTable::layoutString() const { return table.getHeader().toString() + "\n" + juce::String (sortCol) + "," + (sortForward ? "1" : "0"); }

void SidTable::restoreLayout (const juce::String& s)
{
    if (s.isEmpty()) return;
    auto lines = juce::StringArray::fromLines (s);
    if (lines.size() >= 1 && lines[0].isNotEmpty())
    {
        auto& hdr = table.getHeader();
        hdr.restoreFromString (lines[0]);
        // a layout saved before the "Verlauf" column existed: show the new column behind "Länge"
        if (! lines[0].contains ("id=\"" + juce::String (cLandscape) + "\""))
        {
            hdr.setColumnVisible (cLandscape, true);
            hdr.moveColumn (cLandscape, hdr.getIndexOfColumnId (cLength, false) + 1);
        }
    }
    if (lines.size() >= 2)
    {
        sortCol = lines[1].upToFirstOccurrenceOf (",", false, false).getIntValue();
        sortForward = lines[1].fromFirstOccurrenceOf (",", false, false) != "0";
        if (sortCol < 1 || sortCol >= cLastCol) sortCol = cTitle;
        table.getHeader().setSortColumnId (sortCol, sortForward);
    }
    layoutFooter();
}

void SidTable::resized()
{
    auto r = getLocalBounds();
    footer.setBounds (r.removeFromBottom (30));
    table.setBounds (r);
    layoutFooter();
}

void SidTable::layoutFooter()
{
    auto& hdr = table.getHeader();
    const int vx = table.getViewport()->getViewPositionX();
    for (auto& m : masks) m.second->setVisible (false);
    for (int i = 0; i < hdr.getNumColumns (true); ++i)
    {
        const int id = hdr.getColumnIdOfIndex (i, true);
        auto it = masks.find (id);
        if (it == masks.end()) continue;
        const auto r = hdr.getColumnPosition (i);
        it->second->setBounds (r.getX() - vx + 1, 3, std::max (20, r.getWidth() - 2), footer.getHeight() - 6);
        it->second->setVisible (r.getRight() - vx > 0 && r.getX() - vx < footer.getWidth());
    }
}

void SidTable::paint (juce::Graphics& g)
{
    g.setColour (theme().panel2);
    g.fillRect (footer.getBounds());
}

// ---- painting
void SidTable::paintRowBackground (juce::Graphics& g, int row, int w, int h, bool selected)
{
    const auto& th = theme();
    auto c = ui::rowColour (th, row);
    if (selected) c = th.accent.withAlpha (0.38f).withMultipliedSaturation (0.9f);
    g.setColour (c);
    g.fillRect (0, 0, w, h);
    if (auto* e = rowEntry (row); e != nullptr && e->id == playingId && ! selected)
    {
        g.setColour (th.track[2].withAlpha (0.16f));
        g.fillRect (0, 0, w, h);
    }
}

void SidTable::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool selected)
{
    auto* e = rowEntry (row);
    if (e == nullptr) return;
    // cell cursor (moves with the arrow keys): shows which field a click or typing edits
    struct Cursor
    {
        juce::Graphics& g; bool on; int w, h; juce::Colour c;
        ~Cursor() { if (on) { g.setColour (c); g.drawRect (0, 0, w, h, 2); } }
    } cursor { g, selected && isEditableCol (col) && col == curCol && row == table.getLastRowSelected(), w, h, theme().accent.withAlpha (0.9f) };
    const auto& th = theme();
    if (col == cRating)
    {
        const float sw = juce::jmin ((float) h - 6.0f, (float) w / 5.0f - 1.0f);
        for (int i = 0; i < 5; ++i)
        {
            juce::Path star;
            star.addStar ({ 4.0f + sw * ((float) i + 0.5f), (float) h * 0.5f }, 5, sw * 0.22f, sw * 0.5f);
            g.setColour (i < e->rating ? ui::starColour (th, i) : th.dim.withAlpha (0.25f));
            g.fillPath (star);
        }
        return;
    }
    if (col == cLandscape)
    {
        if (e->has (fMissing) || e->has (fError)) return;
        auto* vp = table.getViewport();
        const int rh = std::max (1, table.getRowHeight());
        lsState->from = vp->getViewPositionY() / rh;
        lsState->to = (vp->getViewPositionY() + vp->getViewHeight()) / rh;
        auto it = landscapes.find (e->id);
        if (it == landscapes.end()) requestLandscape (*e, row);
        drawLandscape (g, it != landscapes.end() ? it->second.get() : nullptr, w, h);
        return;
    }
    if (col == cFav)
    {
        ui::drawHeart (g, juce::Rectangle<float> (0, 0, (float) w, (float) h), e->favourite ? juce::Colour (0xffff4f7b) : th.dim.withAlpha (0.22f));
        return;
    }
    auto text = cellText (*e, col);
    auto colour = th.text;
    if (e->has (fMissing)) colour = th.dim;
    else if (e->has (fProtected)) colour = th.text.interpolatedWith (juce::Colour (0xffe0503c), 0.55f);
    if (col == cNote && text.isNotEmpty()) colour = th.track[3];
    if (col == cStil || col == cPath || col == cCountry) colour = colour.withMultipliedAlpha (0.8f);
    if (col == cTitle && e->title.isEmpty()) colour = th.dim;
    if (col == cSid || col == cClock || col == cType) colour = th.dim.interpolatedWith (th.text, 0.5f);
    g.setColour (colour);
    g.setFont (ui::font (13.0f));
    const auto just = colDef (col).numeric ? juce::Justification::centredRight : juce::Justification::centredLeft;
    g.drawText (text.replaceCharacters ("\n\r", "  "), juce::Rectangle<int> (0, 0, w, h).reduced (5, 0), just, true);
    if (e->has (fMissing) && col == cTitle)
    {
        g.setColour (th.dim);
        g.drawHorizontalLine (h / 2, 4.0f, juce::jmin ((float) w - 4.0f, 6.0f + juce::GlyphArrangement::getStringWidth (ui::font (13.0f), text)));
    }
}

void SidTable::requestLandscape (const Entry& e, int row)
{
    if (! landscapeAsked.insert (e.id).second) return;
    const int id = e.id, song = e.startSong;
    const auto path = e.path;
    const double known = e.lengthOf (e.startSong);
    auto state = lsState;
    juce::Component::SafePointer<SidTable> safe (this);
    landscapePool.addJob ([safe, state, id, song, path, known, row]
    {
        auto forget = [safe, id] { juce::MessageManager::callAsync ([safe, id] { if (safe != nullptr) safe->landscapeAsked.erase (id); }); };
        // the row was scrolled out of sight while it waited: skip it (it is asked again when it comes back)
        if (! state->alive || row < state->from.load() - 3 || row > state->to.load() + 3) { forget(); return; }
        const auto si = parseSidFile (juce::File (path), true);
        if (! si.valid) { forget(); return; }
        const double want = known > 0 ? std::min (known + 1.0, 240.0) : 120.0;
        auto t = computeTrace (si, std::max (1, song), want, [state] { return ! state->alive.load(); }, 6.0);
        if (! state->alive) return;
        auto l = std::make_shared<Landscape>();
        const int n = Landscape::kBuckets;
        for (auto& v : l->height) v.assign ((size_t) n, 0);
        if (t != nullptr && t->frames > 0)
        {
            // mean pitch of the sounding frames per time bucket; every voice uses its own pitch range, so the ridges stay lively
            for (int v = 0; v < 3; ++v)
            {
                std::vector<float> mean ((size_t) n, -1.0f);
                float lo = 1000.0f, hi = -1000.0f;
                for (int b = 0; b < n; ++b)
                {
                    const juce::uint32 f0 = (juce::uint32) ((juce::int64) b * t->frames / n);
                    const juce::uint32 f1 = std::max<juce::uint32> (f0 + 1, (juce::uint32) ((juce::int64) (b + 1) * t->frames / n));
                    double sum = 0; int cnt = 0;
                    for (juce::uint32 f = f0; f < f1 && f < t->frames; ++f) if (t->v[v].note[f] >= 0.0f) { sum += t->v[v].note[f]; ++cnt; }
                    if (cnt > 0) { mean[(size_t) b] = (float) (sum / cnt); lo = std::min (lo, mean[(size_t) b]); hi = std::max (hi, mean[(size_t) b]); }
                }
                const float range = std::max (10.0f, hi - lo);
                for (int b = 0; b < n; ++b)
                    if (mean[(size_t) b] >= 0.0f)
                        l->height[(size_t) v][(size_t) b] = (juce::uint8) juce::jlimit (1, 255, 40 + (int) (215.0f * (mean[(size_t) b] - lo) / range));
            }
        }
        juce::MessageManager::callAsync ([safe, id, l]
        {
            if (safe == nullptr) return;
            auto& self = *safe;
            if (self.landscapes.size() > 20000) self.landscapes.clear();
            self.landscapes[id] = l;
            const int r = self.rowOf (id);
            if (r >= 0) self.table.repaintRow (r);
        });
    });
}

void SidTable::drawLandscape (juce::Graphics& g, const Landscape* l, int w, int h)
{
    const auto& th = theme();
    const bool light = th.bg.getBrightness() > 0.5f;
    const float pad = 2.0f, bandH = ((float) h - 2.0f * pad) / 3.0f;
    for (int v = 0; v < 3; ++v)
    {
        const float top = pad + bandH * (float) v, base = top + bandH - 1.0f, span = bandH - 2.0f;
        const auto vc = voiceColour (v, light);
        if (l == nullptr)
        {
            g.setColour (th.dim.withAlpha (0.22f));
            g.drawHorizontalLine ((int) base, 4.0f, (float) w - 4.0f);
            continue;
        }
        const auto& hs = l->height[(size_t) v];
        const float x0 = 4.0f, x1 = (float) w - 4.0f;
        juce::Path fill, line;
        fill.startNewSubPath (x0, base);
        bool open = false;
        for (int i = 0; i < Landscape::kBuckets; ++i)
        {
            const float x = x0 + (x1 - x0) * ((float) i + 0.5f) / (float) Landscape::kBuckets;
            const float y = base - span * (float) hs[(size_t) i] / 255.0f;
            fill.lineTo (x, y);
            if (hs[(size_t) i] > 0) { if (! open) { line.startNewSubPath (x, y); open = true; } else line.lineTo (x, y); }
            else open = false;
        }
        fill.lineTo (x1, base);
        fill.closeSubPath();
        g.setColour (vc.withAlpha (0.45f));
        g.fillPath (fill);
        g.setColour (vc.withAlpha (0.95f));
        g.strokePath (line, juce::PathStrokeType (1.0f));
        g.setColour (th.dim.withAlpha (0.18f));
        g.drawHorizontalLine ((int) base, x0, x1);
    }
}

void SidTable::sortOrderChanged (int col, bool forwards)
{
    sortCol = col;
    sortForward = forwards;
    refresh();
}

void SidTable::cellClicked (int row, int col, const juce::MouseEvent& e)
{
    auto* en = rowEntry (row);
    if (en == nullptr) return;
    if (e.mods.isPopupMenu())
    {
        if (onMenu) onMenu (*en, col, e.getScreenPosition());
        return;
    }
    if (col == cFav)
    {
        arch.setFavourite (*en, ! en->favourite);
        table.repaintRow (row);
    }
    else if (isEditableCol (col) && ! en->has (fMissing))
    {
        // the last clicked row of a (multi) selection gets the editor directly - the selection stays as it is
        pendingId = en->id;
        pendingCol = col;
        clickTimer.startTimer (30);
    }
    else if (col == cRating)
    {
        const int idx = table.getHeader().getIndexOfColumnId (col, true);
        const auto cell = table.getHeader().getColumnPosition (idx);
        const float sw = juce::jmin ((float) table.getRowHeight() - 6.0f, (float) cell.getWidth() / 5.0f - 1.0f);
        const int star = juce::jlimit (0, 5, (int) ((float) (e.x - cell.getX() - 4) / sw) + 1);
        arch.setRating (*en, star == en->rating ? star - 1 : star);
        table.repaintRow (row);
    }
}

void SidTable::cellDoubleClicked (int row, int col, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || col == cFav || col == cRating) return;
    pendingId = 0; clickTimer.stopTimer();   // no field opens after a double click
    if (onPlay) onPlay (rowEntry (row));
}

void SidTable::selectedRowsChanged (int)
{
    if (inRefresh) return;
    if (editor != nullptr) finishEdit (true, false);
    auto* e = selectedEntry();
    if (e != nullptr) lastSelectedId = e->id;
    if (onSelect) onSelect (e);
}

void SidTable::returnKeyPressed (int row)
{
    // Return on an editable cell opens it (content selected), elsewhere it plays
    auto* e = rowEntry (row);
    if (e == nullptr) return;
    if (isEditableCol (curCol) && ! e->has (fMissing)) { startEdit (row, curCol); return; }
    if (onPlay) onPlay (e);
}

juce::String SidTable::getCellTooltip (int row, int col)
{
    auto* e = rowEntry (row);
    if (e == nullptr) return {};
    if (col == cLength && e->lengths.size() > 1)
    {
        juce::String s;
        for (size_t i = 0; i < e->lengths.size() && i < 24; ++i)
            s << T ("Song %1: %2", (int) i + 1, formatTime (e->lengths[i])) << "\n";
        if (e->lengths.size() > 24) s << U ("…");
        return s.trimEnd();
    }
    if (col == cStil || col == cNote) return e->stilFirst;
    if (col == cPath || col == cFile) return e->path;
    return {};
}

// ================================================================================================ inline editing
// suggestions while typing: values of the column that contain the text
struct SidTable::SuggestBox : public juce::Component
{
    explicit SuggestBox (SidTable& o) : owner (o) { setAlwaysOnTop (true); setWantsKeyboardFocus (false); setMouseClickGrabsKeyboardFocus (false); }
    SidTable& owner;
    juce::StringArray items;
    juce::String query;
    int selected = -1;
    static constexpr int rowH = 20;
    void paint (juce::Graphics& g) override
    {
        const auto& th = owner.theme();
        g.setColour (th.panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (th.accent);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
        for (int i = 0; i < items.size(); ++i)
        {
            auto r = juce::Rectangle<int> (2, 2 + i * rowH, getWidth() - 4, rowH);
            if (i == selected) { g.setColour (th.accent.withAlpha (0.4f)); g.fillRoundedRectangle (r.toFloat(), 3.0f); }
            const auto& t = items[i];
            const int at = t.indexOfIgnoreCase (query);
            juce::AttributedString as;
            as.setJustification (juce::Justification::centredLeft);
            as.setWordWrap (juce::AttributedString::none);
            auto add = [&] (const juce::String& part, juce::Colour c, bool bold) { if (part.isNotEmpty()) as.append (part, ui::font (13.0f, bold), c); };
            if (at >= 0 && query.isNotEmpty())
            {
                add (t.substring (0, at), th.text, false);
                add (t.substring (at, at + query.length()), th.accent, true);
                add (t.substring (at + query.length()), th.text, false);
            }
            else add (t, th.text, false);
            as.draw (g, r.reduced (6, 0).toFloat());
        }
    }
    void mouseMove (const juce::MouseEvent& e) override { const int i = (e.y - 2) / rowH; if (i != selected && i >= 0 && i < items.size()) { selected = i; repaint(); } }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const int i = (e.y - 2) / rowH;
        if (i >= 0 && i < items.size())
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SidTable> (&owner), i] { if (safe) safe->acceptSuggestion (i); });
    }
};

// cell editor: arrow keys / Tab / Return go to the table first (cell navigation)
struct SidTable::CellEditor : juce::TextEditor
{
    std::function<bool (const juce::KeyPress&)> onKey;
    std::function<void()> onClick;
    std::function<bool()> onDoubleClick;   // true = handled (the tune plays), else the usual word selection
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (! (onDoubleClick && onDoubleClick())) juce::TextEditor::mouseDoubleClick (e); }
    bool keyPressed (const juce::KeyPress& k) override { return (onKey && onKey (k)) || juce::TextEditor::keyPressed (k); }
    void mouseDown (const juce::MouseEvent& e) override { if (onClick) onClick(); juce::TextEditor::mouseDown (e); }
};

bool SidTable::isEditableCol (int col) const
{
    return (col == cTitle || col == cAuthor || col == cReleased) && table.getHeader().isColumnVisible (col);
}

juce::String SidTable::rawValue (const Entry& e, int col) const
{
    const auto& v = col == cTitle ? e.title : col == cAuthor ? e.author : e.released;
    return v == "<?>" ? juce::String() : v;
}

int SidTable::nextCol (int col, int dir, bool editableOnly) const
{
    auto& h = table.getHeader();
    const int n = h.getNumColumns (true);
    int idx = h.getIndexOfColumnId (col, true);
    if (idx < 0) idx = dir > 0 ? -1 : n;
    for (int i = idx + dir; i >= 0 && i < n; i += dir)
    {
        const int c = h.getColumnIdOfIndex (i, true);
        if (! editableOnly || isEditableCol (c)) return c;
    }
    return col;
}

void SidTable::scrollToColumn (int col)
{
    auto& h = table.getHeader();
    const int idx = h.getIndexOfColumnId (col, true);
    if (idx < 0) return;
    auto* vp = table.getViewport();
    const auto r = h.getColumnPosition (idx);
    int x = vp->getViewPositionX();
    const int w = vp->getViewWidth();
    if (r.getX() < x) x = r.getX();
    else if (r.getRight() > x + w) x = juce::jmin (r.getX(), r.getRight() - w);
    if (x != vp->getViewPositionX()) vp->setViewPosition (x, vp->getViewPositionY());
}

bool SidTable::canEdit (int row, int col) const
{
    auto* e = rowEntry (row);
    return e != nullptr && isEditableCol (col) && ! e->has (fMissing) && ! e->has (fProtected);
}

// moves the cell cursor; with edit = true the target cell opens with its content selected
void SidTable::goTo (int row, int col, bool edit)
{
    if (rows.empty()) return;
    row = juce::jlimit (0, (int) rows.size() - 1, row);
    curCol = col;
    if (table.getNumSelectedRows() != 1 || table.getSelectedRow() != row) table.selectRow (row);
    table.scrollToEnsureRowIsOnscreen (row);
    scrollToColumn (col);
    table.repaint();
    if (edit && canEdit (row, col)) startEdit (row, col);
    else table.grabKeyboardFocus();
}

void SidTable::enterCaretMode()
{
    if (editor == nullptr) return;
    caretMode = true;
    editor->moveCaretToEnd();
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::TextEditor> (editor.get())] { if (safe != nullptr) safe->moveCaretToEnd(); });
}

void SidTable::startPendingEdit()
{
    if (juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
    {
        if (auto* dc = juce::DragAndDropContainer::findParentDragContainerFor (this); dc != nullptr && dc->isDragAndDropActive()) { pendingId = 0; return; }
        clickTimer.startTimer (30);   // wait for the mouse-up
        return;
    }
    const int id = pendingId, col = pendingCol;
    pendingId = 0;
    if (id == 0 || (editor != nullptr && editId == id && editCol == col)) return;
    const int r = rowOf (id);
    if (r >= 0 && table.isRowSelected (r)) startEdit (r, col);
}

void SidTable::startEdit (int row, int col, const juce::String& typed)
{
    auto* e = rowEntry (row);
    if (e == nullptr || ! isEditableCol (col)) return;
    if (e->has (fMissing)) { if (onStatus) onStatus (T ("Datei fehlt – nicht bearbeitbar")); return; }
    if (e->has (fProtected)) { if (onStatus) onStatus (arch.protectedMessage()); return; }
    finishEdit (true, false);
    table.scrollToEnsureRowIsOnscreen (row);
    editId = e->id; editCol = col; curCol = col;
    scrollToColumn (col);
    auto ce = std::make_unique<CellEditor>();
    ce->onKey = [this] (const juce::KeyPress& k) { return editorKey (k); };
    ce->onClick = [this] { caretMode = true; };
    // a double click on a field nobody has typed into plays the tune, like a double click on any other cell
    ce->onDoubleClick = [this]
    {
        if (editor == nullptr || editor->getText() != editOriginal) return false;
        const int id = editId;
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SidTable> (this), id]
        {
            if (safe == nullptr) return;
            safe->finishEdit (false);
            safe->table.grabKeyboardFocus();
            if (auto* en = safe->arch.byId (id); en != nullptr && safe->onPlay) safe->onPlay (en);
        });
        return true;
    };
    caretMode = false;
    editor = std::move (ce);
    editor->setFont (ui::font (13.0f));
    editor->setIndents (4, 3);
    editor->setInputRestrictions (32);
    editor->setText (rawValue (*e, col), false);
    editor->addListener (this);
    editOriginal = editor->getText();
    knownValues.clear();
    {
        std::map<juce::String, int> count;
        for (auto& x : arch.entries())
            if (auto v = rawValue (*x, col).trim(); v.isNotEmpty()) ++count[v];
        knownValues.assign (count.begin(), count.end());
    }
    const auto& th = theme();
    editor->setColour (juce::TextEditor::outlineColourId, th.accent);
    editor->setColour (juce::TextEditor::focusedOutlineColourId, th.accent);
    editor->setColour (juce::TextEditor::backgroundColourId, th.bg);
    editor->setColour (juce::TextEditor::textColourId, th.text);
    editor->setColour (juce::TextEditor::highlightColourId, th.accent);
    editor->setColour (juce::TextEditor::highlightedTextColourId, th.bg.getPerceivedBrightness() < 0.5f ? juce::Colours::black : juce::Colours::white);
    editor->setColour (juce::CaretComponent::caretColourId, th.text);
    editor->setSelectAllWhenFocused (true);
    editor->applyColourToAllText (th.text);
    // the editor lives in the scrolled row area: it moves with the mouse wheel / scrollbars
    auto* content = table.getViewport()->getViewedComponent();
    editor->setAlwaysOnTop (true);
    content->addAndMakeVisible (*editor);
    editor->setBounds (content->getLocalArea (&table, table.getCellPosition (col, row, true)).expanded (0, 1));
    editor->grabKeyboardFocus();
    table.repaint();
    if (typed.isNotEmpty())   // typing started the edit: replace the content with that character
    {
        editor->setText (typed, false);
        updateSuggestions();
        editor->moveCaretToEnd();
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::TextEditor> (editor.get())]
        { if (safe != nullptr && ! safe->hasKeyboardFocus (false)) { safe->grabKeyboardFocus(); safe->moveCaretToEnd(); } });
        return;
    }
    editor->selectAll();
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::TextEditor> (editor.get())]
    { if (safe != nullptr) { if (! safe->hasKeyboardFocus (false)) safe->grabKeyboardFocus(); safe->selectAll(); } });
}

void SidTable::textEditorEscapeKeyPressed (juce::TextEditor&)
{
    finishEdit (false);
    table.grabKeyboardFocus();
    table.repaint();
}

void SidTable::textEditorFocusLost (juce::TextEditor&)
{
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SidTable> (this)]
    {
        if (safe == nullptr) return;
        if (safe->suggest != nullptr && safe->suggest->isVisible() && safe->suggest->isMouseOverOrDragging()) return;
        safe->finishEdit (true, true);
    });
}

void SidTable::finishEdit (bool commit, bool toSelection)
{
    hideSuggestions();
    if (editor == nullptr) return;
    auto ed = std::move (editor);
    ed->removeListener (this);
    const auto value = ed->getText().trim();
    const int id = editId, col = editCol;
    ed.reset();
    if (! commit) return;
    auto* e = arch.byId (id);
    if (e == nullptr) return;
    std::vector<int> ids;
    const auto sel = selectedEntries();
    const bool multi = toSelection && sel.size() > 1 && std::find (sel.begin(), sel.end(), e) != sel.end();
    for (auto* x : multi ? sel : std::vector<Entry*> { e })
        if (! x->has (fMissing) && ! x->has (fProtected) && value != rawValue (*x, col).trim()) ids.push_back (x->id);
    if (ids.empty()) return;
    if (ids.size() == 1) { applyValue (ids, col, value); return; }
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, T ("Mehrere Dateien ändern"),
        T ("%1 = „%2“\n\nin %3 markierte Dateien schreiben?", T (colDef (col).name), value, (int) ids.size()),
        T ("Alle %1 ändern", (int) ids.size()), T ("Abbrechen"), this,
        juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<SidTable> (this), ids, col, value] (int r)
        { if (r != 0 && safe != nullptr) safe->applyValue (ids, col, value); }));
}

void SidTable::copyValueToSelection (int col, const Entry& from)
{
    const auto value = rawValue (from, col).trim();
    std::vector<int> ids;
    for (auto* x : selectedEntries())
        if (! x->has (fMissing) && ! x->has (fProtected) && value != rawValue (*x, col).trim()) ids.push_back (x->id);
    if (! ids.empty()) applyValue (ids, col, value);
}

void SidTable::applyValue (const std::vector<int>& ids, int col, const juce::String& value)
{
    const int field = col == cTitle ? Archive::fieldTitle : col == cAuthor ? Archive::fieldAuthor : Archive::fieldReleased;
    int ok = 0;
    juce::String errors;
    Archive::UndoGroup g (arch, ids.size() == 1 ? T ("%1: %2", T (colDef (col).name), arch.byId (ids[0]) != nullptr ? arch.byId (ids[0])->name : juce::String())
                                                : T ("%1 in %2 Dateien", T (colDef (col).name), (int) ids.size()));
    for (int id : ids)
        if (auto* x = arch.byId (id))
        {
            juce::String err;
            if (arch.setSidField (*x, field, value, err)) ++ok;
            else errors << x->name << ": " << err << "\n";
        }
    if (errors.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "SIDiMIDI", T ("Nicht geschrieben:") + "\n" + errors.substring (0, 2000));
    if (onStatus && ok > 0)
        onStatus (ok == 1 ? T ("Gespeichert in %1 (Original einmalig gesichert in %2)", arch.byId (ids[0]) != nullptr ? arch.byId (ids[0])->name : juce::String(), arch.backupFolder().getFullPathName())
                          : T ("%1 in %2 Dateien geschrieben (Originale einmalig gesichert)", T (colDef (col).name), ok));
    table.repaint();
    if (onEdited) onEdited();
}

bool SidTable::navKey (const juce::KeyPress& k)
{
    if (editor != nullptr || rows.empty()) return false;
    const auto mods = k.getModifiers();
    if (mods.isAnyModifierKeyDown()) return false;   // Shift + arrows: extend the selection as usual
    const int code = k.getKeyCode();
    const int row = juce::jmax (0, table.getLastRowSelected());
    int to = row;
    if (code == juce::KeyPress::upKey) to = row - 1;
    else if (code == juce::KeyPress::downKey) to = row + 1;
    else if (code == juce::KeyPress::pageUpKey) to = row - pageRows();
    else if (code == juce::KeyPress::pageDownKey) to = row + pageRows();
    else return false;
    to = juce::jlimit (0, (int) rows.size() - 1, to);
    goTo (to, curCol, true);
    return true;
}

bool SidTable::editorKey (const juce::KeyPress& k)
{
    if (editor == nullptr) return false;
    const int row = rowOfId (editId);
    if (row < 0) return false;
    const int col = editCol;
    const auto code = k.getKeyCode();
    const bool plain = ! k.getModifiers().isCommandDown() && ! k.getModifiers().isAltDown();
    if (suggest != nullptr && suggest->isVisible())
    {
        auto& sb = *suggest;
        if (code == juce::KeyPress::downKey) { sb.selected = juce::jmin (sb.items.size() - 1, sb.selected + 1); sb.repaint(); return true; }
        if (code == juce::KeyPress::upKey) { sb.selected = juce::jmax (-1, sb.selected - 1); sb.repaint(); return true; }
        if (code == juce::KeyPress::escapeKey) { hideSuggestions(); return true; }
        if ((code == juce::KeyPress::returnKey || code == juce::KeyPress::tabKey) && sb.selected >= 0)
        {
            editor->setText (sb.items[sb.selected], false);
            hideSuggestions();
        }
        else if (code == juce::KeyPress::returnKey || code == juce::KeyPress::tabKey) hideSuggestions();
    }
    auto commitAndGo = [this] (int toRow, int toCol)
    {
        finishEdit (true, false);
        goTo (toRow, toCol, true);
    };
    if (code == juce::KeyPress::returnKey && plain)
    {
        const auto sel = selectedEntries();
        const int id = editId;
        const bool multi = sel.size() > 1 && std::any_of (sel.begin(), sel.end(), [id] (Entry* x) { return x->id == id; });
        if (multi) { finishEdit (true); return true; }                       // asks before writing into all selected files
        if (row + 1 >= (int) rows.size()) { finishEdit (true, false); goTo (row, col, false); return true; }
        commitAndGo (row + 1, col);
        return true;
    }
    if (code == juce::KeyPress::escapeKey) { textEditorEscapeKeyPressed (*editor); return true; }
    if (code == juce::KeyPress::tabKey)
    {
        const int c = nextCol (col, k.getModifiers().isShiftDown() ? -1 : 1, true);
        if (c != col) commitAndGo (row, c);
        return true;
    }
    // like a spreadsheet: an unchanged field lets the arrow keys walk from cell to cell; once something was typed,
    // left/right move the caret, up/down stay, Return saves
    if (code == juce::KeyPress::F2Key) { enterCaretMode(); return true; }
    const bool up = code == juce::KeyPress::upKey, down = code == juce::KeyPress::downKey;
    const bool left = code == juce::KeyPress::leftKey, right = code == juce::KeyPress::rightKey;
    const bool page = code == juce::KeyPress::pageUpKey || code == juce::KeyPress::pageDownKey;
    if (! (up || down || left || right || page)) return false;
    const bool modified = caretMode || editor->getText() != editOriginal;
    if (modified) return up || down || page;
    if (k.getModifiers().isAnyModifierKeyDown()) return false;
    int toRow = row, toCol = col;
    if (up) toRow = row - 1;
    else if (down) toRow = row + 1;
    else if (page) toRow = row + (code == juce::KeyPress::pageUpKey ? -pageRows() : pageRows());
    else toCol = nextCol (col, left ? -1 : 1, false);
    toRow = juce::jlimit (0, (int) rows.size() - 1, toRow);
    if (toRow == row && toCol == col) return true;
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SidTable> (this), toRow, toCol]
    {
        if (safe == nullptr) return;
        safe->finishEdit (false, false);
        safe->goTo (toRow, toCol, true);
    });
    return true;
}

bool SidTable::keyPressed (const juce::KeyPress& k)
{
    const int row = table.getLastRowSelected();
    if (k.getKeyCode() == juce::KeyPress::F2Key)   // like a spreadsheet: straight into the input phase, caret at the end
    {
        startEdit (row, isEditableCol (curCol) ? curCol : cTitle);
        enterCaretMode();
        return true;
    }
    if (editor != nullptr || row < 0 || rowEntry (row) == nullptr) return false;
    if (navKey (k)) return true;
    const auto code = k.getKeyCode();
    const auto mods = k.getModifiers();
    if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && ! mods.isAnyModifierKeyDown())
    {
        goTo (row, nextCol (curCol, code == juce::KeyPress::leftKey ? -1 : 1, false), true);
        return true;
    }
    if (code == juce::KeyPress::tabKey && ! mods.isCommandDown() && ! mods.isAltDown())
    {
        goTo (row, nextCol (curCol, mods.isShiftDown() ? -1 : 1, true), true);
        return true;
    }
    // typing on an editable cell starts editing with that character (space stays play / pause)
    const auto ch = k.getTextCharacter();
    if (ch > ' ' && ! mods.isCommandDown() && ! mods.isAltDown() && isEditableCol (curCol) && ! rowEntry (row)->has (fMissing))
    {
        startEdit (row, curCol, juce::String::charToString (ch));
        return true;
    }
    return false;
}

void SidTable::hideSuggestions()
{
    if (suggest != nullptr) suggest->setVisible (false);
}

void SidTable::updateSuggestions()
{
    if (editor == nullptr) { hideSuggestions(); return; }
    const auto q = editor->getText().trim();
    if (q.isEmpty() || editor->getText() == editOriginal) { hideSuggestions(); return; }
    std::vector<std::pair<juce::String, int>> list;
    for (auto& kv : knownValues)
        if (kv.first.containsIgnoreCase (q) && kv.first != q) list.push_back (kv);
    std::sort (list.begin(), list.end(), [&q] (const auto& a, const auto& b)
    {
        const bool sa = a.first.startsWithIgnoreCase (q), sb = b.first.startsWithIgnoreCase (q);
        if (sa != sb) return sa;
        return a.first.compareNatural (b.first) < 0;
    });
    if (list.empty()) { hideSuggestions(); return; }
    if (suggest == nullptr) { suggest = std::make_unique<SuggestBox> (*this); addChildComponent (*suggest); }
    suggest->items.clear();
    for (size_t i = 0; i < list.size() && i < 12; ++i) suggest->items.add (list[i].first);
    suggest->query = q;
    suggest->selected = -1;
    const auto er = getLocalArea (editor->getParentComponent(), editor->getBounds());
    int w = juce::jmax (er.getWidth(), 240);
    for (auto& s : suggest->items) w = juce::jmax (w, juce::jmin (520, juce::GlyphArrangement::getStringWidthInt (ui::font (13.0f, true), s) + 20));
    const int h = suggest->items.size() * SuggestBox::rowH + 4;
    auto r = juce::Rectangle<int> (er.getX(), er.getBottom() + 1, w, h);
    if (r.getBottom() > getHeight()) r.setY (er.getY() - h - 1);
    if (r.getRight() > getWidth()) r.setX (juce::jmax (0, getWidth() - w));
    suggest->setBounds (r);
    suggest->setVisible (true);
    suggest->toFront (false);
    suggest->repaint();
}

void SidTable::acceptSuggestion (int index)
{
    if (suggest == nullptr || editor == nullptr || index < 0 || index >= suggest->items.size()) return;
    editor->setText (suggest->items[index], false);
    hideSuggestions();
    editorKey (juce::KeyPress (juce::KeyPress::returnKey));   // like Return: save and one row down
}

void SidTable::backgroundClicked (const juce::MouseEvent&) { table.deselectAllRows(); }

void SidTable::timerCallback()
{
    stopTimer();
    if (masksChanged) { masksChanged = false; refresh(); }
}
} // namespace sm
