/* zxprog — cart ROM launcher.
 *
 * Compiled with SDCC -mz80.  No z88dk CRT — custom crt0.s provides:
 *   - Startup at 0x0000 (JP _startup), IM1 handler at 0x0038 (EI; RETI),
 *     NMI dispatch at 0x0066 (JP _nmi_wrapper), and _zx_launcher.
 * Code section starts at 0x0069 (--code-loc 0x0069).
 * BSS placed in cart RAM at --data-loc 0x3F00.
 * Stack at 0x3EFE (set by crt0.s startup).
 *
 * IM1 handler is intentionally minimal (EI; RETI only).  The z88dk handler
 * called timer routines that jumped into already-loaded snapshot game code,
 * causing the launcher to fail with spurious addresses in the trace.
 *
 * Border colour sequence (visible on the Spectrum display):
 *   CYAN   (5)  startup complete, main poll loop running
 *   YELLOW (6)  trigger 0x55 detected — about to call launcher
 *   WHITE  (7)  launcher entered, alive byte written
 *   <snap> (*)  snapshot's own border colour restored from regblock
 *
 * Cart RAM mailbox layout (shared with CH32 MPU):
 *   0x302E..0x302F  WCMD_SEQ / WCMD_DONE
 *   0x3030..0x3033  WCMD_DST_LO/HI, WCMD_LEN_LO/HI
 *   0x3034..0x3233  WCMD_DATA (512 bytes max)
 *   0x3F00..0x3F01  BSS (wcmd_last_seq, rcmd_last_seq) — placed by linker
 *   0x3F10          LAUNCH_TRIGGER  (CH32 writes 0x55)
 *   0x3F20..0x3F24  RCMD meta
 *   0x3F40..0x3F7F  RCMD buffer (64 bytes)
 *   0x3F90..0x3FA9  REGBLOCK (26 bytes, written by CH32)
 *   0x3FAA          LAUNCHER_ALIVE  (launcher writes 0xAA)
 *   0x3FF0..0x3FF5  LAUNCHER_TAIL  [ED, IM_byte, EI/NOP, C3, pc_lo, pc_hi]
 */

#include "zx.h"

/* WCMD mailbox */
#define WCMD_SEQ_ADDR    0x302Eu
#define WCMD_DONE_ADDR   0x302Fu
#define WCMD_DST_LO_ADDR 0x3030u
#define WCMD_DST_HI_ADDR 0x3031u
#define WCMD_LEN_LO_ADDR 0x3032u
#define WCMD_LEN_HI_ADDR 0x3033u
#define WCMD_DATA_ADDR   0x3034u
#define WCMD_MAX_DATA    0x0200u    /* 512 bytes max per WCMD chunk */

/* RCMD mailbox */
#define RCMD_SEQ_ADDR    0x3F20u
#define RCMD_DONE_ADDR   0x3F21u
#define RCMD_SRC_LO_ADDR 0x3F22u
#define RCMD_SRC_HI_ADDR 0x3F23u
#define RCMD_LEN_ADDR    0x3F24u
#define RCMD_BUF_ADDR    0x3F40u
#define RCMD_MAX_LEN     64u

/* Launch trigger: CH32 writes LAUNCH_TRIGGER_GO here */
#define LAUNCH_TRIGGER_ADDR  0x3F10u
#define LAUNCH_TRIGGER_GO    0x55u

#define WCMD_SEQ     (*((volatile unsigned char *)WCMD_SEQ_ADDR))
#define WCMD_DONE    (*((volatile unsigned char *)WCMD_DONE_ADDR))
#define WCMD_DST_LO  (*((volatile unsigned char *)WCMD_DST_LO_ADDR))
#define WCMD_DST_HI  (*((volatile unsigned char *)WCMD_DST_HI_ADDR))
#define WCMD_LEN_LO  (*((volatile unsigned char *)WCMD_LEN_LO_ADDR))
#define WCMD_LEN_HI  (*((volatile unsigned char *)WCMD_LEN_HI_ADDR))

