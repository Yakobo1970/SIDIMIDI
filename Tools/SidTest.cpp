// SidTest – console checks for the core of SIDiMIDI (no user interface).
//   SidTest info <file.sid>
//   SidTest hvsc <C64Music folder>          documents: version, lengths, STIL, bugs, musicians
//   SidTest scanstat <folder> [max]         header statistics over a folder tree
#include <juce_core/juce_core.h>
#include "../Source/SidInfo.h"
#include "../Source/Hvsc.h"
#include "../Source/Archive.h"
#include "../Source/SidTrace.h"
#include "../Source/SevenZip.h"
#include "../Source/HvscUpdate.h"
#include "../Source/SidToMidiCore.h"
#include "../Source/MidiToSidCore.h"
#include <iostream>

using namespace sm;

static void out (const juce::String& s) { std::cout << s.toRawUTF8() << std::endl; }

int main (int argc, char** argv)
{
    const juce::String cmd = argc > 1 ? argv[1] : "";

    if (cmd == "info" && argc > 2)
    {
        const auto si = parseSidFile (juce::File (juce::String::fromUTF8 (argv[2])));
        out ("valid=" + juce::String ((int) si.valid) + " " + si.formatName() + " songs=" + juce::String (si.songs) + " start=" + juce::String (si.startSong));
        out ("title='" + si.title + "' author='" + si.author + "' released='" + si.released + "' year=" + juce::String (si.year()));
        out ("load=$" + juce::String::toHexString (si.effLoad) + " init=$" + juce::String::toHexString (si.effInit()) + " play=$" + juce::String::toHexString (si.playAddr)
             + " data=" + juce::String ((int) si.code.size()) + " speed=$" + juce::String::toHexString ((int) si.speed));
        out ("clock=" + si.clockName() + " model=" + si.modelName() + " sids=" + juce::String (si.sidCount) + " md5=" + si.md5);
        return 0;
    }

    if (cmd == "hvsc" && argc > 2)
    {
        const auto dir = juce::File (juce::String::fromUTF8 (argv[2]));
        const auto root = Hvsc::findRoot (dir);
        out ("root: " + root.getFullPathName());
        auto h = Hvsc::open (root);
        if (h == nullptr) { out ("kein HVSC"); return 1; }
        out ("version " + juce::String (h->version) + " date '" + h->versionDate + "'");
        const auto t0 = juce::Time::getMillisecondCounter();
        h->loadDocs();
        out ("docs " + juce::String ((int) (juce::Time::getMillisecondCounter() - t0)) + " ms: lengths=" + juce::String (h->lengthEntries()) + " stil=" + juce::String (h->stilEntries())
             + " bugs=" + juce::String (h->bugEntries()) + " musicians=" + juce::String (h->musicianEntries()));
        const auto commando = h->root.getChildFile ("MUSICIANS/H/Hubbard_Rob/Commando.sid");
        const auto si = parseSidFile (commando);
        out ("Commando: " + si.title + " / " + si.author + " md5 " + si.md5);
        if (auto* l = h->lengths (si.md5)) { juce::String s; for (auto v : *l) s << formatTime (v, true) << " "; out ("lengths: " + s); }
        else out ("lengths: NICHT GEFUNDEN");
        const auto rel = h->relPath (commando);
        out ("rel " + rel + "  stil " + juce::String (h->stil (rel).length()) + " chars, dir " + juce::String (h->stilDirectory (rel).length()));
        if (auto* m = h->musicianFor (rel)) out ("musician: " + m->handle + " | " + m->name + " | " + m->group + " | " + m->country);
        else out ("musician: nicht gefunden");
        out (h->bug ("/DEMOS/A-F/Digital_II.sid").substring (0, 80));
        out ("key: '" + Hvsc::normaliseName ("Galway Martin") + "' / '" + Hvsc::normaliseName ("Hubbard, Rob") + "'");
        for (auto* r : { "/MUSICIANS/G/Galway_Martin/Comic_Bakery.sid", "/MUSICIANS/H/Hubbard_Rob/Monty_on_the_Run.sid", "/MUSICIANS/J/Jeroen_Tel/Cybernoid.sid", "/MUSICIANS/T/Tel_Jeroen/Cybernoid.sid", "/MUSICIANS/D/Daglish_Ben/Last_Ninja.sid" })
            if (auto* m = h->musicianFor (r)) out (juce::String (r) + " -> " + m->handle + " | " + m->name + " | " + m->group + " | " + m->country); else out (juce::String (r) + " -> -");
        return 0;
    }

    if (cmd == "scanstat" && argc > 2)
    {
        const auto dir = juce::File (juce::String::fromUTF8 (argv[2]));
        const int maxFiles = argc > 3 ? juce::String (argv[3]).getIntValue() : 1000000;
        auto root = Hvsc::findRoot (dir);
        auto h = Hvsc::open (root);
        if (h != nullptr) h->loadDocs();
        int n = 0, bad = 0, found = 0, ntsc = 0, rsid = 0, multi = 0, mus = 0;
        int models[4] = {};
        const auto t0 = juce::Time::getMillisecondCounter();
        for (const auto& e : juce::RangedDirectoryIterator (dir, true, "*.sid", juce::File::findFiles))
        {
            if (n >= maxFiles) break;
            ++n;
            const auto si = parseSidFile (e.getFile(), false);
            if (! si.valid) { ++bad; continue; }
            if (h != nullptr && h->lengths (si.md5) != nullptr) ++found;
            ntsc += si.clock == 2; rsid += si.rsid; multi += si.sidCount > 1; mus += si.mus; models[si.model & 3]++;
        }
        out (juce::String (n) + " files, " + juce::String (bad) + " invalid, lengths found " + juce::String (found) + ", NTSC " + juce::String (ntsc) + ", RSID " + juce::String (rsid)
             + ", multi-SID " + juce::String (multi) + ", MUS " + juce::String (mus) + ", models unknown/6581/8580/both "
             + juce::String (models[0]) + "/" + juce::String (models[1]) + "/" + juce::String (models[2]) + "/" + juce::String (models[3])
             + " in " + juce::String ((int) (juce::Time::getMillisecondCounter() - t0)) + " ms");
        return 0;
    }

    if (cmd == "archive" && argc > 3)
    {
        // SidTest archive <db-file> <folder> [full]   (scans into a database file, saves, reloads, checks)
        #if JUCE_WINDOWS
        _putenv_s ("SIDIMIDI_DB", argv[2]);
#else
        setenv ("SIDIMIDI_DB", argv[2], 1);
#endif
        juce::MessageManager::getInstance();
        {
            Archive a;
            a.addFolder (juce::File (juce::String::fromUTF8 (argv[3])));
            const auto t0 = juce::Time::getMillisecondCounter();
            a.startScan (argc > 4);
            a.waitForScan();
            out ("scan " + juce::String ((int) (juce::Time::getMillisecondCounter() - t0)) + " ms: " + juce::String ((int) a.entries().size()) + " entries, "
                 + juce::String (a.duplicateCount()) + " with identical content, hvsc roots " + juce::String ((int) a.hvscList().size()));
            int withLen = 0, stil = 0, bug = 0, withComposer = 0, withCountry = 0, err = 0;
            for (auto& e : a.entries()) { withLen += e->seconds > 0; stil += e->has (fHasStil); bug += e->has (fHasBug); withComposer += e->composer.isNotEmpty(); withCountry += e->country.isNotEmpty(); err += e->has (fError); }
            out ("lengths " + juce::String (withLen) + ", stil " + juce::String (stil) + ", bug " + juce::String (bug) + ", composer " + juce::String (withComposer) + ", country " + juce::String (withCountry) + ", errors " + juce::String (err));
            if (! a.entries().empty())
            {
                auto& e0 = *a.entries().front();
                a.setRating (e0, 4); a.setFavourite (e0, true);
                out ("first: " + e0.name + " | " + e0.title + " | " + e0.author + " | " + e0.released + " | " + lengthText (e0) + " | " + e0.composer + " | " + e0.country + " | " + e0.stilFirst);
            }
            a.save();
        }
        {
            Archive b;
            out ("reload: " + juce::String ((int) b.entries().size()) + " entries, folders " + juce::String (b.folders.size()));
            if (! b.entries().empty()) out ("first rating " + juce::String (b.entries().front()->rating) + " fav " + juce::String ((int) b.entries().front()->favourite));
            const auto t0 = juce::Time::getMillisecondCounter();
            b.startScan (false);
            b.waitForScan();
            out ("rescan (nothing changed) " + juce::String ((int) (juce::Time::getMillisecondCounter() - t0)) + " ms, entries " + juce::String ((int) b.entries().size()));
        }
        return 0;
    }

    if (cmd == "trace" && argc > 2)
    {
        // SidTest trace <file> [song] [seconds]
        const auto si = parseSidFile (juce::File (juce::String::fromUTF8 (argv[2])));
        const int song = argc > 3 ? juce::String (argv[3]).getIntValue() : si.startSong;
        const double secs = argc > 4 ? juce::String (argv[4]).getDoubleValue() : 60.0;
        const auto t0 = juce::Time::getMillisecondCounter();
        auto t = computeTrace (si, song, secs);
        out ("trace song " + juce::String (song) + ": fps " + juce::String (t->fps, 3) + " frames " + juce::String ((int) t->frames) + " events " + juce::String (t->events ? (int) t->events->size() : 0)
             + " timedOut " + juce::String ((int) t->timedOut) + " noPlay " + juce::String ((int) t->noPlay) + " note '" + t->note + "' in " + juce::String ((int) (juce::Time::getMillisecondCounter() - t0)) + " ms");
        for (int v = 0; v < 3; ++v)
        {
            int active = 0, changes = 0; float last = -2; int waves[16] = {};
            for (juce::uint32 f = 0; f < t->frames; ++f)
            {
                const float n = t->v[v].note[f];
                if (n >= 0) ++active;
                if (std::abs (n - last) > 0.4f) ++changes;
                last = n; waves[t->v[v].wave[f] & 15]++;
            }
            out ("voice " + juce::String (v + 1) + ": active frames " + juce::String (active) + " (" + juce::String (100 * active / (int) std::max<juce::uint32> (1, t->frames)) + "%), note changes " + juce::String (changes)
                 + ", tri/saw/pulse/noise frames " + juce::String (waves[1]) + "/" + juce::String (waves[2]) + "/" + juce::String (waves[4]) + "/" + juce::String (waves[8]));
        }
        out ("note range " + juce::String (t->minNote) + " .. " + juce::String (t->maxNote));
        return 0;
    }

    if (cmd == "tracesample" && argc > 2)
    {
        // SidTest tracesample <folder> [max]: emulates the start song of many files (10 s) and counts problems
        int maxFiles = argc > 3 ? juce::String (argv[3]).getIntValue() : 50;
        int n = 0, silent = 0, noPlay = 0, timedOut = 0, ciaTunes = 0;
        double totalMs = 0;
        for (const auto& e : juce::RangedDirectoryIterator (juce::File (juce::String::fromUTF8 (argv[2])), true, "*.sid", juce::File::findFiles))
        {
            if (n >= maxFiles) break;
            const auto si = parseSidFile (e.getFile());
            if (! si.valid) continue;
            ++n;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            auto t = computeTrace (si, si.startSong, 10.0);
            totalMs += juce::Time::getMillisecondCounterHiRes() - t0;
            silent += t->events == nullptr || t->events->empty(); noPlay += t->noPlay; timedOut += t->timedOut; ciaTunes += si.cia (si.startSong);
            if (t->events == nullptr || t->events->empty()) out ("  silent/no events: " + e.getFile().getFileName() + " (" + t->note + ")");
        }
        out (juce::String (n) + " tunes: silent " + juce::String (silent) + ", noPlay " + juce::String (noPlay) + ", timed out " + juce::String (timedOut) + ", CIA-speed " + juce::String (ciaTunes) + ", avg " + juce::String (totalMs / std::max (1, n), 1) + " ms");
        return 0;
    }

    if (cmd == "tomidi" && argc > 3)
    {
        // SidTest tomidi <sid> <out.mid> [song]
        juce::String log;
        const bool ok = SidToMidiCoreEngine::convertSidToMidiFile (juce::File (juce::String::fromUTF8 (argv[2])), juce::File (juce::String::fromUTF8 (argv[3])), log, argc > 4 ? juce::String (argv[4]).getIntValue() : 1);
        out ((ok ? "ok" : "FEHLER") + juce::String (" ") + log.trim());
        return ok ? 0 : 1;
    }

    if (cmd == "midiinfo" && argc > 2)
    {
        juce::FileInputStream in (juce::File (juce::String::fromUTF8 (argv[2])));
        juce::MidiFile mf;
        if (! in.openedOk() || ! mf.readFrom (in)) { out ("cannot read"); return 1; }
        const int tpq = mf.getTimeFormat();
        mf.convertTimestampTicksToSeconds();
        double last = 0; int notes = 0;
        for (int t = 0; t < mf.getNumTracks(); ++t) { auto* tr = mf.getTrack (t); last = std::max (last, tr->getEndTime()); for (auto* e : *tr) notes += e->message.isNoteOn(); }
        out ("tracks " + juce::String (mf.getNumTracks()) + " tpq " + juce::String (tpq) + " notes " + juce::String (notes) + " length " + formatTime (last, true));
        return 0;
    }

    if (cmd == "parts" && argc > 2)
    {
        // SidTest parts <mid>: the parts (track / channel) the converter offers and its automatic choice
        juce::String log;
        std::vector<MidiToSidCoreEngine::ChannelSuggestion> all;
        MidiToSidCoreEngine::listAllTracks (juce::File (juce::String::fromUTF8 (argv[2])), all, log);
        for (auto& p : all) out ("  part " + juce::String::toHexString (p.partId()) + ": " + p.label);
        bool round = false;
        std::array<MidiToSidCoreEngine::ChannelSuggestion, 3> sug;
        MidiToSidCoreEngine::analyzeMidiFile (juce::File (juce::String::fromUTF8 (argv[2])), round, sug, log);
        out ("auto: " + sug[0].label + " | " + sug[1].label + " | " + sug[2].label + (round ? " (round trip)" : ""));
        return 0;
    }

    if (cmd == "tosid" && argc > 3)
    {
        // SidTest tosid <mid> <out.sid> [auto|roundtrip]
        const juce::File mid (juce::String::fromUTF8 (argv[2])), target (juce::String::fromUTF8 (argv[3]));
        juce::String log;
        bool round = false;
        std::array<MidiToSidCoreEngine::ChannelSuggestion, 3> sug;
        if (! MidiToSidCoreEngine::analyzeMidiFile (mid, round, sug, log)) { out ("analyse failed " + log); return 1; }
        if (argc > 4 && juce::String (argv[4]) == "roundtrip") round = true; else if (argc > 4) round = false;
        std::array<int, 3> idx { sug[0].partId(), sug[1].partId(), sug[2].partId() };
        const bool ok = MidiToSidCoreEngine::convertMidiToSidFile (mid, target, idx, round, log);
        out ((ok ? "ok " : "FEHLER ") + log.trim());
        return ok ? 0 : 1;
    }

    if (cmd == "un7z" && argc > 3)
    {
        // SidTest un7z <archive.7z> <dest> : extracts everything (also lists)
        const juce::File arc (juce::String::fromUTF8 (argv[2])), dest (juce::String::fromUTF8 (argv[3]));
        juce::String err;
        juce::StringArray skipped;
        const auto t0 = juce::Time::getMillisecondCounter();
        const bool ok = SevenZip::extractAll (arc, dest, [] (juce::uint64, juce::uint64, const juce::String&) { return true; }, err, &skipped);
        out ((ok ? "ok" : "FEHLER: " + err) + juce::String (" in ") + juce::String ((int) (juce::Time::getMillisecondCounter() - t0)) + " ms, skipped " + juce::String (skipped.size()));
        return ok ? 0 : 1;
    }

    if (cmd == "hvscverify" && argc > 3)
    {
        // SidTest hvscverify <C64Music root> <UpdateNN.hvs> : parses the script, compares header patches with the real files
        const juce::File root (juce::String::fromUTF8 (argv[2])), script (juce::String::fromUTF8 (argv[3]));
        juce::StringArray warnings, mismatches;
        const auto ops = parseHvscScript (readDocumentText (script), warnings);
        int counts[20] = {};
        for (auto& o : ops) ++counts[(int) o.type];
        out ("Operationen: " + juce::String ((int) ops.size()) + " (replace " + juce::String (counts[0]) + ", move " + juce::String (counts[1]) + ", delete " + juce::String (counts[2])
             + ", mkdir " + juce::String (counts[3]) + ", Kopfdaten " + juce::String ((int) ops.size() - counts[0] - counts[1] - counts[2] - counts[3]) + ")");
        for (int i = 0; i < std::min (10, warnings.size()); ++i) out ("Warnung: " + warnings[i]);
        out ("Warnungen gesamt: " + juce::String (warnings.size()));
        int checked = 0;
        const int ok = verifyHvscHeaders (root, ops, mismatches, &checked);
        out ("Kopfdaten geprüft: " + juce::String (checked) + ", übereinstimmend: " + juce::String (ok));
        for (int i = 0; i < std::min (15, mismatches.size()); ++i) out ("  Abweichung: " + mismatches[i]);
        return 0;
    }

    if (cmd == "hvscapply" && argc > 4)
    {
        // SidTest hvscapply <root> <script.hvs> <backupDir> : applies a script (only ever run on a scratch copy!)
        const juce::File root (juce::String::fromUTF8 (argv[2])), script (juce::String::fromUTF8 (argv[3])), backup (juce::String::fromUTF8 (argv[4]));
        juce::StringArray warnings;
        const auto ops = parseHvscScript (readDocumentText (script), warnings);
        HvscApplyResult r;
        const bool ok = applyHvscScript (root, ops, backup, r);
        out (juce::String (ok ? "ok" : "abgebrochen") + ": verschoben " + juce::String (r.moved) + ", ersetzt " + juce::String (r.replaced) + ", gelöscht " + juce::String (r.deleted)
             + ", Köpfe " + juce::String (r.patched) + ", fehlend " + juce::String (r.missing) + ", Fehler " + juce::String (r.errors));
        for (int i = 0; i < std::min (20, r.messages.size()); ++i) out ("  " + r.messages[i]);
        return r.errors == 0 ? 0 : 1;
    }

    if (cmd == "hvscinstall" && argc > 5)
    {
        // SidTest hvscinstall <root> <version> <package.7z|zip> <backupDir> : full install chain (only on a scratch copy!)
        const juce::File root (juce::String::fromUTF8 (argv[2])), pkg (juce::String::fromUTF8 (argv[4])), backup (juce::String::fromUTF8 (argv[5]));
        const auto r = installHvscPackage (root, juce::String (argv[3]).getIntValue(), pkg, backup, [] (const juce::String& s, double f)
        {
            static juce::String last;
            if (s != last) { last = s; out ("  " + s + " " + juce::String (juce::roundToInt (f * 100)) + " %"); }
            return true;
        });
        out (juce::String (r.ok ? "ok" : "FEHLER: " + r.error) + ": kopiert " + juce::String (r.copiedFiles) + ", verschoben " + juce::String (r.applied.moved) + ", ersetzt "
             + juce::String (r.applied.replaced) + ", gelöscht " + juce::String (r.applied.deleted) + ", Köpfe " + juce::String (r.applied.patched)
             + ", fehlend " + juce::String (r.applied.missing) + ", Fehler " + juce::String (r.applied.errors));
        for (int i = 0; i < std::min (20, r.applied.messages.size()); ++i) out ("  " + r.applied.messages[i]);
        return r.ok ? 0 : 1;
    }

    if (cmd == "hvscupdate" && argc > 3)
    {
        // SidTest hvscupdate <root> <backupBase> [maxVersion] : the same chain as the HVSC panel (check, download, install)
        const juce::File root (juce::String::fromUTF8 (argv[2])), backupBase (juce::String::fromUTF8 (argv[3]));
        const int maxVersion = argc > 4 ? juce::String (argv[4]).getIntValue() : 9999;
        const int local = Hvsc::detectVersion (root);
        out ("Installiert: #" + juce::String (local));
        const auto dl = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("SIDIMIDI-hvsc-dl-test");
        dl.createDirectory();
        int installed = local;
        for (int v = local + 1; v <= maxVersion; ++v)
        {
            const auto name = HvscOnline::updateFileName (v);
            juce::String used;
            const auto size = HvscOnline::remoteSize (name, &used);
            if (size < 0) { out ("Update #" + juce::String (v) + " gibt es nicht (Ende)"); break; }
            out ("Update #" + juce::String (v) + ": " + juce::String (size) + " Bytes von " + used);
            juce::String err;
            const auto file = dl.getChildFile (name);
            if (! HvscOnline::download (name, file, [] (juce::int64, juce::int64) { return true; }, err)) { out ("Download-Fehler: " + err); return 1; }
            const auto r = installHvscPackage (root, v, file, backupBase.getChildFile ("Update" + juce::String (v)), {});
            out (juce::String (r.ok ? "  ok" : "  FEHLER: " + r.error) + ": kopiert " + juce::String (r.copiedFiles) + ", verschoben " + juce::String (r.applied.moved)
                 + ", ersetzt " + juce::String (r.applied.replaced) + ", gelöscht " + juce::String (r.applied.deleted) + ", Köpfe " + juce::String (r.applied.patched)
                 + ", fehlend " + juce::String (r.applied.missing) + ", Fehler " + juce::String (r.applied.errors) + ", Warnungen " + juce::String (r.warnings.size()));
            for (int i = 0; i < std::min (8, r.applied.messages.size()); ++i) out ("    " + r.applied.messages[i]);
            file.deleteFile();
            if (! r.ok) return 1;
            installed = v;
        }
        dl.deleteRecursively();
        out ("Jetzt installiert: #" + juce::String (Hvsc::detectVersion (root)) + " (vorher #" + juce::String (local) + ")");
        return installed > local ? 0 : 2;
    }

    out ("usage: SidTest info <file> | hvsc <folder> | scanstat <folder> [max] | hvscverify <root> <script.hvs> | hvscapply <root> <script.hvs> <backup>");
    return 2;
}
