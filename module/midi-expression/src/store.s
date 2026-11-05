| SPDX-License-Identifier: GPL-3.0-or-later
| MIDI Expression: knowing when a project is saved or loaded (OS 1.54).
|
| The firmware's project jobs format their message with sprintf(buf, fmt, name)
| (0x40000e82):
|     4004da62  jsr 0x40000e82      "SAVING '%.16s'"   (SAVE PROJECT AS, every save)
|     4004fe28  jsr 0x40000e82      "LOADING '%.16s'"  (LOAD PROJECT)
| A 6-byte jsr site over each comes here: store.c is told the name and the
| project's slot (0-based; measured in digiemu: %d4 in the save job, %d3 in the
| load job, where it was passed to the project list's name lookup), then
| sprintf runs on with the arguments as the firmware passed them and returns
| to the firmware.
        .equ    SPRINTF, 0x40000e82

        .section .run, "ax"
        .balign 2
        .globl  me_saving_hook
me_saving_hook:                         | (sp) return, 4 buf, 8 fmt, 12 name
        move.l  %d4, -(%sp)                 | the slot
        move.l  16(%sp), -(%sp)             | the name
        jsr     me_project_saving
        addq.l  #8, %sp
        jmp     SPRINTF

        .balign 2
        .globl  me_loading_hook
me_loading_hook:
        move.l  %d3, -(%sp)
        move.l  16(%sp), -(%sp)
        jsr     me_project_loading
        addq.l  #8, %sp
        jmp     SPRINTF
