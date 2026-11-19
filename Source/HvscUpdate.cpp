#include "HvscUpdate.h"
#include "SidInfo.h"
#include "Hvsc.h"
#include "SevenZip.h"

namespace sm
{
namespace
{
juce::String U8 (const char* s) { return juce::String::fromUTF8 (s); }

const char* const kSections[] = { "REPLACE", "MOVE", "DELETE", "MKDIR", "CREDITS", "TITLE", "AUTHOR", "RELEASED", "SONGS", "SPEED", "INITPLAY",
                                  "FLAGS", "CLOCK", "SIDMODEL", "FREEPAGES", "FIXLOAD", "COPYRIGHT", "PLAYSID" };

bool sectionType (const juce::String& line, HvscOp::Type& t)
{
    const auto s = line.trim();
    static const HvscOp::Type types[] = { HvscOp::replace, HvscOp::move, HvscOp::del, HvscOp::mkdir, HvscOp::credits, HvscOp::title, HvscOp::author,
                                          HvscOp::released, HvscOp::songs, HvscOp::speed, HvscOp::initplay, HvscOp::flags, HvscOp::clock,
                                          HvscOp::sidmodel, HvscOp::freepages, HvscOp::fixload, HvscOp::released, HvscOp::playsid };
    for (size_t i = 0; i < std::size (kSections); ++i)
        if (s == kSections[i]) { t = types[i]; return true; }
    return false;
}

// number of value lines after the path line
int valueLines (HvscOp::Type t)
{
    switch (t)
    {
        case HvscOp::replace: case HvscOp::move: return 1;                       // destination
        case HvscOp::del: case HvscOp::mkdir: case HvscOp::fixload: return 0;
        case HvscOp::credits: return 3;
        case HvscOp::flags: return 4;
        default: return 1;
    }
}

bool safeRelative (const juce::String& p)
{
    if (! p.startsWithChar ('/') || p.contains ("\\") || p.contains (":")) return false;
    for (const auto& part : juce::StringArray::fromTokens (p, "/", {}))
        if (part == "..") return false;
    return true;
}

juce::File fileOf (const juce::File& root, const juce::String& rel) { return root.getChildFile (rel.substring (1)); }

// network drives (NAS shares) sometimes refuse a directory or file operation once ("path not found"), so retry
bool createDirRetry (const juce::File& dir)
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        if (attempt > 0) juce::Thread::sleep (200 * attempt);
        if (dir.isDirectory() || dir.createDirectory().wasOk()) return true;
    }
    return dir.isDirectory();
}

bool copyWithRetry (const juce::File& src, const juce::File& dst)
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        if (attempt > 0) juce::Thread::sleep (250 * attempt);
        if (! createDirRetry (dst.getParentDirectory())) continue;
        if (src.copyFileTo (dst)) return true;
        if (dst.existsAsFile() && dst.getSize() == src.getSize()) return true;
        juce::FileInputStream in (src);                                  // second way: plain stream copy
        if (in.openedOk())
        {
            dst.deleteFile();
            juce::FileOutputStream out (dst);
            if (out.openedOk())
            {
                out.writeFromInputStream (in, -1);
                out.flush();
                if (out.getStatus().wasOk() && dst.getSize() == src.getSize()) return true;
            }
        }
    }
    return false;
}

void backup (const juce::File& root, const juce::File& file, const juce::File& backupDir)
{
    const auto rel = file.getRelativePathFrom (root);
    const auto target = backupDir.getChildFile (rel);
    if (target.existsAsFile() || ! file.existsAsFile()) return;
    copyWithRetry (file, target);
}

bool dirHasFiles (const juce::File& dir)
{
    for (const auto& e : juce::RangedDirectoryIterator (dir, true, "*", juce::File::findFiles)) { (void) e; return true; }
    return false;
}

