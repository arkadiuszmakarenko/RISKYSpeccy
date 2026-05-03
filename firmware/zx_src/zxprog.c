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
 *   YELLOW (6)  startup complete, main poll loop running
 *   <game> (*)  first WCMD received — game data loading, border from regblock[14]
 *   GREEN  (4)  trigger 0x55 detected — game launch starting
 *   WHITE  (7)  launcher entered, alive byte written
 *   <snap> (*)  snapshot's own border colour restored from regblock
 *
 * Cart RAM mailbox layout (shared with CH32 MPU):
 *   0x302E..0x302F  WCMD_SEQ / WCMD_DONE
 *   0x3030..0x3033  WCMD_DST_LO/HI, WCMD_LEN_LO/HI
 *   0x3034..0x3233  WCMD_DATA (512 bytes max)
 *   0x3028..0x3029  KEY_SEQ / KEY_CODE (ZX->MPU key events)
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

/* ZX->MPU key mailbox */
#define KEY_SEQ_ADDR     0x3028u
#define KEY_CODE_ADDR    0x3029u

/* ZX RAM region to clear at startup (48K: 0x4000..0xFFFF) */
#define ZX_RAM_BASE_ADDR 0x4000u
#define ZX_RAM_SIZE      0xC000u

/* RCMD mailbox */
#define RCMD_SEQ_ADDR    0x3F20u
#define RCMD_DONE_ADDR   0x3F21u
#define RCMD_SRC_LO_ADDR 0x3F22u
#define RCMD_SRC_HI_ADDR 0x3F23u
#define RCMD_LEN_ADDR    0x3F24u
#define RCMD_BUF_ADDR    0x3F40u
#define RCMD_MAX_LEN     64u

#define WCMD_SEQ     (*((volatile unsigned char *)WCMD_SEQ_ADDR))
#define WCMD_DONE    (*((volatile unsigned char *)WCMD_DONE_ADDR))
#define WCMD_DST_LO  (*((volatile unsigned char *)WCMD_DST_LO_ADDR))
#define WCMD_DST_HI  (*((volatile unsigned char *)WCMD_DST_HI_ADDR))
#define WCMD_LEN_LO  (*((volatile unsigned char *)WCMD_LEN_LO_ADDR))
#define WCMD_LEN_HI  (*((volatile unsigned char *)WCMD_LEN_HI_ADDR))

#define KEY_SEQ      (*((volatile unsigned char *)KEY_SEQ_ADDR))
#define KEY_CODE     (*((volatile unsigned char *)KEY_CODE_ADDR))

#define RCMD_SEQ     (*((volatile unsigned char *)RCMD_SEQ_ADDR))
#define RCMD_DONE    (*((volatile unsigned char *)RCMD_DONE_ADDR))
#define RCMD_SRC_LO  (*((volatile unsigned char *)RCMD_SRC_LO_ADDR))
#define RCMD_SRC_HI  (*((volatile unsigned char *)RCMD_SRC_HI_ADDR))
#define RCMD_LEN     (*((volatile unsigned char *)RCMD_LEN_ADDR))

/* BSS section — placed at 0x3F00 by --data-loc 0x3F00. */
static volatile unsigned char kbd_prev0;
static volatile unsigned char kbd_prev1;
static volatile unsigned char kbd_prev2;
static volatile unsigned char kbd_prev3;
static volatile unsigned char kbd_prev4;
static volatile unsigned char kbd_prev5;
static volatile unsigned char kbd_prev6;
static volatile unsigned char kbd_prev7;

/* Startup, NMI wrapper are in crt0.s */

/* ---- byte-copy helper (avoids stdlib dependency) ---- */
static void zcopy(unsigned char *dst, const unsigned char *src, unsigned int len)
{
    while (len--) {
        *dst++ = *src++;
    }
}

/* ---- byte-fill helper ---- */
static void zfill(unsigned char *dst, unsigned char value, unsigned int len)
{
    while (len--) {
        *dst++ = value;
    }
}

