# miniplayer2 (vendored)

Source: https://github.com/cadaver/miniplayer2 (Lasse Oorni / cadaver), MIT
licensed - see `LICENSE.txt`.

Used as the C64 playback routine for SIDIMIDI's MIDI-to-SID conversion
(`MidiToSidCore.h`): our own encoder generates a DASM assembly source file
containing the song's note/instrument data as `dc.b` tables (same style
`gt2mini2.c` emits), `include`s `player.s`, and adds a minimal PSID-header +
init stub (see `sidexample.s` for the reference template). `tools/dasm.exe`
(prebuilt DASM v2.20.17, GPL-2.0, see `LICENSE.txt`) then assembles that
into the final `.sid` file. This happens at conversion time, not
build/ship time - `dasm.exe` is invoked as an external process from
`MidiToSidCore.h`, the same way a compiler would be invoked, and is not
linked into SIDIMIDI itself.

`gt2mini2.c`/`fileio.c`/`fileio.h`/`gcommon.h`/`mw4title.sng` are kept here
only as the reference converter (GoatTracker2 `.sng` -> miniplayer2 data)
that documents the exact byte-level target format - SIDIMIDI does not read
`.sng` files itself, it encodes directly from MIDI note data into the same
target format.

Verified: assembling the vendored `sidexample.s` (using `mw4title.sng` run
through `gt2mini2.c`) with `tools/dasm.exe` reproduces cadaver's official
prebuilt `example.sid` byte-for-byte from the PSID header's data offset
onward (only unused header-padding bytes differ) - confirms the toolchain
works correctly on this machine before building our own encoder on top of it.
