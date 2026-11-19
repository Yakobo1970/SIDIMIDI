#include "SidTable.h"
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
    auto& hdr = table.getHeader();
    int flags = juce::TableHeaderComponent::resizable | juce::TableHeaderComponent::sortable | juce::TableHeaderComponent::draggable | juce::TableHeaderComponent::appearsOnColumnMenu;
    for (auto& c : kCols)
        hdr.addColumn (T (c.name), c.id, c.width, 24, 600, c.visible ? (flags | juce::TableHeaderComponent::visible) : flags);
    hdr.setSortColumnId (sortCol, sortForward);
    hdr.addListener (watcherOf (watcher));
    table.getViewport()->getHorizontalScrollBar().addListener (watcherOf (watcher));

    for (auto& c : kCols)
    {
        if (c.id == cFav) continue;
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
    if (lines.size() >= 1 && lines[0].isNotEmpty()) table.getHeader().restoreFromString (lines[0]);
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
    auto c = (row & 1) ? th.panel : th.panel.interpolatedWith (th.panel2, 0.35f);
    if (selected) c = th.accent.withAlpha (0.38f).withMultipliedSaturation (0.9f);
    g.setColour (c);
    g.fillRect (0, 0, w, h);
    if (auto* e = rowEntry (row); e != nullptr && e->id == playingId && ! selected)
    {
        g.setColour (th.track[2].withAlpha (0.16f));
        g.fillRect (0, 0, w, h);
    }
}

void SidTable::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool)
{
    auto* e = rowEntry (row);
    if (e == nullptr) return;
    const auto& th = theme();
    if (col == cRating)
    {
        const float sw = juce::jmin ((float) h - 6.0f, (float) w / 5.0f - 1.0f);
        for (int i = 0; i < 5; ++i)
        {
            juce::Path star;
            star.addStar ({ 4.0f + sw * ((float) i + 0.5f), (float) h * 0.5f }, 5, sw * 0.22f, sw * 0.5f);
            g.setColour (i < e->rating ? juce::Colour (0xffffc933) : th.dim.withAlpha (0.25f));
            g.fillPath (star);
        }
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
    if (onPlay) onPlay (rowEntry (row));
}

void SidTable::selectedRowsChanged (int)
{
    if (inRefresh) return;
    auto* e = selectedEntry();
    if (e != nullptr) lastSelectedId = e->id;
    if (onSelect) onSelect (e);
}

void SidTable::returnKeyPressed (int row)
{
    if (onPlay) onPlay (rowEntry (row));
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

void SidTable::backgroundClicked (const juce::MouseEvent&) { table.deselectAllRows(); }

void SidTable::timerCallback()
{
    stopTimer();
    if (masksChanged) { masksChanged = false; refresh(); }
}
} // namespace sm