// moves a file or a directory's content; returns number of moved files
int moveInto (const juce::File& src, const juce::File& dst, const juce::File& root, const juce::File& backupDir, HvscApplyResult& r)
{
    int n = 0;
    if (src.isDirectory())
    {
        dst.createDirectory();
        for (const auto& c : juce::RangedDirectoryIterator (src, false, "*", juce::File::findFilesAndDirectories))
            n += moveInto (c.getFile(), dst.getChildFile (c.getFile().getFileName()), root, backupDir, r);
        if (! dirHasFiles (src)) src.deleteRecursively();
        return n;
    }
    if (dst.existsAsFile()) { backup (root, dst, backupDir); dst.deleteFile(); }
    createDirRetry (dst.getParentDirectory());
    if (src.moveFileTo (dst)) { ++n; return n; }
    if (copyWithRetry (src, dst)) { src.deleteFile(); ++n; return n; }
    ++r.errors;
    r.messages.add ("Verschieben fehlgeschlagen: " + src.getFullPathName() + " -> " + dst.getFullPathName());
    return n;
}

void writeBe16 (juce::uint8* p, int v) { p[0] = (juce::uint8) (v >> 8); p[1] = (juce::uint8) v; }

juce::String latin1Fix (const juce::String& s) { return s; }

void putField (juce::MemoryBlock& mb, size_t offset, const juce::String& text)
{
    auto* p = static_cast<juce::uint8*> (mb.getData()) + offset;
    std::memset (p, 0, 32);
    int n = 0;
    for (auto c : text)
    {
        if (n >= 32) break;                      // header fields are 32 bytes; a full field has no terminator
        p[n++] = (juce::uint8) (c < 256 ? c : '?');
    }
}

int clockBits (const juce::String& v) { return v == "PAL" ? 1 : v == "NTSC" ? 2 : v == "ANY" ? 3 : 0; }
int modelBits (const juce::String& v) { return v == "6581" ? 1 : v == "8580" ? 2 : v == "ANY" ? 3 : 0; }

// patches one header; returns false (and a message) if it cannot
bool patchHeader (const HvscOp& op, juce::MemoryBlock& mb, juce::String& why)
{
    if (mb.getSize() < 0x76 || (std::memcmp (mb.getData(), "PSID", 4) != 0 && std::memcmp (mb.getData(), "RSID", 4) != 0)) { why = "kein SID-Kopf"; return false; }
    auto* d = static_cast<juce::uint8*> (mb.getData());
    const int version = (d[4] << 8) | d[5];
    switch (op.type)
    {
        case HvscOp::credits:
            if (op.values.size() >= 3)
            {
                if (op.values[0] != "*") putField (mb, 0x16, op.values[0]);
                if (op.values[1] != "*") putField (mb, 0x36, op.values[1]);
                if (op.values[2] != "*") putField (mb, 0x56, op.values[2]);
            }
            return true;
        case HvscOp::title: if (! op.values.isEmpty() && op.values[0] != "*") putField (mb, 0x16, op.values[0]); return true;
        case HvscOp::author: if (! op.values.isEmpty() && op.values[0] != "*") putField (mb, 0x36, op.values[0]); return true;
        case HvscOp::released: if (! op.values.isEmpty() && op.values[0] != "*") putField (mb, 0x56, op.values[0]); return true;
        case HvscOp::songs:
        {
            const auto a = op.values.isEmpty() ? juce::String() : op.values[0];
            writeBe16 (d + 0x0E, a.upToFirstOccurrenceOf (",", false, false).getIntValue());
            writeBe16 (d + 0x10, a.fromFirstOccurrenceOf (",", false, false).getIntValue());
            return true;
        }
        case HvscOp::speed:
        {
            const auto v = (juce::uint32) (op.values.isEmpty() ? 0 : op.values[0].trim().getHexValue32());
            d[0x12] = (juce::uint8) (v >> 24); d[0x13] = (juce::uint8) (v >> 16); d[0x14] = (juce::uint8) (v >> 8); d[0x15] = (juce::uint8) v;
            return true;
        }
        case HvscOp::initplay:
        {
            const auto a = op.values.isEmpty() ? juce::String() : op.values[0];
            writeBe16 (d + 0x0A, a.upToFirstOccurrenceOf (",", false, false).getHexValue32());
            writeBe16 (d + 0x0C, a.fromFirstOccurrenceOf (",", false, false).getHexValue32());
            return true;
        }
        case HvscOp::flags: case HvscOp::clock: case HvscOp::sidmodel: case HvscOp::freepages: case HvscOp::playsid:
        {
            if (version < 2 || mb.getSize() < 0x7C) { why = "Kopf ohne Flags (PSID v1)"; return false; }
            int flags = (d[0x76] << 8) | d[0x77];
            if (op.type == HvscOp::flags && op.values.size() >= 4)
            {
                if (op.values[2] != "*") flags = (flags & ~0x0C) | (clockBits (op.values[2]) << 2);
                if (op.values[3] != "*") flags = (flags & ~0x30) | (modelBits (op.values[3]) << 4);
            }
            else if (op.type == HvscOp::clock && ! op.values.isEmpty()) flags = (flags & ~0x0C) | (clockBits (op.values[0]) << 2);
            else if (op.type == HvscOp::sidmodel && ! op.values.isEmpty()) flags = (flags & ~0x30) | (modelBits (op.values[0]) << 4);
            else if (op.type == HvscOp::playsid && ! op.values.isEmpty())
                flags = (flags & ~0x02) | (op.values[0].trim().getIntValue() != 0 ? 0x02 : 0);
            else if (op.type == HvscOp::freepages && ! op.values.isEmpty())
            {
                d[0x78] = (juce::uint8) op.values[0].upToFirstOccurrenceOf (",", false, false).getHexValue32();
                d[0x79] = (juce::uint8) op.values[0].fromFirstOccurrenceOf (",", false, false).getHexValue32();
            }
            writeBe16 (d + 0x76, flags);
            return true;
        }
        default: return true;
    }
}
} // namespace

