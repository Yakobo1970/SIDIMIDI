#include "Archive.h"
#include <unordered_set>

namespace sm
{
namespace
{
constexpr int kDbMagic = 0x42444953;   // "SIDB"
constexpr int kDbVersion = 1;

juce::String stripFolderEnd (juce::String p)
{
    while (p.length() > 3 && (p.endsWithChar ('\\') || p.endsWithChar ('/'))) p = p.dropLastCharacters (1);
    return p;
}

bool insideFolder (const juce::String& path, const juce::String& folder)
{
    const auto f = stripFolderEnd (folder);
    if (! path.startsWithIgnoreCase (f)) return false;
    return path.length() == f.length() || path[f.length()] == '\\' || path[f.length()] == '/';
}

juce::String firstStilLine (const juce::String& stil)
{
    for (auto line : juce::StringArray::fromLines (stil))
    {
        line = line.trim();
        if (line.isEmpty() || line.startsWith ("(#")) continue;
        return line.length() > 160 ? line.substring (0, 160) : line;
    }
    return {};
}
} // namespace

juce::String lengthText (const Entry& e)
{
    return e.seconds > 0 ? formatTime (e.seconds) : juce::String();
}

std::unique_ptr<Entry> makeEntry (const juce::File& file, const SidInfo& si, const Hvsc* hv)
{
    auto e = std::make_unique<Entry>();
    e->path = file.getFullPathName();
    e->name = file.getFileName();
    e->folder = file.getParentDirectory().getFullPathName();
    e->size = file.getSize();
    e->mtime = file.getLastModificationTime().toMilliseconds();
    if (! si.valid) { e->flags |= fError; e->error = juce::String::fromUTF8 ("Kein gültiger PSID/RSID-Kopf"); return e; }
    e->md5 = si.md5;
    e->title = si.title; e->author = si.author; e->released = si.released;
    e->year = si.year(); e->songs = si.songs; e->startSong = si.startSong; e->version = si.version;
    e->clock = (juce::uint8) si.clock; e->model = (juce::uint8) si.model; e->sidCount = (juce::uint8) si.sidCount;
    e->load = si.effLoad; e->init = si.effInit(); e->play = si.playAddr;
    e->dataSize = (int) (si.fileSize - si.dataOffset);
    if (si.rsid) e->flags |= fRsid;
    if (si.sidCount > 1) e->flags |= fMulti;
    if (si.mus) e->flags |= fMus;
    if (si.basicTune) e->flags |= fBasic;
    if (hv != nullptr)
    {
        e->rel = hv->relPath (file);
        if (e->rel.isNotEmpty())
        {
            e->flags |= fHvsc;
            if (auto* l = hv->lengths (si.md5)) { e->lengths = *l; e->seconds = (float) e->lengthOf (si.startSong); }
            const auto stil = hv->stil (e->rel);
            if (stil.isNotEmpty()) { e->flags |= fHasStil; e->stilFirst = firstStilLine (stil); }
            if (hv->hasBug (e->rel)) e->flags |= fHasBug;
            auto parts = juce::StringArray::fromTokens (e->rel, "/", {});
            parts.removeEmptyStrings();
            if (parts.size() >= 2) e->area = parts[0];
            if (auto* m = hv->musicianFor (e->rel)) { e->composer = m->handle; e->country = m->country; }
            else if (parts.size() >= 4 && parts[0] == "MUSICIANS") e->composer = parts[2].replace ("_", " ");
        }
    }
    return e;
}

// ================================================================================================ scanner
struct Archive::Scanner
{
    struct Job { juce::File file; std::shared_ptr<Hvsc> hvsc; };
    Scanner (Archive& a, bool fullRescan, juce::StringArray folderList,
             std::unordered_map<juce::String, std::pair<juce::int64, juce::int64>> knownEntries)
        : arch (a), full (fullRescan), folders (std::move (folderList)), known (std::move (knownEntries)) {}

