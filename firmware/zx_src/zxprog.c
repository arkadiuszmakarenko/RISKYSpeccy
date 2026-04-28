/* Minimal zxprog: NMI-driven memory writer + launch trap.
 *
 * Code at 0x0000 (cart ROM).  BSS+stack moved to a tiny region at
 * 0xBE00..0xBEFF so it stays clear of typical snapshot usage in the
 * 0x4000-0xBDFF range.  No screen output, no bridge/view/draw helpers
 * — those have been removed because the CH32-side console + .z80
 * loader no longer need them.
 *
 * Mailboxes (cart RAM, 0x3000-0x3FFF — written by CH32, read here):
 *   WCMD   0x302E..0x3033 + 0x3034.. data window  — generic NMI-write
 *   LAUNCH 0x3F10..0x3F12                          — redirect Z80 PC
 *
 * Protocol (WCMD): CH32 writes DST, LEN, DATA, then bumps SEQ.  We
 * memcpy(DATA -> DST) and write SEQ back to DONE.  CH32 polls DONE.
 *
 * Protocol (LAUNCH): CH32 writes target then bumps SEQ.  We set
 * launch_pending; the NMI return wrapper then pops launch_target into
 * PC instead of resuming our main loop.
 */

#include <string.h>
#include "zx.h"

#pragma output CRT_ORG_CODE  = 0
#pragma output CRT_ORG_BSS   = 0xBE00
#pragma output REGISTER_SP   = 0xBEFF

/* WCMD mailbox */
#define WCMD_SEQ_ADDR    0x302Eu
#define WCMD_DONE_ADDR   0x302Fu
#define WCMD_DST_LO_ADDR 0x3030u
#define WCMD_DST_HI_ADDR 0x3031u
#define WCMD_LEN_LO_ADDR 0x3032u
#define WCMD_LEN_HI_ADDR 0x3033u
#define WCMD_DATA_ADDR   0x3034u
#define WCMD_MAX_DATA    0x0F00u    /* 3840 bytes max per chunk */

/* LAUNCH mailbox */
#define LAUNCH_SEQ_ADDR    0x3F10u
#define LAUNCH_TGT_LO_ADDR 0x3F11u
#define LAUNCH_TGT_HI_ADDR 0x3F12u

/* RCMD mailbox: Z80-driven read.  CH32 writes SRC,LEN, bumps SEQ;
   NMI handler does memcpy(RBUF, SRC, LEN) and writes SEQ back to DONE.
   CH32 then reads RBUF directly via cart RAM shadow.  Max 64 bytes. */
#define RCMD_SEQ_ADDR      0x3F20u
#define RCMD_DONE_ADDR     0x3F21u
#define RCMD_SRC_LO_ADDR   0x3F22u
#define RCMD_SRC_HI_ADDR   0x3F23u
#define RCMD_LEN_ADDR      0x3F24u
#define RCMD_BUF_ADDR      0x3F40u
#define RCMD_MAX_LEN       64u

#define WCMD_SEQ     (*((volatile unsigned char *)WCMD_SEQ_ADDR))
#define WCMD_DONE    (*((volatile unsigned char *)WCMD_DONE_ADDR))
#define WCMD_DST_LO  (*((volatile unsigned char *)WCMD_DST_LO_ADDR))
#define WCMD_DST_HI  (*((volatile unsigned char *)WCMD_DST_HI_ADDR))
#define WCMD_LEN_LO  (*((volatile unsigned char *)WCMD_LEN_LO_ADDR))
#define WCMD_LEN_HI  (*((volatile unsigned char *)WCMD_LEN_HI_ADDR))

#define LAUNCH_SEQ    (*((volatile unsigned char *)LAUNCH_SEQ_ADDR))
#define LAUNCH_TGT_LO (*((volatile unsigned char *)LAUNCH_TGT_LO_ADDR))
#define LAUNCH_TGT_HI (*((volatile unsigned char *)LAUNCH_TGT_HI_ADDR))