// ================================================================================================ parser
std::vector<HvscOp> parseHvscScript (const juce::String& text, juce::StringArray& warnings)
{
    std::vector<HvscOp> ops;
    HvscOp::Type section = HvscOp::fixload;
    bool haveSection = false;
    const auto lines = juce::StringArray::fromLines (text);
    int i = 0;
    auto nextLine = [&] (juce::String& out) -> bool
    {
        while (i < lines.size())
        {
            const auto l = lines[i++];
            if (l.trimStart().startsWithChar ('#')) continue;
            out = l.trimEnd();
            return true;
        }
        return false;
    };
    juce::String line;
    while (nextLine (line))
    {
        if (line.trim().isEmpty()) continue;
        HvscOp::Type t;
        if (! line.startsWithChar ('/') && sectionType (line, t)) { section = t; haveSection = true; continue; }
        if (! haveSection || ! line.startsWithChar ('/')) { warnings.add ("Unbekannte Zeile: " + line.substring (0, 80)); continue; }
        HvscOp op;
        op.type = section;
        op.path = line.trim();
        const int n = valueLines (section);
        for (int k = 0; k < n; ++k)
        {
            juce::String v;
            if (! nextLine (v)) { warnings.add ("Skript endet mitten in einem Eintrag: " + op.path); break; }
            op.values.add (v);
        }
        if (section == HvscOp::fixload) { warnings.add (U8("FIXLOAD wird nicht unterstützt: ") + op.path); continue; }
        ops.push_back (std::move (op));
    }
    return ops;
}

// ================================================================================================ apply
bool applyHvscScript (const juce::File& root, const std::vector<HvscOp>& ops, const juce::File& backupDir, HvscApplyResult& r,
                      const std::function<bool (int, int)>& progress, const juce::File& stagingTop)
{
    const int total = (int) ops.size();
    int done = 0;
    for (const auto& op : ops)
    {
        if (progress && (done % 64) == 0 && ! progress (done, total)) { r.messages.add ("Abgebrochen"); return false; }
        ++done;
        if (! safeRelative (op.path)) { ++r.errors; r.messages.add ("Unsicherer Pfad: " + op.path); continue; }
        // "/update/..." lives in the local staging folder when one is given (keeps thousands of temporary files off a network drive)
        const bool staged = stagingTop != juce::File() && (op.path == "/update" || op.path.startsWith ("/update/"));
        const auto target = staged ? stagingTop.getChildFile (op.path.substring (8)) : fileOf (root, op.path);
        switch (op.type)
        {
            case HvscOp::replace: case HvscOp::move:
            {
                const auto dstRel = op.values.isEmpty() ? juce::String() : op.values[0].trim();
                if (! safeRelative (dstRel)) { ++r.errors; r.messages.add ("Unsicheres Ziel: " + dstRel); break; }
                if (! target.exists()) { ++r.missing; r.messages.add ("Fehlt: " + op.path); break; }
                auto dst = fileOf (root, dstRel);
                if (dstRel.endsWithChar ('/') && target.isDirectory() == false) dst = dst.getChildFile (target.getFileName());
                const int n = moveInto (target, dst, root, backupDir, r);
                (op.type == HvscOp::move ? r.moved : r.replaced) += n;
                break;
            }
            case HvscOp::del:
                if (target.isDirectory())
                {
                    if (! dirHasFiles (target)) target.deleteRecursively();
                    else { r.messages.add ("Ordner nicht leer, bleibt: " + op.path); }
                }
                else if (target.existsAsFile())
                {
                    backup (root, target, backupDir);
                    if (target.deleteFile()) ++r.deleted; else { ++r.errors; r.messages.add (U8("Löschen fehlgeschlagen: ") + op.path); }
                }
                break;
            case HvscOp::mkdir: target.createDirectory(); break;
            case HvscOp::fixload: break;
            default:
            {
                juce::MemoryBlock mb;
                if (! target.existsAsFile() || ! target.loadFileAsData (mb)) { ++r.missing; r.messages.add ("Fehlt: " + op.path); break; }
                juce::String why;
                if (! patchHeader (op, mb, why)) { r.messages.add (op.path + ": " + why); break; }
                backup (root, target, backupDir);
                if (target.replaceWithData (mb.getData(), mb.getSize())) ++r.patched; else { ++r.errors; r.messages.add ("Schreiben fehlgeschlagen: " + op.path); }
                break;
            }
        }
    }
    return true;
}