    Archive& arch;
    bool full;
    juce::StringArray folders;
    std::unordered_map<juce::String, std::pair<juce::int64, juce::int64>> known;
    std::atomic<bool> stop { false }, finished { false };
    std::thread thread;
    juce::CriticalSection seenLock;
    std::unordered_set<juce::String> seen;
    std::vector<Job> jobs;
    std::atomic<size_t> next { 0 };
    int newFiles = 0;
    std::atomic<int> analysed { 0 };

    void start() { thread = std::thread ([this] { run(); }); }
    ~Scanner() { stop = true; if (thread.joinable()) thread.join(); }

    void run()
    {
        // 1. find the files
        {
            const juce::ScopedLock sl (arch.pendingLock);
            arch.scanPhase = juce::String::fromUTF8 ("Suche SID-Dateien …");
        }
        for (const auto& folder : folders)
        {
            if (stop) break;
            auto hv = arch.ensureHvsc (juce::File (folder));
            for (const auto& de : juce::RangedDirectoryIterator (juce::File (folder), true, "*.sid", juce::File::findFiles))
            {
                if (stop) break;
                jobs.push_back ({ de.getFile(), hv });
                arch.total = (int) jobs.size();
            }
        }
        // 2. read the headers on several threads
        {
            const juce::ScopedLock sl (arch.pendingLock);
            arch.scanPhase = juce::String::fromUTF8 ("Lese SID-Köpfe …");
        }
        arch.total = (int) jobs.size();
        arch.done = 0;
        const int n = juce::jlimit (1, 8, (int) std::thread::hardware_concurrency());
        std::vector<std::thread> workers;
        for (int i = 0; i < n; ++i)
            workers.emplace_back ([this] { work(); });
        for (auto& w : workers) w.join();
        finished = true;
    }

    void work()
    {
        std::vector<juce::String> localSeen;
        std::vector<std::unique_ptr<Entry>> batch;
        auto flush = [&]
        {
            if (batch.empty() && localSeen.empty()) return;
            {
                const juce::ScopedLock sl (seenLock);
                for (auto& s : localSeen) seen.insert (s);
            }
            {
                const juce::ScopedLock sl (arch.pendingLock);
                for (auto& e : batch) arch.pending.push_back (std::move (e));
            }
            batch.clear(); localSeen.clear();
        };
        for (;;)
        {
            if (stop) break;
            const auto i = next.fetch_add (1);
            if (i >= jobs.size()) break;
            const auto& job = jobs[i];
            const auto path = job.file.getFullPathName();
            localSeen.push_back (path);
            bool skip = false;
            if (! full)
            {
                auto it = known.find (path);
                if (it != known.end() && it->second.first == job.file.getSize() && it->second.second == job.file.getLastModificationTime().toMilliseconds())
                    skip = true;
            }
            if (! skip)
            {
                const auto si = parseSidFile (job.file, false);
                batch.push_back (makeEntry (job.file, si, job.hvsc.get()));
                ++analysed;
            }
            ++arch.done;
            if (batch.size() >= 64 || localSeen.size() >= 256) flush();
        }
        flush();
    }
};

// ================================================================================================ archive
Archive::Archive()
{
    load();
    startTimerHz (10);
    startHvscLoad();
}

Archive::~Archive()
{
    stopTimer();
    stopScan();
    if (hvscThread.joinable()) hvscThread.join();
    if (dirty || settingsDirty) save();
}

juce::File Archive::dataFolder() const
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("SIDIMIDI_DB", {});
    if (env.isNotEmpty()) return juce::File (env).getParentDirectory();
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SIDIMIDI");
}

juce::File Archive::databaseFile() const
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("SIDIMIDI_DB", {});
    if (env.isNotEmpty()) return juce::File (env);
    return dataFolder().getChildFile ("sidarchiv.sdb");
}