/* ---- keyboard scan (ZX matrix via port FE) ---- */
static unsigned char kbd_row0_read(void) __naked
{
__asm
    ld bc, #0xFEFE
    in a, (c)
    ld l, a
    ret
__endasm;
}

static unsigned char kbd_row1_read(void) __naked
{
__asm
    ld bc, #0xFDFE
    in a, (c)
    ld l, a
    ret
__endasm;
}

static unsigned char kbd_row2_read(void) __naked
{
__asm
    ld bc, #0xFBFE
    in a, (c)
    ld l, a
    ret
__endasm;
}

static unsigned char kbd_row3_read(void) __naked
{
__asm
    ld bc, #0xF7FE
    in a, (c)
    ld l, a
    ret
__endasm;
}

static unsigned char kbd_row4_read(void) __naked
{
__asm
    ld bc, #0xEFFE
    in a, (c)
    ld l, a
    ret
__endasm;
}

static unsigned char kbd_row5_read(void) __naked
{
__asm
    ld bc, #0xDFFE
    in a, (c)
    ld l, a
    ret
__endasm;
}

static unsigned char kbd_row6_read(void) __naked
{
__asm
    ld bc, #0xBFFE
    in a, (c)
    ld l, a
    ret
__endasm;
}

static unsigned char kbd_row7_read(void) __naked
{
__asm
    ld bc, #0x7FFE
    in a, (c)
    ld l, a
    ret
__endasm;
}

static void kbd_publish(unsigned char code)
{
    unsigned char seq;

    if (code == 0u) {
        return;
    }

    KEY_CODE = code;
    seq = (unsigned char)(KEY_SEQ + 1u);
    if (seq == 0u) {
        seq = 1u;
    }
    KEY_SEQ = seq;
}

static unsigned char kbd_decode_press(unsigned char row, unsigned char bit, unsigned char shift_down)
{
    switch (row) {
    case 0u:
        switch (bit) {
        case 1u: return shift_down ? 'Z' : 'z';
        case 2u: return shift_down ? 'X' : 'x';
        case 3u: return shift_down ? 'C' : 'c';
        case 4u: return shift_down ? 'V' : 'v';
        default: return 0u;
        }
    case 1u:
        switch (bit) {
        case 0u: return shift_down ? 'A' : 'a';
        case 1u: return shift_down ? 'S' : 's';
        case 2u: return shift_down ? 'D' : 'd';
        case 3u: return shift_down ? 'F' : 'f';
        case 4u: return shift_down ? 'G' : 'g';
        default: return 0u;
        }
    case 2u:
        switch (bit) {
        case 0u: return shift_down ? 'Q' : 'q';
        case 1u: return shift_down ? 'W' : 'w';
        case 2u: return shift_down ? 'E' : 'e';
        case 3u: return shift_down ? 'R' : 'r';
        case 4u: return shift_down ? 'T' : 't';
        default: return 0u;
        }
    case 3u:
        switch (bit) {
        case 0u: return '1';
        case 1u: return '2';
        case 2u: return '3';
        case 3u: return '4';
        case 4u: return '5';
        default: return 0u;
        }
    case 4u:
        switch (bit) {
        case 0u: return '0';
        case 1u: return '9';
        case 2u: return '8';
        case 3u: return '7';
        case 4u: return '6';
        default: return 0u;
        }
    case 5u:
        switch (bit) {
        case 0u: return shift_down ? 'P' : 'p';
        case 1u: return shift_down ? 'O' : 'o';
        case 2u: return shift_down ? 'I' : 'i';
        case 3u: return shift_down ? 'U' : 'u';
        case 4u: return shift_down ? 'Y' : 'y';
        default: return 0u;
        }
    case 6u:
        switch (bit) {
        case 0u: return '\n';
        case 1u: return shift_down ? 'L' : 'l';
        case 2u: return shift_down ? 'K' : 'k';
        case 3u: return shift_down ? 'J' : 'j';
        case 4u: return shift_down ? 'H' : 'h';
        default: return 0u;
        }
    case 7u:
        switch (bit) {
        case 0u: return ' ';
        case 2u: return shift_down ? 'M' : 'm';
        case 3u: return shift_down ? 'N' : 'n';
        case 4u: return shift_down ? 'B' : 'b';
        default: return 0u;
        }
    default:
        return 0u;
    }
}