int verifyHvscHeaders (const juce::File& root, const std::vector<HvscOp>& ops, juce::StringArray& mismatches, int* checkedOut)
{
    int checked = 0, matches = 0;
    for (const auto& op : ops)
    {
        if (op.type < HvscOp::title || op.type == HvscOp::fixload) continue;
        const auto f = fileOf (root, op.path);
        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb)) continue;
        juce::MemoryBlock patched (mb);
        juce::String why;
        if (! patchHeader (op, patched, why)) continue;
        ++checked;
        // compare only the header bytes this operation may change
        if (std::memcmp (mb.getData(), patched.getData(), std::min<size_t> (0x7C, mb.getSize())) == 0) ++matches;
        else mismatches.add (op.path);
    }
    if (checkedOut != nullptr) *checkedOut = checked;
    return matches;
}

// ================================================================================================ package
namespace
{

// folder that contains "<folder>/<script>" (searched up to two levels deep)
juce::File findPackageRoot (const juce::File& dir, const juce::String& script, const juce::String& folder, int depth = 0)
{
    if (dir.getChildFile (folder).getChildFile (script).existsAsFile()) return dir;
    if (depth >= 2) return {};
    for (const auto& c : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findDirectories))
    {
        auto r = findPackageRoot (c.getFile(), script, folder, depth + 1);
        if (r != juce::File()) return r;
    }
    return {};
}

// extraction with the system's bsdtar/libarchive (reads 7z incl. PPMd): Windows 10+, macOS
bool extractWithSystemTar (const juce::File& archive, const juce::File& dest, const std::function<bool()>& stillWanted, juce::String& error)
{
   #if JUCE_WINDOWS
    const auto tar = juce::File::getSpecialLocation (juce::File::windowsSystemDirectory).getChildFile ("tar.exe");
   #else
    const juce::File tar ("/usr/bin/tar");
   #endif
    if (! tar.existsAsFile()) { error = U8("Das Archiv nutzt ein Verfahren (PPMd), das SIDiMIDI nicht selbst entpacken kann, und das System-Programm „tar“ wurde nicht gefunden."); return false; }
    juce::ChildProcess p;
    if (! p.start (juce::StringArray { tar.getFullPathName(), "-xf", archive.getFullPathName(), "-C", dest.getFullPathName() }, juce::ChildProcess::wantStdErr))
    { error = "tar konnte nicht gestartet werden"; return false; }
    while (p.isRunning())
    {
        if (! stillWanted()) { p.kill(); error = "Abgebrochen"; return false; }
        juce::Thread::sleep (80);
    }
    const auto code = p.getExitCode();
    if (code != 0) { error = "tar meldet Fehler " + juce::String ((int) code) + ": " + p.readAllProcessOutput().trim().substring (0, 300); return false; }
    return true;
}