juce::File Archive::settingsFile() const
{
    const auto db = databaseFile();
    return db.getSiblingFile (db.getFileNameWithoutExtension() + ".settings.xml");
}

Entry* Archive::byId (int id) const { auto it = idIndex.find (id); return it == idIndex.end() ? nullptr : it->second; }
Entry* Archive::byPath (const juce::String& path) const { auto it = pathIndex.find (path); return it == pathIndex.end() ? nullptr : it->second; }

// ---- folders / protection
juce::String Archive::addFolder (const juce::File& dir)
{
    const auto path = stripFolderEnd (dir.getFullPathName());
    for (const auto& f : folders)
        if (insideFolder (path, f)) return f;
    // folders inside the new one are covered from now on
    for (int i = folders.size(); --i >= 0;)
        if (insideFolder (folders[i], path)) folders.remove (i);
    folders.add (path);
    settings.setProperty ("folders", folders.joinIntoString ("\n"), nullptr);
    settingsDirty = true;
    changed();
    startHvscLoad();
    return path;
}

juce::StringArray Archive::protectedFolders() const
{
    return juce::StringArray::fromLines (settings.getProperty ("protected").toString());
}

bool Archive::isProtected (const juce::String& path) const
{
    for (const auto& f : protectedFolders())
        if (insideFolder (path, f)) return true;
    return false;
}

void Archive::setProtected (const juce::String& folder, bool on)
{
    auto prot = protectedFolders();
    prot.removeString (folder);
    if (on) prot.add (folder);
    settings.setProperty ("protected", prot.joinIntoString ("\n"), nullptr);
    settingsDirty = true;
    applyProtection();
    changed();
}

void Archive::markProtection (Entry& e, const juce::StringArray& prot) const
{
    bool p = false;
    for (const auto& f : prot) if (insideFolder (e.path, f)) { p = true; break; }
    if (p) e.flags |= fProtected; else e.flags &= ~(juce::uint32) fProtected;
}

void Archive::applyProtection()
{
    const auto prot = protectedFolders();
    for (auto& e : list) markProtection (*e, prot);
}

int Archive::removeWhere (const std::function<bool (const Entry&)>& pred)
{
    int removed = 0;
    for (size_t i = list.size(); i-- > 0;)
        if (pred (*list[i]))
        {
            pathIndex.erase (list[i]->path);
            idIndex.erase (list[i]->id);
            list.erase (list.begin() + (long) i);
            ++removed;
        }
    if (removed > 0) { dirty = true; markSameContent(); changed(); }
    return removed;
}

int Archive::removeFolder (const juce::String& path)
{
    stopScan();
    folders.removeString (path);
    settings.setProperty ("folders", folders.joinIntoString ("\n"), nullptr);
    setProtected (path, false);
    settingsDirty = true;
    return removeWhere ([&] (const Entry& e) { return insideFolder (e.path, path); });
}

int Archive::countInFolder (const juce::String& path) const
{
    int n = 0;
    for (auto& e : list) if (insideFolder (e->path, path)) ++n;
    return n;
}

int Archive::removeMissing() { return removeWhere ([] (const Entry& e) { return e.has (fMissing); }); }

bool Archive::isInsideFolders (const juce::File& f) const
{
    for (const auto& folder : folders) if (insideFolder (f.getFullPathName(), folder)) return true;
    return false;
}

// ---- HVSC documents
std::shared_ptr<Hvsc> Archive::ensureHvsc (const juce::File& folder)
{
    const auto root = Hvsc::findRoot (folder);
    if (root == juce::File()) return nullptr;
    const juce::ScopedLock sl (hvscLock);
    for (auto& h : hvscs)
        if (h->root == root) return h;
    auto h = Hvsc::open (root);
    if (h == nullptr) return nullptr;
    h->loadDocs();
    hvscs.push_back (h);
    return h;
}

std::vector<std::shared_ptr<Hvsc>> Archive::hvscList() const
{
    const juce::ScopedLock sl (hvscLock);
    return hvscs;
}

