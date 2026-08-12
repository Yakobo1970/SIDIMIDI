; Minimal hand-written test song: 1 voice, 1 note (C-4-ish), long duration, to
; validate my understanding of the miniplayer2 data format before writing the
; C++ encoder.

                processor 6502
                org $0000

SUBTUNES        = 1

                dc.b "PSID"
                dc.b 0,2
                dc.b 0,$7c
                dc.b $00,$00
                dc.b >Init,<Init
                dc.b >PlayRoutine,<PlayRoutine
                dc.b 0,SUBTUNES
                dc.b 0,1
                dc.b 0,0,0,0

                org $0016
                dc.b "MidiToSid format test"

                org $0036
                dc.b "Claude"

                org $007c
                dc.b $00,$10
                rorg $1000

        ; ---- Music data (must start page-aligned at $1000) ----

songTbl:        dc.w song0
                dc.b $00,$03,$06      ; chn0 start=0, chn1 start=3, chn2 start=6

pattTblLo:      dc.b <patt1
                dc.b <pattEmpty
pattTblHi:      dc.b >patt1
                dc.b >pattEmpty

insAD:          dc.b $18              ; attack=1,decay=8
insWavePos:     dc.b 1                ; -> waveTbl step 1
insPulsePos:    dc.b 0
insFiltPos:     dc.b 0

waveTbl:        dc.b $11              ; triangle + gate
waveSRTbl:      dc.b $c6              ; sustain=12,release=6
waveNextTbl:    dc.b 0                ; stop after this step (hold)
noteTbl:        dc.b 0                ; no transpose

pulseLimitTbl:  dc.b 0
pulseSpdTbl:    dc.b 0
pulseNextTbl:   dc.b 0
filtLimitTbl:   dc.b 0
filtSpdTbl:     dc.b 0
filtNextTbl:    dc.b 0

        ; song0: shared orderlist table. chn0 uses patt1 then loops; chn1/2 use pattEmpty forever.
                ; offset 0 (chn0 start):
song0:          dc.b 1                ; pattern 1
                dc.b 0,0              ; SONGJUMP to offset 0 (loop)
                ; offset 3 (chn1 start):
                dc.b 2                ; pattern 2 (pattEmpty)
                dc.b 0,3              ; SONGJUMP to offset 3 (loop)
                ; offset 6 (chn2 start):
                dc.b 2                ; pattern 2 (pattEmpty)
                dc.b 0,6              ; SONGJUMP to offset 6 (loop)

        ; patt1: one note (C4, note-index per player.s equates), duration 100 frames, instrument 1 (new)
C4EQ            = 49*2+1
patt1:          dc.b $9c              ; duration byte: 256-100 = $9c (100 frames)
                dc.b (C4EQ-1)         ; note byte WITHOUT the "+1" -> instrument follows
                dc.b 0                ; instrument 0 (instruments are 0-based, unlike patterns!)
                dc.b 0                ; end of pattern

pattEmpty:      dc.b $80              ; duration byte: 256-128 = $80 (128 frames, max)
                dc.b REST
                dc.b 0

        ; ---- Player configuration ----

PLAYER_ZPBASE   = $20
PLAYER_ZPOPT    = 1
PLAYER_SFX      = 0
PLAYER_MODULES  = 0

                include "player.s"

Init:           tax
                inx
                stx PlayRoutine+1
                rts