#define RCMD_SEQ     (*((volatile unsigned char *)RCMD_SEQ_ADDR))
#define RCMD_DONE    (*((volatile unsigned char *)RCMD_DONE_ADDR))
#define RCMD_SRC_LO  (*((volatile unsigned char *)RCMD_SRC_LO_ADDR))
#define RCMD_SRC_HI  (*((volatile unsigned char *)RCMD_SRC_HI_ADDR))
#define RCMD_LEN     (*((volatile unsigned char *)RCMD_LEN_ADDR))

#define LAUNCH_TRIGGER  (*((volatile unsigned char *)LAUNCH_TRIGGER_ADDR))

/* BSS section — placed at 0x3F00 by --data-loc 0x3F00.
   main() re-syncs these from cart RAM before the poll loop,
   so zero-initialisation by the CRT is not required. */
static volatile unsigned char wcmd_last_seq;   /* 0x3F00 */
static volatile unsigned char rcmd_last_seq;   /* 0x3F01 */

/* Startup, NMI wrapper, and zx_launcher are in crt0.s */
extern void zx_launcher(void);

/* ---- byte-copy helper (avoids stdlib dependency) ---- */
static void zcopy(unsigned char *dst, const unsigned char *src, unsigned int len)
{
    while (len--) {
        *dst++ = *src++;
    }
}

/* ---- WCMD poll ---- */
static void wcmd_poll(void)
{
    unsigned char seq = WCMD_SEQ;

    if (seq != wcmd_last_seq) {
        unsigned int dst = (unsigned int)WCMD_DST_LO |
                           ((unsigned int)WCMD_DST_HI << 8);
        unsigned int len = (unsigned int)WCMD_LEN_LO |
                           ((unsigned int)WCMD_LEN_HI << 8);

        if (len > WCMD_MAX_DATA) {
            len = WCMD_MAX_DATA;
        }

        zcopy((unsigned char *)dst,
              (const unsigned char *)WCMD_DATA_ADDR, len);

        WCMD_DONE = seq;
        wcmd_last_seq = seq;
    }
}

/* ---- RCMD poll ---- */
static void rcmd_poll(void)
{
    unsigned char seq = RCMD_SEQ;

    if (seq != rcmd_last_seq) {
        unsigned int src = (unsigned int)RCMD_SRC_LO |
                           ((unsigned int)RCMD_SRC_HI << 8);
        unsigned char len = RCMD_LEN;

        if (len > RCMD_MAX_LEN) {
            len = RCMD_MAX_LEN;
        }

        zcopy((unsigned char *)RCMD_BUF_ADDR,
              (const unsigned char *)src, (unsigned int)len);

        RCMD_DONE = seq;
        rcmd_last_seq = seq;
    }
}

/* ---- NMI C handler: called from _nmi_wrapper in crt0.s ---- */
void nmi_handler_c(void)
{
    wcmd_poll();
    rcmd_poll();
}

/* ---- Main poll loop ---- */
/* Called from _startup in crt0.s after DI/SP-init/EI.
   Border is already CYAN (set by startup). */
void main(void)
{
    /* Sync sequence numbers to current cart RAM state so we don't
       re-process commands that were queued before this boot. */
    wcmd_last_seq = WCMD_SEQ;
    rcmd_last_seq = RCMD_SEQ;

    for (;;) {
        wcmd_poll();
        rcmd_poll();

        if (LAUNCH_TRIGGER == LAUNCH_TRIGGER_GO) {
            /* Border: YELLOW = trigger received, about to launch */
            ULA_PORT = YELLOW;
            /* _zx_launcher (in crt0.s) never returns:
               it sets border WHITE, writes alive byte, restores all Z80
               registers from regblock at 0x3F90, then JPs to tail at 0x3FF0. */
            zx_launcher();
        }
    }
}