std::shared_ptr<Hvsc> Archive::hvscFor (const juce::String& path) const
{
    const juce::ScopedLock sl (hvscLock);
    for (auto& h : hvscs)
        if (insideFolder (path, h->root.getFullPathName())) return h;
    return nullptr;
}

void Archive::startHvscLoad()
{
    if (hvscLoading.exchange (true)) return;
    if (hvscThread.joinable()) hvscThread.join();
    const auto list2 = folders;
    hvscThread = std::thread ([this, list2]
    {
        for (const auto& f : list2) ensureHvsc (juce::File (f));
        hvscLoading = false;
        juce::MessageManager::callAsync ([this] { changed(); });
    });
}

void Archive::reloadHvsc()
{
    if (hvscThread.joinable()) hvscThread.join();
    {
        const juce::ScopedLock sl (hvscLock);
        hvscs.clear();
    }
    hvscLoading = false;
    startHvscLoad();
}

// ---- scanning
void Archive::startScan (bool fullRescan, const juce::StringArray& only)
{
    stopScan();
    juce::StringArray todo = only.isEmpty() ? folders : only;
    if (todo.isEmpty()) return;
    std::unordered_map<juce::String, std::pair<juce::int64, juce::int64>> known;
    known.reserve (list.size());
    for (auto& e : list) known[e->path] = { e->size, e->mtime };
    seenPaths = todo;   // remembered for the "missing" check at the end
    total = 0; done = 0;
    scanStart = juce::Time::getMillisecondCounter();
    scanPhase = juce::String::fromUTF8 ("Suche SID-Dateien …");
    scanner = std::make_unique<Scanner> (*this, fullRescan, todo, std::move (known));
    scanner->start();
    changed();
}

void Archive::stopScan()
{
    if (scanner == nullptr) return;
    scanner->stop = true;
    pump();
    scanner.reset();
    pending.clear();
    changed();
}

juce::String Archive::scanStatus() const
{
    const juce::ScopedLock sl (pendingLock);
    juce::String s = scanPhase;
    if (total.load() > 0) s << " " << done.load() << " / " << total.load();
    return s;
}

void Archive::insertOrReplace (std::unique_ptr<Entry> e)
{
    auto it = pathIndex.find (e->path);
    if (it != pathIndex.end())
    {
        auto* old = it->second;
        e->id = old->id; e->rating = old->rating; e->favourite = old->favourite;
        *old = std::move (*e);
        return;
    }
    e->id = nextId++;
    auto* raw = e.get();
    pathIndex[e->path] = raw;
    idIndex[e->id] = raw;
    list.push_back (std::move (e));
}

void Archive::pump()
{
    // read "finished" before taking the results: the workers push their last batch before they set it
    const bool scanFinished = scanner != nullptr && scanner->finished && scanner->next.load() >= scanner->jobs.size();
    std::deque<std::unique_ptr<Entry>> got;
    {
        const juce::ScopedLock sl (pendingLock);
        got.swap (pending);
    }
    const auto prot = protectedFolders();
    std::vector<int> newIds;
    bool any = ! got.empty();
    for (auto& e : got)
    {
        markProtection (*e, prot);
        const bool isNew = pathIndex.find (e->path) == pathIndex.end();
        const auto path = e->path;
        insertOrReplace (std::move (e));
        if (isNew) newIds.push_back (pathIndex[path]->id);
    }
    if (scanFinished)
    {
        // all results are in: mark what disappeared, give moved files the rating of their old entry
        std::unordered_set<juce::String> seen;
        { const juce::ScopedLock sl (scanner->seenLock); seen = scanner->seen; }
        const bool complete = ! scanner->stop;
        const auto scanned = seenPaths;
        scanner.reset();
        if (complete)
        {
            std::unordered_map<juce::String, Entry*> missingByMd5;
            for (auto& e : list)
            {
                bool inScan = false;
                for (const auto& f : scanned) if (insideFolder (e->path, f)) { inScan = true; break; }
                if (! inScan) continue;
                if (seen.count (e->path) != 0) e->flags &= ~(juce::uint32) fMissing;
                else { e->flags |= fMissing; if (e->md5.isNotEmpty()) missingByMd5[e->md5] = e.get(); }
            }
            for (int id : newIds)
                if (auto* ne = byId (id))
                    if (ne->rating == 0 && ! ne->favourite)
                    {
                        auto it = missingByMd5.find (ne->md5);
                        if (it != missingByMd5.end() && (it->second->rating > 0 || it->second->favourite))
                        {
                            ne->rating = it->second->rating; ne->favourite = it->second->favourite;
                            it->second->rating = 0; it->second->favourite = false;
                        }
                    }
        }
        pending.clear();
        any = true;
        dirty = true;
    }
    if (any)
    {
        markSameContent();
        dirty = true;
        changed();
    }
}

