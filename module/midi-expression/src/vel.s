| SPDX-License-Identifier: GPL-3.0-or-later
| MIDI Expression: the velocity of notes from MIDI IN (OS 1.54).
| The note router (0x400d5c1c) calls the live note-on for each routed track:
|     400d5d8e  jsr (%a2)            | %a2 = 0x400d5604, live_note
|     400d5d90  lea 28(%sp),%sp      | (track, note, velocity, flag, 0, -1, -1)
| A 6-byte jsr site over those two instructions comes here. me_note_velocity
| (audio.c) gives the velocity to use; the stock live note gets the same
| seven arguments with it, and the caller's arguments are dropped as its own
| lea did. Only %d0/%d1/%a0/%a1 change; the router keeps nothing in them.
        .equ    LIVE_NOTE, 0x400d5604

        .section .run, "ax"
        .balign 2
        .globl  me_note_hook
me_note_hook:                           | (sp) return, 4 track, 8 note, 12 velocity, 16 flag ...
        move.l  16(%sp), -(%sp)         | flag
        move.l  16(%sp), -(%sp)         | velocity
        move.l  12(%sp), -(%sp)         | track
        jsr     me_note_velocity
        lea     12(%sp), %sp
        move.l  %d0, 12(%sp)            | the velocity to play and record
        .rept   7
        move.l  28(%sp), -(%sp)         | the seven arguments again
        .endr
        jsr     LIVE_NOTE
        lea     28(%sp), %sp
        movea.l (%sp)+, %a0             | back to the router,
        lea     28(%sp), %sp            | without its arguments (its own lea)
        jmp     (%a0)
