// SidTest – console checks for the core of SIDiMIDI (no user interface).
//   SidTest info <file.sid>
//   SidTest hvsc <C64Music folder>          documents: version, lengths, STIL, bugs, musicians
//   SidTest scanstat <folder> [max]         header statistics over a folder tree
#include "../Source/AudioPlayerEngine.h"
#include <juce_core/juce_core.h>
#include "../Source/SidInfo.h"
#include "../Source/Hvsc.h"
#include "../Source/Archive.h"
#include "../Source/SidTrace.h"
#include "../Source/SevenZip.h"
#include "../Source/HvscUpdate.h"
#include "../Source/SidToMidiCore.h"
#include "../Source/MidiToSidCore.h"
#include "../Source/NpTune.h"
#include "../Source/NpFromMidi.h"
#include "../Source/InstrumentPool.h"
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

    if (cmd == "tracedump" && argc > 2)
    {
        // SidTest tracedump <file> [song] [maxEvents]: lists the first register writes of the trace
        const auto si = parseSidFile (juce::File (juce::String::fromUTF8 (argv[2])));
        const int song = argc > 3 ? juce::String (argv[3]).getIntValue() : si.startSong;
        const int maxEv = argc > 4 ? juce::String (argv[4]).getIntValue() : 80;
        auto t = computeTrace (si, song, 20.0);
        out ("frames " + juce::String ((int) t->frames) + " events " + juce::String (t->events ? (int) t->events->size() : 0) + " fps " + juce::String (t->fps, 3));
        int n = 0;
        if (t->events)
            for (auto& e : *t->events)
            {
                if (n++ >= maxEv) break;
                out ("f" + juce::String ((int) e.frameIndex) + " reg " + juce::String::toHexString ((int) e.reg) + " = " + juce::String::toHexString ((int) e.value));
            }
        return 0;
    }

    if (cmd == "render" && argc > 2)
    {
        // SidTest render <file> [song] [seconds]: runs the player engine offline and prints the loudness per second
        const auto si = parseSidFile (juce::File (juce::String::fromUTF8 (argv[2])));
        const int song = argc > 3 ? juce::String (argv[3]).getIntValue() : si.startSong;
        const int secs = argc > 4 ? juce::String (argv[4]).getIntValue() : 12;
        auto t = computeTrace (si, song, (double) secs + 1.0);
        AudioPlayerEngine pl (false);
        pl.prepareToPlay (512, 44100.0);
        pl.playTrace (*t->events, t->fps, t->frames, 0, si.ntsc());
        juce::AudioBuffer<float> buf (2, 512);
        for (int s = 0; s < secs; ++s)
        {
            double sum = 0; int n = 0;
            for (int b = 0; b < 44100 / 512; ++b)
            {
                buf.clear();
                pl.getNextAudioBlock ({ &buf, 0, 512 });
                for (int i = 0; i < 512; ++i) { sum += buf.getSample (0, i) * buf.getSample (0, i); ++n; }
            }
            out ("second " + juce::String (s) + ": rms " + juce::String (std::sqrt (sum / std::max (1, n)), 4));
        }
        return 0;
    }

    if (cmd == "edittest" && argc > 3)
    {
        // SidTest edittest <db-file> <folder with copies of SID files>: writes a title, reads it back, undoes it
        _putenv_s ("SIDIMIDI_DB", argv[2]);
        juce::MessageManager::getInstance();
        Archive a;
        a.addFolder (juce::File (juce::String::fromUTF8 (argv[3])));
        a.startScan (true);
        a.waitForScan();
        if (a.entries().empty()) { out ("no entries"); return 1; }
        auto& e = *a.entries().front();
        const auto before = parseSidFile (juce::File (e.path), false);
        juce::String err;
        const bool ok = a.setSidField (e, Archive::fieldTitle, juce::String::fromUTF8 ("Neuer Titel äöü"), err);
        const auto after = parseSidFile (juce::File (e.path), false);
        out ("write " + juce::String ((int) ok) + " err '" + err + "' title '" + after.title + "' entry '" + e.title + "' md5 changed " + juce::String ((int) (after.md5 != before.md5)) + " songs " + juce::String (after.songs) + "/" + juce::String (before.songs) + " load " + juce::String ((int) (after.effLoad == before.effLoad)));
        const bool ok2 = a.setSidField (e, Archive::fieldAuthor, juce::String::fromUTF8 ("€ nicht möglich"), err);
        out ("euro: " + juce::String ((int) ok2) + " " + err);
        const bool ok3 = a.setSidField (e, Archive::fieldReleased, "x123456789012345678901234567890123456", err);
        out ("too long: " + juce::String ((int) ok3) + " " + err);
        out ("undo available " + juce::String ((int) a.canUndo()) + " '" + a.undoLabel() + "'");
        juce::String failed;
        const int n = a.undo (failed);
        const auto restored = parseSidFile (juce::File (e.path), false);
        out ("undo " + juce::String (n) + " failed '" + failed + "' title '" + restored.title + "' md5 equal " + juce::String ((int) (restored.md5 == before.md5)));
        out ("backup exists " + juce::String ((int) a.backupFolder().isDirectory()));
        return 0;
    }

    if (cmd == "npd64" && argc > 2)
    {
        // SidTest npd64 <disk.d64> [dump]: loads every worktune of the disk, checks the round trip and plays it in the emulator
        std::vector<NpD64File> files;
        const juce::File img (juce::String::fromUTF8 (argv[2]));
        if (! npD64List (img, files)) { out ("not a d64"); return 1; }
        int ok = 0, bad = 0;
        for (auto& f : files)
        {
            juce::MemoryBlock mb;
            if (! npD64Extract (img, f, mb) || mb.getSize() < 100 || ((const juce::uint8*) mb.getData())[1] != 0x0f) continue;
            NpTune t; juce::String err;
            if (! t.loadPrg (mb.getData(), mb.getSize(), err)) { out (f.name + ": " + err); ++bad; continue; }
            const auto back = t.toPrg();
            const bool same = back.getSize() == mb.getSize() && std::memcmp (back.getData(), mb.getData(), mb.getSize()) == 0;
            const auto psid = t.toPsid (f.name, "JCH", "test");
            const auto si = parseSidBytes (psid.getData(), psid.getSize(), true);
            auto tr = computeTrace (si, 1, 15.0);
            int act = 0; if (tr) for (int v = 0; v < 3; ++v) for (juce::uint32 i = 0; i < tr->frames; ++i) act += tr->v[v].note[i] >= 0;
            out (f.name.paddedRight (' ', 18) + " player " + t.playerName().paddedRight (' ', 6) + " speed " + juce::String (t.speed()) + " tracks " + juce::String (t.trackListLength (0)) + "/" + juce::String (t.trackListLength (1)) + "/" + juce::String (t.trackListLength (2))
                 + " seqs " + juce::String ((int) t.usedSequences().size()) + " est " + juce::String (t.estimateSeconds(), 1) + "s round-trip " + (same ? "OK" : "DIFF") + " events " + juce::String (tr && tr->events ? (int) tr->events->size() : 0) + " sounding " + juce::String (act));
            ++ok;
            {
                // start from the third track entry: the copy must still play
                const auto t2 = t.withStartIndex ({ 2, 2, 2 });
                const auto p2 = t2.toPsid ("x", "x", "x");
                auto tr2 = computeTrace (parseSidBytes (p2.getData(), p2.getSize(), true), 1, 8.0);
                int act2 = 0; if (tr2) for (int v = 0; v < 3; ++v) for (juce::uint32 i = 0; i < tr2->frames; ++i) act2 += tr2->v[v].note[i] >= 0;
                out ("    from entry 2: tracks " + juce::String (t2.trackListLength (0)) + "/" + juce::String (t2.trackListLength (1)) + "/" + juce::String (t2.trackListLength (2)) + " sounding " + juce::String (act2));
            }
            if (argc > 3 && ok == 1)
            {
                // text dump of the first sequence of voice 1
                auto tl = t.trackList (0);
                if (! tl.empty())
                {
                    out ("track 0: " + juce::String::toHexString (tl[0].trans) + " " + juce::String::toHexString (tl[0].seq));
                    for (auto& r : t.sequence (tl[0].seq)) out ("  " + juce::String::toHexString (r.cmd) + " " + juce::String::toHexString (r.note));
                }
            }
        }
        out (juce::String (ok) + " tunes ok, " + juce::String (bad) + " rejected");
        return 0;
    }

    if (cmd == "npedit" && argc > 3)
    {
        // SidTest npedit <template.prg> <out.sid>: new tune from the template, writes a short melody, exports and plays it
        NpTune t; juce::String err;
        if (! t.loadFile (juce::File (juce::String::fromUTF8 (argv[2])), err)) { out (err); return 1; }
        out ("template: player " + t.playerName() + " speed " + juce::String (t.speed()) + " seq0 rows " + juce::String ((int) t.sequence (0).size()) + " usable1 " + juce::String ((int) t.sequenceUsable (1)));
        t.setSpeed (4);
        // instrument 1: saw lead
        const juce::uint8 ins[8] = { 0x08, 0xa8, 0x80, 0x00, 0x00, 0x00, 0x04, 0x04 };   // v20: ADSR, hard restart on, arpeggio pointers 4
        for (int i = 0; i < 8; ++i) t.set (NpTune::tabInstrument, 1, i, ins[i]);
        t.set (NpTune::tabArpeggio, 4, 0, 0x00); t.set (NpTune::tabArpeggio, 4, 1, 0x21);   // saw, no note add
        t.set (NpTune::tabArpeggio, 5, 0, 0x7f); t.set (NpTune::tabArpeggio, 5, 1, 0x04);   // wrap to row 4
        std::vector<NpRow> rows;
        const int notes[] = { 48, 52, 55, 60, 55, 52, 48, 0 };
        for (int i = 0; i < 8; ++i) { NpRow r; if (i == 0) r.cmd = 0xa1; r.note = (juce::uint8) notes[i]; rows.push_back (r); rows.push_back ({ 0x80, 0x7e }); }
        t.makeBlankSequence (1, 16);
        t.setSequence (1, rows);
        t.setTrackAll (0, { { 0x8c, 1 }, { 0x8c, 1 }, { 0xff, 0 } });
        const auto psid = t.toPsid ("Tracker test", "SIDiMIDI", "2026");
        juce::File (juce::String::fromUTF8 (argv[3])).replaceWithData (psid.getData(), psid.getSize());
        const auto si = parseSidBytes (psid.getData(), psid.getSize(), true);
        auto tr = computeTrace (si, 1, 12.0);
        int act[3] = {};
        if (tr) for (int v = 0; v < 3; ++v) for (juce::uint32 i = 0; i < tr->frames; ++i) act[v] += tr->v[v].note[i] >= 0;
        if (tr) { juce::String seq; float last = -9; for (juce::uint32 i = 0; i < tr->frames && seq.length() < 80; ++i) { const float n = tr->v[0].note[i]; if (n >= 0 && std::abs (n - last) > 0.4f) { seq << juce::String (n, 1) << " "; last = n; } } out ("voice 1 notes: " + seq); }
        out ("trace frames " + juce::String (tr ? (int) tr->frames : 0) + " sounding frames per voice " + juce::String (act[0]) + "/" + juce::String (act[1]) + "/" + juce::String (act[2]) + " note range " + juce::String (tr ? tr->minNote : 0) + ".." + juce::String (tr ? tr->maxNote : 0));
        return 0;
    }

    if (cmd == "pooltest" && argc > 2)
    {
        // SidTest pooltest <template.prg>: loads every factory preset into the tune (checks room and round trip) and plays a few
        juce::MemoryBlock tpl;
        if (! juce::File (juce::String::fromUTF8 (argv[2])).loadFileAsData (tpl)) { out ("template not found"); return 1; }
        NpTune t; juce::String err;
        t.loadPrg (tpl.getData(), tpl.getSize(), err);
        t.makeStarter();
        const auto fac = InstrumentPool::factoryPresets();
        out ("factory presets: " + juce::String ((int) fac.size()) + ", player " + t.playerName());
        int slot = 6, bad = 0;
        for (auto& p : fac)
        {
            juce::String e;
            if (! InstrumentPool::apply (t, slot, p, e)) { out ("FAILED " + p.name + ": " + e); ++bad; continue; }
            const auto back = InstrumentPool::capture (t, slot, p.name, p.category);
            const bool sameProg = back.arpOn.rows.size() == p.arpOn.rows.size() && back.pulseSets.size() == p.pulseSets.size();
            if (! sameProg) { out ("ROUND TRIP differs: " + p.name); ++bad; }
            if (++slot > 31) slot = 6;
        }
        int arpTop = 0; for (int r = 255; r >= 0; --r) if (t.get (NpTune::tabArpeggio, r, 0) || t.get (NpTune::tabArpeggio, r, 1)) { arpTop = r + 1; break; }
        int pulTop = 0; for (int r = 63; r >= 0; --r) if (t.get (NpTune::tabPulse, r, 0) || t.get (NpTune::tabPulse, r, 1) || t.get (NpTune::tabPulse, r, 2) || t.get (NpTune::tabPulse, r, 3)) { pulTop = r + 1; break; }
        out ("arpeggio rows used " + juce::String (arpTop) + " of 256, pulse sets " + juce::String (pulTop) + " of 64, problems " + juce::String (bad));
        // the same preset twice does not grow the tables
        { juce::String e; InstrumentPool::apply (t, 7, fac[1], e); InstrumentPool::apply (t, 8, fac[1], e);
          int a2 = 0; for (int r = 255; r >= 0; --r) if (t.get (NpTune::tabArpeggio, r, 0) || t.get (NpTune::tabArpeggio, r, 1)) { a2 = r + 1; break; }
          out ("after loading a preset twice: arpeggio rows " + juce::String (a2)); }
        for (const char* name : { "Kick", "Puls (PWM)", "Dur", "Hi-Hat offen" })
        {
            for (auto& p : fac) if (p.name == name)
            {
                NpTune u; u.loadPrg (tpl.getData(), tpl.getSize(), err); u.makeStarter();
                juce::String e; InstrumentPool::apply (u, 9, p, e);
                std::vector<NpRow> rows (16); rows[0].cmd = 0xa0 + 9; rows[0].note = 48;
                u.setSequence (1, rows);
                const auto psid = u.toPsid ("pool", "SIDiMIDI", "");
                const auto si = parseSidBytes (psid.getData(), psid.getSize(), true);
                auto tr = computeTrace (si, 1, 1.5);
                int act = 0; juce::String ns;
                if (tr) for (juce::uint32 i = 0; i < tr->frames; ++i) { if (tr->v[0].note[i] >= 0) ++act; }
                if (tr) { float last = -9; for (juce::uint32 i = 0; i < tr->frames && ns.length() < 70; ++i) { const float n = tr->v[0].note[i]; if (n >= 0 && std::abs (n - last) > 0.4f) { ns << juce::String (n, 0) << " "; last = n; } } }
                out (juce::String (name) + ": sounding frames " + juce::String (act) + "  notes " + ns);
            }
        }
        return bad;
    }

    if (cmd == "mid2np" && argc > 4)
    {
        // SidTest mid2np <template.prg> <in.mid> <out.sid> [out.np]: MIDI -> NewPlayer tune, exports it and plays it in the emulator
        juce::MemoryBlock tpl;
        if (! juce::File (juce::String::fromUTF8 (argv[2])).loadFileAsData (tpl)) { out ("template not found"); return 1; }
        // SIDI_PARTS="2,3;4;5": the tracks of the three voices (several per voice, the first has the highest priority)
        VoiceSources vs = autoVoiceSources();
        if (const char* ev = std::getenv ("SIDI_PARTS"))
        {
            vs = {};
            const auto voices = juce::StringArray::fromTokens (ev, ";", "");
            for (int v = 0; v < 3 && v < voices.size(); ++v)
            {
                int prio = 0;
                for (auto& t : juce::StringArray::fromTokens (voices[v], ",", "")) if (t.isNotEmpty()) vs[(size_t) v].push_back ({ t.getIntValue(), 0, prio++ });
            }
        }
        if (const char* pn = std::getenv ("SIDI_PRESET"))        // SIDI_PRESET="Säge" gives the first track of OSC1 a factory preset
            for (auto& fp : InstrumentPool::factoryPresets()) if (fp.name == juce::String::fromUTF8 (pn) && ! vs[0].empty()) vs[0][0].preset = InstrumentPool::keyOf (fp);
        if (const char* kn = std::getenv ("SIDI_KIT"))           // SIDI_KIT="808" gives the first track of OSC1 a factory drum kit
            for (auto& fk : InstrumentPool::factoryKits()) if (fk.name == juce::String::fromUTF8 (kn) && ! vs[0].empty()) vs[0][0].kit = InstrumentPool::keyOf (fk);
        auto r = npImportMidi (juce::File (juce::String::fromUTF8 (argv[3])), tpl.getData(), tpl.getSize(), vs);
        out (r.report);
        if (! r.ok) return 1;
        const auto psid = r.tune.toPsid ("MIDI import", "SIDiMIDI", "");
        juce::File (juce::String::fromUTF8 (argv[4])).replaceWithData (psid.getData(), psid.getSize());
        if (argc > 5) { juce::String e; r.tune.saveFile (juce::File (juce::String::fromUTF8 (argv[5])), e); }
        const auto si = parseSidBytes (psid.getData(), psid.getSize(), true);
        const double secs = std::min (60.0, r.tune.estimateSeconds() + 1.0);
        auto tr = computeTrace (si, 1, secs);
        int act[3] = {}; float lo = 200, hi = -1;
        if (tr) for (int v = 0; v < 3; ++v) for (juce::uint32 i = 0; i < tr->frames; ++i) { act[v] += tr->v[v].note[i] >= 0; }
        out ("estimated " + juce::String (r.tune.estimateSeconds(), 1) + " s; trace " + juce::String (tr ? (int) tr->frames : 0) + " frames, sounding per voice " + juce::String (act[0]) + "/" + juce::String (act[1]) + "/" + juce::String (act[2]) + "  size " + juce::String ((int) psid.getSize()));
        (void) lo; (void) hi;
        if (tr)
            for (int v = 0; v < 3; ++v)
            {
                juce::String seq; float last = -9;
                for (juce::uint32 i = 0; i < tr->frames && seq.length() < 120; ++i) { const float n = tr->v[v].note[i]; if (n >= 0 && std::abs (n - last) > 0.4f) { seq << juce::String (n, 0) << " "; last = n; } }
                out ("voice " + juce::String (v + 1) + ": " + seq);
            }
        return 0;
    }

    if (cmd == "nptiming" && argc > 4)
    {
        // SidTest nptiming <template.prg> <in.mid> <voice 1-3>: compares the rows of the imported tune with the gate edges the player produces
        juce::MemoryBlock tpl;
        juce::File (juce::String::fromUTF8 (argv[2])).loadFileAsData (tpl);
        NpImport r;
        const juce::File inFile (juce::String::fromUTF8 (argv[3]));
        if (inFile.hasFileExtension ("mid;midi")) r = npImportMidi (inFile, tpl.getData(), tpl.getSize());
        else { juce::String e; r.ok = r.tune.loadFile (inFile, e); r.report = e; }
        if (! r.ok) { out (r.report); return 1; }
        const int voice = juce::String (argv[4]).getIntValue() - 1;
        const int sp = r.tune.speed();
        std::vector<int> expectedRows;
        int row = 0;
        for (auto& e : r.tune.trackList (voice))
            for (auto& rw : r.tune.sequence (e.seq)) { if (rw.note != 0 && rw.note != 0x7e) expectedRows.push_back (row); ++row; }
        const auto psid = r.tune.toPsid ("t", "t", "t");
        auto tr = computeTrace (parseSidBytes (psid.getData(), psid.getSize(), true), 1, std::min (40.0, r.tune.estimateSeconds()));
        std::vector<int> gateOn;
        bool prev = false;
        for (juce::uint32 i = 0; tr && i < tr->frames; ++i) { const bool g = (tr->v[voice].mods[i] & 4) != 0; if (g && ! prev) gateOn.push_back ((int) i); prev = g; }
        out ("speed " + juce::String (sp) + ", note starts in rows " + juce::String ((int) expectedRows.size()) + ", gate rises " + juce::String ((int) gateOn.size()));
        juce::String line;
        for (size_t i = 0; i < expectedRows.size() && i < gateOn.size() && i < 60; ++i)
            line << "(" << (expectedRows[i] * (sp + 1)) << "," << gateOn[i] << "," << gateOn[i] - expectedRows[i] * (sp + 1) << ") ";
        out (line);
        return 0;
    }

    if (cmd == "looptest" && argc > 2)
    {
        // SidTest looptest <file.sid> <seconds of the loop>: offline player with repeat - the frame counter must wrap without the tune ending
        const auto si = parseSidFile (juce::File (juce::String::fromUTF8 (argv[2])));
        const double loopSecs = juce::String (argv[3]).getDoubleValue();
        auto t = computeTrace (si, 1, loopSecs + 2.0);
        AudioPlayerEngine pl (false);
        pl.prepareToPlay (512, 44100.0);
        pl.playTrace (*t->events, t->fps, t->frames, 0, si.ntsc());
        pl.setSidPlayRange ((juce::uint32) std::lround (loopSecs * t->fps), true);
        juce::AudioBuffer<float> buf (2, 512);
        juce::uint32 maxFrame = 0; int wraps = 0; juce::uint32 last = 0; double quiet = 0; int quietBlocks = 0, blocks = 0;
        for (int b = 0; b < (int) (44100.0 * (loopSecs * 3.5) / 512.0); ++b)
        {
            buf.clear();
            pl.getNextAudioBlock ({ &buf, 0, 512 });
            const auto f = pl.getPlayFrame();
            if (f < last) ++wraps;
            last = f; maxFrame = std::max (maxFrame, f);
            ++blocks;
        }
        out ("loop " + juce::String (loopSecs, 1) + " s -> max frame " + juce::String ((int) maxFrame) + " (loop end " + juce::String ((int) std::lround (loopSecs * t->fps)) + "), wraps " + juce::String (wraps) + ", still playing " + juce::String ((int) pl.isSidTunePlaying()) + ", finished " + juce::String ((int) pl.takeSidFinished()));
        (void) quiet; (void) quietBlocks; (void) blocks;
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