void Archive::waitForScan()
{
    while (scanner != nullptr)
    {
        juce::Thread::sleep (50);
        pump();
    }
}

void Archive::markSameContent()
{
    std::unordered_map<juce::String, int> count;
    count.reserve (list.size());
    for (auto& e : list) if (e->md5.isNotEmpty()) ++count[e->md5];
    duplicates = 0;
    for (auto& e : list)
    {
        const int c = e->md5.isNotEmpty() ? count[e->md5] : 1;
        e->sameGroup = c;
        if (c > 1) { e->flags |= fSameFile; ++duplicates; } else e->flags &= ~(juce::uint32) fSameFile;
    }
}

void Archive::timerCallback()
{
    if (scanner != nullptr || ! pending.empty()) pump();
    if ((dirty || settingsDirty) && ++saveCountdown >= (scanner != nullptr ? 900 : 20))   // 2 s, during a scan every 90 s
    {
        saveCountdown = 0;
        save();
    }
}

// ---- edits
void Archive::setRating (Entry& e, int rating)
{
    e.rating = juce::jlimit (0, 5, rating);
    dirty = true;
    changed();
}

void Archive::setFavourite (Entry& e, bool fav)
{
    e.favourite = fav;
    dirty = true;
    changed();
}

void Archive::removeEntries (const std::vector<int>& ids)
{
    std::unordered_set<int> s (ids.begin(), ids.end());
    removeWhere ([&] (const Entry& e) { return s.count (e.id) != 0; });
}

void Archive::relocate (Entry& e, const juce::File& newFile)
{
    pathIndex.erase (e.path);
    e.path = newFile.getFullPathName();
    e.name = newFile.getFileName();
    e.folder = newFile.getParentDirectory().getFullPathName();
    e.mtime = newFile.getLastModificationTime().toMilliseconds();
    pathIndex[e.path] = &e;
    markProtection (e, protectedFolders());
    dirty = true;
    changed();
}

Entry* Archive::addFile (const juce::File& f)
{
    const auto si = parseSidFile (f, false);
    auto hv = hvscFor (f.getFullPathName());
    auto e = makeEntry (f, si, hv.get());
    markProtection (*e, protectedFolders());
    const auto path = e->path;
    insertOrReplace (std::move (e));
    markSameContent();
    dirty = true;
    changed();
    return byPath (path);
}

