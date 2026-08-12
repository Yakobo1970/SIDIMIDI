# miniplayer2 data format (reverse-engineered from `player.s`)

Derived by reading `player.s` directly (the player is the ground truth; the
converter `gt2mini2.c` builds the same format but from GT2 songs, which we
don't need). Validated end-to-end: `minimal_example.s` assembles with
`tools/dasm.exe` and, when traced through SIDIMIDI's own 6502 emulator
(`SidCpu6502Engine`), produces exactly the expected register writes
(correct ADSR, waveform, and frequency - `0x22D0`, exactly 16x the `freqTbl`
C0 entry, confirms correct octave math for a C4 note). This is the format
`MidiToSidCore.h`'s encoder targets, generated as `dc.b` tables in DASM
assembly text (not raw binary) - see `ThirdParty/miniplayer/README.md`.

Config used: `PLAYER_ZPOPT=1`, `PLAYER_SFX=0`, `PLAYER_MODULES=0` (single
music, assembled together with the player - "bare mode" in gt2mini2 terms).
With `PLAYER_MODULES=0`, `player.s` does NOT alias `songTbl` etc. to
`dummyData` - our generated source must define those label names directly,
placed *before* `include "player.s"` (and page-aligned: music data must
start exactly at `$1000` since `player.s` requires patterns to not cross
pages).

## Layout or a generated file

1. PSID header (`dc.b`, 124 bytes) - `init`/`play` addresses filled via
   `dc.b >Init,<Init` / `dc.b >PlayRoutine,<PlayRoutine` (DASM resolves the
   labels; `PlayRoutine` is defined inside `player.s`, `Init` is our own
   tiny stub at the end).
2. `rorg $1000`, then immediately the music data tables (order matters only
   in that everything must be defined before use is *resolved*, but DASM is
   multi-pass so textual order doesn't matter much - convention: tables
   first, patterns after, to mirror `gt2mini2.c`'s own output style).
3. `PLAYER_ZPBASE/ZPOPT/SFX/MODULES` config, then `include "player.s"`.
4. `Init:` stub (3 instructions, copied verbatim from `sidexample.s`).

## Tables

- `songTbl`: per subtune, 5 bytes: `dc.w <songN label>` (address of the
  shared orderlist table for this subtune) + 3 bytes = the starting byte
  offset *within that table* for channel 0/1/2 respectively. All 3 SID
  voices share ONE orderlist table, just starting at different offsets -
  there is no separate per-channel table.
- `pattTblLo` / `pattTblHi`: low/high address bytes, one per pattern,
  **1-based** (`pattTblLo-1,y` in the player code - pattern number 1 is
  index 0). Pattern number 0 in the orderlist has a different meaning
  (SONGJUMP), so real patterns start at 1.
- `insAD` / `insWavePos` / `insPulsePos` / `insFiltPos`: one entry per
  instrument, **0-based** (no `-1` offset in the player - instrument value
  IS the array index directly). This is the opposite convention from
  patterns and is an easy off-by-one trap (hit it once during validation:
  using instrument value `1` for a table with only one entry at index 0
  reads one byte past the end and corrupts everything downstream).
  - `insAD`: literal `$D405,x` byte (attack<<4 | decay).
  - `insWavePos`: 1-based index into the wave-step tables below (0 = "no
    wavetable", only valid for legato-continuation instruments).
  - `insPulsePos`/`insFiltPos`: 1-based index into pulse/filter tables, or
    `0` to skip pulse/filter modulation entirely for that instrument (our
    v1 encoder always uses 0 here - no pulse-width or filter automation).
- `waveTbl` / `waveSRTbl` / `waveNextTbl` / `noteTbl`: parallel "wave step"
  tables, one entry per step, **1-based** (`waveTbl-1,y`). For a plain
  sustained note (no vibrato/slide/arpeggio), a single step is enough:
  - `waveTbl[i]`: literal SID control-register byte written to `$D404,x`
    (waveform bits 4-7 + gate bit 0), e.g. `$11` = triangle+gate, `$21` =
    saw+gate, `$81` = noise+gate. Same bit layout `SIDVoiceData` already
    uses elsewhere in this codebase.
  - `waveSRTbl[i]`: if non-zero, overrides `$D406,x` (sustain/release) that
    was set to `$0F` by default at note-on. `0` keeps the default.
  - `waveNextTbl[i]`: next wave-step index, or `0` to stop (hold current
    state - fine for a sustained note, SID keeps the register value).
  - `noteTbl[i]`: signed semitone offset added to the note before indexing
    `freqTbl` (word table, 2 bytes/semitone) - `0` for no transposition.
    High bit set = "absolute" mode (not used by our encoder).
  - Pulse/filter tables (`pulseLimitTbl`/`pulseSpdTbl`/`pulseNextTbl`,
    `filtLimitTbl`/`filtSpdTbl`/`filtNextTbl`) are "destination compare"
    step tables with the same shape; **must still exist as valid labels**
    even when unused (all instruments have `insPulsePos=insFiltPos=0`) -
    `player.s` references them unconditionally. A single `dc.b 0` per
    table is enough since they're never actually indexed into at runtime
    in that case.

## Orderlist (the shared `songN` table)

Sequence of bytes per channel segment, terminated implicitly by looping
(there's no real "end", loop back via `SONGJUMP`):
- `1-127`: play this pattern number (1-based), then advance.
- `0`, followed by 1 byte: SONGJUMP - jump the channel's position within
  this table to the given byte offset (used for looping/repeats).
- `$80 | (semitones<<1)`, followed by 1 byte (the pattern number): sets a
  transpose value applied to all notes in that pattern, then plays it.

## Pattern bytes

Sequence of events, each: `[duration] note_or_command [instrument]`,
terminated by a `0` byte (end of pattern -> back to the orderlist for the
next entry).

- Duration byte is **optional per event** - only present when its value has
  bit 7 set (i.e. `$80-$FF`), stored as `256-durationInFrames` (two's
  complement countdown), covering **1-128 frames** (~20ms-2.56s at 50Hz).
  If omitted, the previous duration is reused. Because the very first event
  reuses whatever garbage is in `chnDuration` if omitted, our encoder
  **always emits an explicit duration byte on every note** rather than ever
  relying on reuse - simpler and avoids relying on power-on zero-page state.
  Notes longer than 128 frames are not directly representable; not yet
  handled by the v1 encoder (candidate follow-up: chain via a legato
  instrument, which changes `chnWavePos` without retriggering gate/ADSR).
- Note/command byte, compared against `REST = $7a` (122):
  - `< $7a`: a note. Encoded as `noteIndex*2 + hasNoInstrument`, where
    `noteIndex` 1-60 spans 5 octaves (`player.s` equates: `C0=1`... up to
    `H4=60`) and bit 0 means "no instrument byte follows, reuse the
    current instrument" (1) vs "instrument byte follows" (0). Our encoder
    always sends the instrument explicitly on the first note of a run.
  - `== $7a` (`REST`): silence, no note/instrument follows.
  - `== $7b` (`WAVEPOS`): followed by 1 byte = new `chnWavePos` value
    directly (jump to a specific wavetable step without a new note).
  - `>= $7c`: direct SID register poke - low 3 bits of the command byte are
    the register offset from `$D400,x` (`$7c`&7=4 -> control/`$D404,x`,
    `$7d`&7=5 -> AD/`$D405,x`, `$7e`&7=6 -> SR/`$D406,x`), followed by 1
    byte = the value to write. Not used by the v1 encoder.
- Instrument byte (0-based, see above) - present only when the note byte's
  bit 0 is clear.

## Known v1 encoder limitations (deliberate, to keep scope tractable)

- No pulse-width or filter modulation (`insPulsePos`/`insFiltPos` always 0).
- No vibrato/slide/wavetable arpeggios (always a single wave step per
  instrument).
- Notes longer than 128 frames not yet split/chained.
- Instrument palette is small and chosen from a simple MIDI-program
  heuristic (see `MidiToSidCore.h`), not a full GM mapping.