bool extractZip (const juce::File& zipFile, const juce::File& dest, const std::function<bool (double)>& progress, juce::String& error)
{
    juce::ZipFile zip (zipFile);
    const int n = zip.getNumEntries();
    if (n <= 0) { error = U8("ZIP-Archiv ist leer oder beschädigt"); return false; }
    for (int i = 0; i < n; ++i)
    {
        if (progress && ! progress ((double) i / n)) { error = "Abgebrochen"; return false; }
        const auto* e = zip.getEntry (i);
        const auto name = e->filename.replaceCharacter ('\\', '/');
        bool unsafe = name.startsWithChar ('/') || name.contains (":");
        for (const auto& part : juce::StringArray::fromTokens (name, "/", {})) unsafe = unsafe || part == "..";
        if (unsafe) continue;
        const auto target = dest.getChildFile (name);
        if (name.endsWithChar ('/')) { target.createDirectory(); continue; }
        std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
        if (in == nullptr) { error = "Eintrag nicht lesbar: " + name; return false; }
        target.getParentDirectory().createDirectory();
        juce::FileOutputStream out (target);
        if (! out.openedOk()) { error = U8("Schreiben nicht möglich: ") + target.getFullPathName(); return false; }
        out.writeFromInputStream (*in, -1);
    }
    return true;
}

int countFiles (const juce::File& dir)
{
    int n = 0;
    for (const auto& e : juce::RangedDirectoryIterator (dir, true, "*", juce::File::findFiles)) { (void) e; ++n; }
    return n;
}
} // namespace

