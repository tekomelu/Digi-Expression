| SPDX-License-Identifier: GPL-3.0-or-later
| MIDI Expression: three checkbox rows in SETTINGS, PB THRU, AT THRU and
| MW THRU, like the stock ones: YES toggles, RIGHT ticks, LEFT unticks.
| Unticked (stock) at every power-on; not saved.
|
| The row record (label, select, draw, change) and the OS 1.54 addresses
| below follow the SETTINGS row FX Track documents (draadpiraat, FX Track,
| GPL-3.0-or-later); this file is written for this mod.

        .equ STR_CTOR,   0x4017af20     | std::string(this, const char*, alloc&)
        .equ INVALIDATE, 0x400c9a3a     | View::invalidate(view)
        .equ BLIT,       0x400c2b88     | blit(dst, src, x, y, centre)
        .equ CHECKBOXES, 0x421f8a3c     | -> Bitmap[2], 0x1c each: empty, ticked

        .section .run, "ax"

| ---- ev_settings: add the three rows --------------------------------------
        .balign 2
        .globl  me_settings
me_settings:
        pea     me_row_pb
        move.l  8(%sp), -(%sp)          | the menu
        jsr     core_additem
        addq.l  #8, %sp
        pea     me_row_at
        move.l  8(%sp), -(%sp)
        jsr     core_additem
        addq.l  #8, %sp
        pea     me_row_mw
        move.l  8(%sp), -(%sp)
        jsr     core_additem
        addq.l  #8, %sp
        rts

| ---- shared row code; %a1 holds the row's setting byte --------------------

| label: build the row's name into the string %a0 points to (returned in d0).
| %a1: the name.
row_label:
        link    %a6, #-4
        move.l  %d2, -(%sp)
        pea     -1(%a6)                 | the allocator
        move.l  %a1, -(%sp)             | the name
        move.l  %a0, %d2
        move.l  %a0, -(%sp)             | the string to build
        jsr     STR_CTOR
        lea     12(%sp), %sp
        move.l  %d2, %d0
        move.l  -8(%a6), %d2
        unlk    %a6
        rts

| set: %d0 = 0 or 1 into the byte at %a1, then redraw the menu.
| 4(%sp): the row's argument, whose first long is the menu.
row_set:
        move.b  %d0, (%a1)
        movea.l 4(%sp), %a0
        movea.l (%a0), %a0              | the menu
        lea     0x38(%a0), %a0
        move.l  %a0, 4(%sp)
        jmp     INVALIDATE

row_select:                             | YES: toggle
        moveq   #0, %d0
        tst.b   (%a1)
        bne.s   1f
        moveq   #1, %d0
1:      bra.s   row_set

row_change:                             | LEFT/RIGHT: 12(%sp) is the delta
        move.l  12(%sp), %d0
        beq.s   9f
        bmi.s   1f
        moveq   #1, %d0                 | RIGHT: tick
        bra.s   row_set
1:      moveq   #0, %d0                 | LEFT: untick
        bra.s   row_set
9:      rts

| draw: blit the empty or ticked box. draw(row, ?, dst, x, y) -> blit(dst, box, x, y, 0)
row_draw:
        moveq   #0, %d0
        tst.b   (%a1)
        beq.s   1f
        moveq   #0x1c, %d0              | the ticked box
1:      add.l   CHECKBOXES, %d0
        move.l  12(%sp), %d1
        move.l  %d1, 4(%sp)
        move.l  %d0, 8(%sp)
        move.l  16(%sp), %d1
        move.l  %d1, 12(%sp)
        move.l  20(%sp), %d1
        move.l  %d1, 16(%sp)
        clr.l   20(%sp)
        jmp     BLIT

| ---- one row per setting ---------------------------------------------------
        .macro  ROW tag, flag, text
\tag\()_label:
        lea     \tag\()_name, %a1
        bra     row_label
\tag\()_select:
        lea     \flag, %a1
        bra     row_select
\tag\()_draw:
        lea     \flag, %a1
        bra     row_draw
\tag\()_change:
        lea     \flag, %a1
        bra     row_change
        .balign 4
me_row_\tag:
        .long   \tag\()_label, \tag\()_select, \tag\()_draw, \tag\()_change
\tag\()_name:
        .asciz  "\text"
        .balign 2
        .endm

        ROW     pb, me_pb_on, "PB THRU"
        ROW     at, me_at_on, "AT THRU"
        ROW     mw, me_mw_on, "MW THRU"