// ---- persistence
void Archive::save()
{
    const auto dir = databaseFile().getParentDirectory();
    dir.createDirectory();
    if (settingsDirty || ! settingsFile().existsAsFile())
    {
        settings.setProperty ("folders", folders.joinIntoString ("\n"), nullptr);
        if (auto xml = settings.createXml()) xml->writeTo (settingsFile());
        settingsDirty = false;
    }
    if (! dirty && databaseFile().existsAsFile()) return;
    juce::MemoryOutputStream os;
    os.writeInt (kDbMagic);
    os.writeInt (kDbVersion);
    os.writeInt (nextId);
    os.writeInt ((int) list.size());
    auto s = [&] (const juce::String& v) { os.writeString (v); };
    for (auto& ep : list)
    {
        auto& e = *ep;
        os.writeInt (e.id);
        s (e.path); s (e.rel); s (e.md5); s (e.title); s (e.author); s (e.released); s (e.composer); s (e.area); s (e.country); s (e.stilFirst); s (e.error);
        os.writeInt64 (e.size); os.writeInt64 (e.mtime);
        os.writeInt ((int) (e.flags & ~((juce::uint32) fProtected | fSameFile)));
        os.writeInt (e.year); os.writeInt (e.songs); os.writeInt (e.startSong); os.writeInt (e.version);
        os.writeByte ((char) e.clock); os.writeByte ((char) e.model); os.writeByte ((char) e.sidCount);
        os.writeShort ((short) e.load); os.writeShort ((short) e.init); os.writeShort ((short) e.play);
        os.writeInt (e.dataSize);
        os.writeFloat (e.seconds);
        os.writeInt ((int) e.lengths.size());
        for (auto l : e.lengths) os.writeFloat (l);
        os.writeInt (e.rating);
        os.writeByte (e.favourite ? 1 : 0);
    }
    const auto tmp = databaseFile().getSiblingFile (databaseFile().getFileName() + ".tmp");
    if (tmp.replaceWithData (os.getData(), os.getDataSize()))
        tmp.moveFileTo (databaseFile());
    dirty = false;
}

bool Archive::load()
{
    if (auto xml = juce::XmlDocument::parse (settingsFile()))
    {
        settings = juce::ValueTree::fromXml (*xml);
        folders = juce::StringArray::fromLines (settings.getProperty ("folders").toString());
        folders.removeEmptyStrings();
    }
    juce::MemoryBlock mb;
    if (! databaseFile().loadFileAsData (mb)) return false;
    juce::MemoryInputStream is (mb, false);
    if (is.readInt() != kDbMagic || is.readInt() != kDbVersion) return false;
    nextId = is.readInt();
    const int n = is.readInt();
    if (n < 0 || n > 5000000) return false;
    list.reserve ((size_t) n);
    const auto prot = protectedFolders();
    for (int i = 0; i < n && ! is.isExhausted(); ++i)
    {
        auto e = std::make_unique<Entry>();
        e->id = is.readInt();
        e->path = is.readString(); e->rel = is.readString(); e->md5 = is.readString(); e->title = is.readString(); e->author = is.readString();
        e->released = is.readString(); e->composer = is.readString(); e->area = is.readString(); e->country = is.readString();
        e->stilFirst = is.readString(); e->error = is.readString();
        e->size = is.readInt64(); e->mtime = is.readInt64();
        e->flags = (juce::uint32) is.readInt();
        e->year = is.readInt(); e->songs = is.readInt(); e->startSong = is.readInt(); e->version = is.readInt();
        e->clock = (juce::uint8) is.readByte(); e->model = (juce::uint8) is.readByte(); e->sidCount = (juce::uint8) is.readByte();
        e->load = (juce::uint16) is.readShort(); e->init = (juce::uint16) is.readShort(); e->play = (juce::uint16) is.readShort();
        e->dataSize = is.readInt();
        e->seconds = is.readFloat();
        const int nl = is.readInt();
        if (nl < 0 || nl > 4096) return false;
        for (int k = 0; k < nl; ++k) e->lengths.push_back (is.readFloat());
        e->rating = is.readInt();
        e->favourite = is.readByte() != 0;
        const juce::File f (e->path);
        e->name = f.getFileName();
        e->folder = f.getParentDirectory().getFullPathName();
        markProtection (*e, prot);
        auto* raw = e.get();
        pathIndex[e->path] = raw;
        idIndex[e->id] = raw;
        list.push_back (std::move (e));
    }
    markSameContent();
    return true;
}
} // namespace sm