HvscInstallResult installHvscPackage (const juce::File& root, int version, const juce::File& packageFile, const juce::File& backupDir,
                                      const std::function<bool (const juce::String&, double)>& progress)
{
    HvscInstallResult r;
    auto report = [&] (const juce::String& s, double f) { return ! progress || progress (s, f); };
    auto fail = [&] (const juce::String& e) { r.error = e; return r; };

    if (! root.isDirectory()) return fail ("Sammlung nicht gefunden: " + root.getFullPathName());
    { // writable?
        const auto probe = root.getChildFile (".sidimidi-write-test");
        if (! probe.replaceWithText ("x")) return fail ("Der Ordner ist nicht beschreibbar: " + root.getFullPathName());
        probe.deleteFile();
    }

    const auto work = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("SIDIMIDI-hvsc", "", false);
    work.createDirectory();
    struct Cleanup { juce::File f; ~Cleanup() { f.deleteRecursively(); } } cleanup { work };

    // 1) extract
    if (! report (U8("Entpacken …"), 0.0)) { r.cancelled = true; return fail ("Abgebrochen"); }
    juce::String err;
    bool ok;
    if (packageFile.hasFileExtension ("zip"))
        ok = extractZip (packageFile, work, [&] (double f) { return report (U8("Entpacken …"), 0.05 + 0.35 * f); }, err);
    else
    {
        juce::StringArray skippedFiles;    // e.g. update.exe (BCJ2) – programs are not needed
        ok = SevenZip::extractAll (packageFile, work, [&] (juce::uint64 d, juce::uint64 t, const juce::String&) { return report (U8("Entpacken …"), 0.05 + 0.35 * (t > 0 ? (double) d / (double) t : 0.0)); }, err, &skippedFiles);
        for (auto& sk : skippedFiles)
            if (! sk.endsWithIgnoreCase (".exe")) { ok = false; err = "Nicht unterstützt: " + sk; }
        if (! ok && err != "Abgebrochen")
        {
            // methods the own reader does not know (the HVSC packages use PPMd): let the system's tar do it
            const auto own = err;
            work.deleteRecursively();
            work.createDirectory();
            ok = extractWithSystemTar (packageFile, work, [&] { return report (U8("Entpacken …"), 0.2); }, err);
            if (! ok && err != "Abgebrochen") err = own + " / " + err;
        }
    }
    if (! ok) { r.cancelled = (err == "Abgebrochen"); return fail (err.isEmpty() ? juce::String ("Entpacken fehlgeschlagen") : err); }

    // 2) find the package: official layout = "update/UpdateNN.hvs" (+ update/DOCUMENTS, update/fix, update/new); older/simple = "DOCUMENTS/UpdateNN.hvs"
    const auto scriptName = juce::String::formatted ("Update%02d.hvs", version);
    juce::File pkg;
    bool official = false;
    pkg = findPackageRoot (work, scriptName, "update");
    official = pkg != juce::File();
    if (! official) pkg = findPackageRoot (work, scriptName, "DOCUMENTS");
    if (pkg == juce::File()) return fail (juce::String::formatted (U8("Das Paket enthält kein Update%02d.hvs – es passt nicht zu Version %d."), version, version));

    // 3) copy into the collection (official layout: the "update" folder, which the script refers to as /update/...)
    // Official layout: the package's "update" folder stays in the local temp folder (the script's /update/... sources are read from there),
    // so no thousands of temporary files are written to the collection (which may be on a NAS). Simple layout: copied into the collection.
    const auto sourceTop = official ? pkg.getChildFile ("update") : pkg;
    const auto destTop = official ? sourceTop : root;
    const int total = std::max (1, countFiles (sourceTop));
    int done = 0;
    for (const auto& e : juce::RangedDirectoryIterator (sourceTop, true, "*", juce::File::findFiles))
    {
        if (official) break;
        if (! report (U8("Dateien einspielen …"), 0.40 + 0.30 * ((double) done / total))) { r.cancelled = true; return fail ("Abgebrochen"); }
        ++done;
        const auto f = e.getFile();
        const auto ext = f.getFileExtension().toLowerCase();
        if (ext == ".exe" || ext == ".dll" || ext == ".bat" || ext == ".cmd" || ext == ".com") continue;   // never copy programs into the collection
        const auto dst = destTop.getChildFile (f.getRelativePathFrom (sourceTop));
        if (dst.existsAsFile()) backup (root, dst, backupDir);
        dst.getParentDirectory().createDirectory();
        if (copyWithRetry (f, dst)) ++r.copiedFiles;
        else
        {
            r.applied.errors++;
            r.applied.messages.add ("Kopieren fehlgeschlagen: " + dst.getFullPathName() + " (Quelle " + (f.existsAsFile() ? juce::String (f.getSize()) + " Bytes" : juce::String ("fehlt"))
                                    + ", Zielordner " + (dst.getParentDirectory().isDirectory() ? juce::String ("vorhanden") : "fehlt: " + dst.getParentDirectory().createDirectory().getErrorMessage()) + ", Ziel " + (dst.existsAsFile() ? juce::String (dst.getSize()) + " Bytes" : juce::String ("fehlt")) + ")");
        }
    }
    if (r.applied.errors > 0) return fail (U8("Dateien konnten nicht eingespielt werden – siehe Protokoll"));   // the collection has not been touched yet

    // 4) run the script
    const auto scriptFile = official ? destTop.getChildFile (scriptName) : root.getChildFile ("DOCUMENTS").getChildFile (scriptName);
    const auto ops = parseHvscScript (readDocumentText (scriptFile), r.warnings);
    if (ops.empty()) return fail ("Das Update-Skript ist leer oder nicht lesbar");
    const bool completed = applyHvscScript (root, ops, backupDir, r.applied, [&] (int d, int t) { return report (U8("Update-Skript ausführen …"), 0.70 + 0.25 * (t > 0 ? (double) d / t : 1.0)); },
                                            official ? sourceTop : juce::File());
    if (! completed) { r.cancelled = true; return fail (U8("Abgebrochen – die Sammlung ist nur teilweise aktualisiert (Backup: ") + backupDir.getFullPathName() + ")"); }
    if (r.applied.errors > 0) return fail (U8("Einzelne Schritte sind fehlgeschlagen – siehe Protokoll (die Dokumente wurden nicht umgestellt)"));

    // 5) documents last: the highest DOCUMENTS/UpdateNN.hvs defines the version, so only after everything worked
    if (official)
    {
        const auto docs = destTop.getChildFile ("DOCUMENTS");
        if (docs.isDirectory())
            for (const auto& e : juce::RangedDirectoryIterator (docs, true, "*", juce::File::findFiles))
            {
                const auto dst = root.getChildFile ("DOCUMENTS").getChildFile (e.getFile().getRelativePathFrom (docs));
                if (dst.existsAsFile()) backup (root, dst, backupDir);
                dst.getParentDirectory().createDirectory();
                if (copyWithRetry (e.getFile(), dst)) ++r.copiedFiles;
                else { r.applied.errors++; r.applied.messages.add ("Kopieren fehlgeschlagen: " + dst.getFullPathName()); }
            }
        const auto dstScript = root.getChildFile ("DOCUMENTS").getChildFile (scriptName);
        if (! copyWithRetry (scriptFile, dstScript)) { r.applied.errors++; r.applied.messages.add ("Kopieren fehlgeschlagen: " + dstScript.getFullPathName()); }
    }

    // 6) a "update" folder left in the collection by an older/aborted run is junk
    if (! official) { const auto leftover = root.getChildFile ("update"); if (leftover.isDirectory() && r.applied.errors == 0) leftover.deleteRecursively(); }
    report ("Fertig", 1.0);
    r.ok = r.applied.errors == 0;
    if (! r.ok) r.error = U8("Einzelne Schritte sind fehlgeschlagen – siehe Protokoll");
    return r;
}