static void kbd_poll_publish(void)
{
    unsigned char rows[8];
    unsigned char prev[8];
    unsigned char r;
    unsigned char b;
    unsigned char shift_down;

    rows[0] = kbd_row0_read();
    rows[1] = kbd_row1_read();
    rows[2] = kbd_row2_read();
    rows[3] = kbd_row3_read();
    rows[4] = kbd_row4_read();
    rows[5] = kbd_row5_read();
    rows[6] = kbd_row6_read();
    rows[7] = kbd_row7_read();

    prev[0] = kbd_prev0;
    prev[1] = kbd_prev1;
    prev[2] = kbd_prev2;
    prev[3] = kbd_prev3;
    prev[4] = kbd_prev4;
    prev[5] = kbd_prev5;
    prev[6] = kbd_prev6;
    prev[7] = kbd_prev7;

    kbd_prev0 = rows[0];
    kbd_prev1 = rows[1];
    kbd_prev2 = rows[2];
    kbd_prev3 = rows[3];
    kbd_prev4 = rows[4];
    kbd_prev5 = rows[5];
    kbd_prev6 = rows[6];
    kbd_prev7 = rows[7];

    shift_down = ((rows[0] & 0x01u) == 0u) ? 1u : 0u; /* CAPS SHIFT */

    for (r = 0u; r < 8u; ++r) {
        unsigned char new_presses = (unsigned char)(prev[r] & (unsigned char)~rows[r]);
        for (b = 0u; b < 5u; ++b) {
            if ((new_presses & (unsigned char)(1u << b)) != 0u) {
                kbd_publish(kbd_decode_press(r, b, shift_down));
                return;
            }
        }
    }
}

/* ---- startup screen/RAM clear ---- */
static void zx_startup_clear(void)
{
    /* Clear full 48K Spectrum RAM so every reset starts from a known state. */
    zfill((unsigned char *)ZX_RAM_BASE_ADDR, 0x00u, ZX_RAM_SIZE);

    /* Force blank white paper across the visible screen. */
    zfill((unsigned char *)ATTR_BASE, ATTR(0, WHITE, BLACK), ATTR_SIZE);
}

/* ---- WCMD poll ---- */
static void wcmd_poll(void)
{
    unsigned char seq = WCMD_SEQ;

    /* Use mailbox DONE/SEQ only, so this is robust even if BSS isn't initialised. */
    if (seq != WCMD_DONE) {
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
    }
}

/* ---- RCMD poll ---- */
static void rcmd_poll(void)
{
    unsigned char seq = RCMD_SEQ;

    /* Use mailbox DONE/SEQ only, so this is robust even if BSS isn't initialised. */
    if (seq != RCMD_DONE) {
        unsigned int src = (unsigned int)RCMD_SRC_LO |
                           ((unsigned int)RCMD_SRC_HI << 8);
        unsigned char len = RCMD_LEN;

        if (len > RCMD_MAX_LEN) {
            len = RCMD_MAX_LEN;
        }

        zcopy((unsigned char *)RCMD_BUF_ADDR,
              (const unsigned char *)src, (unsigned int)len);

        RCMD_DONE = seq;
    }
}

/* ---- NMI C handler: called from _nmi_wrapper in crt0.s ---- */
void nmi_handler_c(void)
{
    wcmd_poll();
    rcmd_poll();
    kbd_poll_publish();
}

/* ---- Main poll loop ---- */
/* Called from _startup in crt0.s after DI/SP-init/EI.
   Border is already CYAN (set by startup). */
void main(void)
{
    zx_startup_clear();
    zx_border(YELLOW); /* startup complete, main poll loop running */

    /* _startup jumps to main (JP), it does not CALL main.
       Returning from main would RET to garbage and crash/reset. */
    for (;;) {
        wcmd_poll();
        rcmd_poll();
        kbd_poll_publish();
    }
}