#define RCMD_SEQ     (*((volatile unsigned char *)RCMD_SEQ_ADDR))
#define RCMD_DONE    (*((volatile unsigned char *)RCMD_DONE_ADDR))
#define RCMD_SRC_LO  (*((volatile unsigned char *)RCMD_SRC_LO_ADDR))
#define RCMD_SRC_HI  (*((volatile unsigned char *)RCMD_SRC_HI_ADDR))
#define RCMD_LEN     (*((volatile unsigned char *)RCMD_LEN_ADDR))

static volatile unsigned char wcmd_last_seq;
static volatile unsigned char launch_last_seq;
static volatile unsigned char rcmd_last_seq;

/* Globals (not static): the asm wrapper references _launch_pending and
   _launch_target, so they need public symbols. */
unsigned char launch_pending;
unsigned int  launch_target;

#asm
nmi_irq_wrapper:
    push af
    push bc
    push de
    push hl
    push ix
    push iy

    exx
    ex af, af'
    push af
    push bc
    push de
    push hl

    call _nmi_handler_c

    ; Launch trap: if _launch_pending != 0, replace the original NMI-saved PC
    ; on the Z80 stack with _launch_target so the wrapper exits to the
    ; launch trampoline instead of resuming the main poll loop.  Plain RET is
    ; used (not RETN) so IFF1 stays cleared until the trampoline runs DI/EI
    ; explicitly.
    ld a, (_launch_pending)
    or a
    jp nz, nmi_do_launch

    pop hl
    pop de
    pop bc
    pop af
    ex af, af'
    exx

    pop iy
    pop ix
    pop hl
    pop de
    pop bc
    pop af
    retn

nmi_do_launch:
    xor a
    ld (_launch_pending), a
    ; Drop the 10 wrapper-saved registers (20 bytes) without restoring.
    ld hl, 20
    add hl, sp
    ld sp, hl
    ; SP now points at the original NMI-pushed PC slot.  Replace it.
    pop af                         ; discard original PC
    ld hl, (_launch_target)
    push hl
    ret                            ; pop launch_target into PC, IFF1 stays 0
#endasm

static void wcmd_poll(void) {
    unsigned char seq = WCMD_SEQ;

    if (seq != wcmd_last_seq) {
        unsigned int dst = (unsigned int)WCMD_DST_LO | ((unsigned int)WCMD_DST_HI << 8);
        unsigned int len = (unsigned int)WCMD_LEN_LO | ((unsigned int)WCMD_LEN_HI << 8);

        if (len > WCMD_MAX_DATA) {
            len = WCMD_MAX_DATA;
        }

        memcpy((void *)dst, (void *)WCMD_DATA_ADDR, len);

        WCMD_DONE = seq;
        wcmd_last_seq = seq;
    }
}

static void launch_poll(void) {
    unsigned char seq = LAUNCH_SEQ;

    if (seq != launch_last_seq) {
        launch_target  = (unsigned int)LAUNCH_TGT_LO
                       | ((unsigned int)LAUNCH_TGT_HI << 8);
        launch_last_seq = seq;
        launch_pending  = 1u;
    }
}

static void rcmd_poll(void) {
    unsigned char seq = RCMD_SEQ;

    if (seq != rcmd_last_seq) {
        unsigned int src = (unsigned int)RCMD_SRC_LO | ((unsigned int)RCMD_SRC_HI << 8);
        unsigned char len = RCMD_LEN;

        if (len > RCMD_MAX_LEN) {
            len = RCMD_MAX_LEN;
        }

        memcpy((void *)RCMD_BUF_ADDR, (void *)src, len);

        RCMD_DONE = seq;
        rcmd_last_seq = seq;
    }
}

void nmi_handler_c(void) {
    wcmd_poll();
    launch_poll();
    rcmd_poll();
}

void main(void) {
    wcmd_last_seq   = WCMD_SEQ;
    launch_last_seq = LAUNCH_SEQ;
    rcmd_last_seq   = RCMD_SEQ;
    launch_pending  = 0u;
    launch_target   = 0u;

    while (1) {
        wcmd_poll();
        launch_poll();
        rcmd_poll();

        if (launch_pending != 0u) {
            /* Launch was requested between NMIs (main-loop path).
               Jump straight to the trampoline; IFF1 is already 0. */
            __asm
                di
                xor a
                ld (_launch_pending), a
                ld hl, (_launch_target)
                jp (hl)
            __endasm;
        }
    }
}