// ================================================================================================ online
juce::StringArray HvscOnline::mirrors()
{
    return { "https://hvsc.c64.org/download/files/archives/", "https://hvsc.brona.dk/HVSC/" };
}

juce::String HvscOnline::updateFileName (int v) { return "HVSC_Update_" + juce::String (v) + (v >= kFirstSevenZip ? ".7z" : ".zip"); }
juce::String HvscOnline::fullFileName (int v) { return "HVSC_" + juce::String (v) + "-all-of-them.7z"; }

namespace
{
juce::int64 headSize (const juce::String& url, int timeoutMs)
{
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withHttpRequestCmd ("HEAD")
                    .withConnectionTimeoutMs (timeoutMs)
                    .withExtraHeaders ("User-Agent: SIDiMIDI/" SM_VERSION_STRING "\r\n")
                    .withNumRedirectsToFollow (3);
    auto stream = juce::URL (url).createInputStream (opts);
    auto* web = dynamic_cast<juce::WebInputStream*> (stream.get());
    if (web == nullptr) return -1;
    if (web->getStatusCode() != 200) return -1;
    const auto len = web->getTotalLength();
    return len > 0 ? len : 0;
}
} // namespace

juce::int64 HvscOnline::remoteSize (const juce::String& fileName, juce::String* usedUrl, int timeoutMs)
{
    for (const auto& m : mirrors())
    {
        const auto url = m + fileName;
        const auto n = headSize (url, timeoutMs);
        if (n >= 0) { if (usedUrl != nullptr) *usedUrl = url; return n; }
    }
    return -1;
}

int HvscOnline::latestVersion (int localVersion, const std::function<bool()>& cancel)
{
    int newest = 0;
    bool reached = false;
    for (int v = std::max (1, localVersion + 1); v < localVersion + 40; ++v)
    {
        if (cancel && cancel()) return newest;
        const auto n = remoteSize (updateFileName (v));
        if (n >= 0) { newest = v; reached = true; }
        else break;
    }
    if (reached) return newest;
    // nothing newer: make sure the connection works at all by asking for the installed version's own file
    return remoteSize (updateFileName (localVersion)) >= 0 ? localVersion : 0;
}

bool HvscOnline::download (const juce::String& fileName, const juce::File& target, const std::function<bool (juce::int64, juce::int64)>& progress, juce::String& error)
{
    error = {};
    for (const auto& m : mirrors())
    {
        const auto url = m + fileName;
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (15000)
                        .withExtraHeaders ("User-Agent: SIDiMIDI/" SM_VERSION_STRING "\r\n")
                        .withNumRedirectsToFollow (3);
        auto stream = juce::URL (url).createInputStream (opts);
        auto* web = dynamic_cast<juce::WebInputStream*> (stream.get());
        if (web == nullptr || web->getStatusCode() != 200) { error = U8("Download nicht möglich: ") + url; continue; }
        const auto total = web->getTotalLength();
        target.deleteFile();
        juce::FileOutputStream out (target);
        if (! out.openedOk()) { error = U8("Datei lässt sich nicht schreiben: ") + target.getFullPathName(); return false; }
        juce::HeapBlock<char> buffer (65536);
        juce::int64 got = 0;
        bool ok = true;
        while (! stream->isExhausted())
        {
            const int n = stream->read (buffer, 65536);
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            out.write (buffer, (size_t) n);
            got += n;
            if (progress && ! progress (got, total)) { out.flush(); target.deleteFile(); error = "Abgebrochen"; return false; }
        }
        out.flush();
        if (ok && (total <= 0 || got == total)) return true;
        target.deleteFile();
        error = U8("Download unvollständig: ") + url;
    }
    return false;
}
} // namespace sm
