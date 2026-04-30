/*
 *  .z80 v1 snapshot loader.
 *
 *  Strategy: stream the 49152-byte body from FATFS into ZX RAM (transport is
 *  either NMI-mailbox or direct BUSREQ).  Then place a small register-restore
 *  trampoline in upper ZX RAM that:
 *
 *      1) waits for a go-byte (BUSREQ-written by the host)
 *      2) restores AF/BC/DE/HL, AF'/BC'/DE'/HL', IX, IY, I, R, border, IM, IFF
 *      3) sets SP to user_sp-2 (PC pre-pushed by the loader)
 *      4) JP 0xFFFE  -> a single 0xC9 (RET) byte in upper ZX RAM
 *
 *  The cart ISR is armed to detect an opcode (M1) fetch at 0xFFFE and atomically
 *  drop ROMCS; the RET then pops user PC from the snapshot stack and Z80
 *  resumes with the internal Spectrum ROM live at 0x0000-0x3FFF.
 *
 *  Limitations: v1 only (PC must be non-zero in header).  v2/v3, 128K paging,
 *  Issue-2 emulation, Multiface state and AY-3-8910 register dump are ignored.
 */

#include "z80_loader.h"

#include "debug.h"
#include "ff.h"
#include "zx_bus.h"

#include <string.h>

/* ===== ZX-side memory map for the loader ============================ */

#define Z80L_REGBLOCK_ADDR     0x3F80u   /* cart RAM: 26 bytes of state   */
#define Z80L_REGBLOCK_LEN      26u
#define Z80L_TRAMP_ADDR        0x3FA0u   /* cart RAM: trampoline code     */
#define Z80L_TRAMP_LEN         65u
#define Z80L_GO_ADDR           0x3FDDu
#define Z80L_GO_VALUE          0x55u

/* Diagnostic markers in cart RAM (sp->ram[]).  Z80 writes them via the
   normal ISR WR-capture path; even if a few WR strobes are missed, by
   the time the trampoline reaches the spinloop the markers settle.
   Crucially: cart RAM lives in MCU SRAM, so the MCU can read these
   markers via sp->ram[] at any time \u2014 BEFORE handover, AFTER handover,
   even seconds later \u2014 with no BUSREQ stall and no chance of the
   running game stomping them (these addresses are outside the 48K map). */
#define Z80L_DBG_ALIVE_ADDR    0x3F7Eu
#define Z80L_DBG_POSTGO_ADDR   0x3F7Fu
#define Z80L_DBG_ALIVE         0xAAu
#define Z80L_DBG_POSTGO        0xBBu

/* zxprog BSS+stack lives at 0xBE00-0xBEFF (minimal NMI-only zxprog).
   NMI-writes into that region would corrupt the running Z80 program;
   defer those bytes and BUSREQ-flush them after the trampoline takes over.
   Note: 0xBE00+ is in the uncontended upper 16K bank, so BUSREQ writes
   here land reliably (unlike contended 0x4000-0x7FFF). */
#define Z80L_GUARD_LO          0xBE00u
#define Z80L_GUARD_HI          0xBF00u
#define Z80L_GUARD_SIZE        (Z80L_GUARD_HI - Z80L_GUARD_LO)

#define Z80L_NMI_TIMEOUT_MS    400u

#define Z80L_BODY_BASE         0x4000u
#define Z80L_BODY_LEN          49152u

/* Header bytes used by the .z80 v1 spec (offsets from start of file). */
typedef struct {
    uint8_t  a, f;
    uint16_t bc, hl, pc, sp;
    uint8_t  i, r;
    uint8_t  flags1;          /* bit0 = R bit7, bit1..3 = border, bit5 = compressed */
    uint16_t de;
    uint16_t bc_alt, de_alt, hl_alt;
    uint8_t  a_alt, f_alt;
    uint16_t iy, ix;
    uint8_t  iff1;
    uint8_t  iff2;
    uint8_t  flags2;          /* bit0..1 = IM mode */
    int      compressed;
    uint8_t  border;
    uint8_t  im;
} Z80Header;

static int z80_parse_header (const uint8_t *h, Z80Header *out) {
    out->a       = h[0];
    out->f       = h[1];
    out->bc      = (uint16_t)h[2] | ((uint16_t)h[3] << 8);
    out->hl      = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
    out->pc      = (uint16_t)h[6] | ((uint16_t)h[7] << 8);
    out->sp      = (uint16_t)h[8] | ((uint16_t)h[9] << 8);
    out->i       = h[10];
    out->r       = (uint8_t)((h[11] & 0x7Fu) | ((h[12] & 0x01u) << 7));
    out->flags1  = (h[12] == 0xFFu) ? 0x01u : h[12];
    out->de      = (uint16_t)h[13] | ((uint16_t)h[14] << 8);
    out->bc_alt  = (uint16_t)h[15] | ((uint16_t)h[16] << 8);
    out->de_alt  = (uint16_t)h[17] | ((uint16_t)h[18] << 8);
    out->hl_alt  = (uint16_t)h[19] | ((uint16_t)h[20] << 8);
    out->a_alt   = h[21];
    out->f_alt   = h[22];
    out->iy      = (uint16_t)h[23] | ((uint16_t)h[24] << 8);
    out->ix      = (uint16_t)h[25] | ((uint16_t)h[26] << 8);
    out->iff1    = h[27];
    out->iff2    = h[28];
    out->flags2  = h[29];
    out->compressed = (out->flags1 & 0x20u) ? 1 : 0;
    out->border  = (uint8_t)((out->flags1 >> 1) & 0x07u);
    out->im      = (uint8_t)(out->flags2 & 0x03u);
    return (out->pc != 0u) ? 1 : 0;   /* pc==0 means v2/v3 extended header */
}

/* ===== Tiny 8x8 hex font + on-screen debug display =================
   Used to render key memory bytes on the ZX screen for a few seconds
   right before the trampoline handover, so the user can visually verify
   what's actually staged in RAM (regblock, trampoline, RET byte, PC
   pre-push) without needing the serial console.  Drawn into screen RAM
   via NMI writes (zxprog still alive at that point), targeting one
   character row at the bottom of the screen.  */

static const uint8_t Z80L_HEXFONT[16][8] = {
    /* '0' */ {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00},
    /* '1' */ {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00},
    /* '2' */ {0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00},
    /* '3' */ {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00},
    /* '4' */ {0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00},
    /* '5' */ {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00},
    /* '6' */ {0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00},
    /* '7' */ {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00},
    /* '8' */ {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00},
    /* '9' */ {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00},
    /* 'A' */ {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00},
    /* 'B' */ {0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00},
    /* 'C' */ {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00},
    /* 'D' */ {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00},
    /* 'E' */ {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00},
    /* 'F' */ {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00},
};

/* Render up to 16 bytes as 32 hex digits (one full character row, 32
   columns) at character row `row` (0..23, 0=top).  Returns 1 on success.
   Uses 256 bytes of stack for the pixel buffer.  */
static int z80_dbg_show_hex_row (uint8_t row, const uint8_t *bytes, uint8_t n) {
    uint8_t pix[256];           /* 8 scanlines x 32 cols */
    uint8_t attr[32];
    uint8_t bank, cir, i, s;
    uint16_t base;

    if (n > 16u) { n = 16u; }
    memset (pix, 0, sizeof (pix));

    for (i = 0u; i < n; ++i) {
        uint8_t hi = (uint8_t)((bytes[i] >> 4) & 0x0Fu);
        uint8_t lo = (uint8_t)(bytes[i] & 0x0Fu);
        uint8_t col_hi = (uint8_t)(i * 2u);
        uint8_t col_lo = (uint8_t)(col_hi + 1u);
        for (s = 0u; s < 8u; ++s) {
            pix[(uint16_t)s * 32u + col_hi] = Z80L_HEXFONT[hi][s];
            pix[(uint16_t)s * 32u + col_lo] = Z80L_HEXFONT[lo][s];
        }
    }

    /* ZX pixel layout: char row R uses bank=R/8, char-in-bank=R%8.
       Top scanline of char row = 0x4000 + bank*0x800 + cir*0x20.
       Each subsequent scanline within the char is +0x100. */
    bank = (uint8_t)(row >> 3);
    cir  = (uint8_t)(row & 0x07u);
    base = (uint16_t)(0x4000u + (uint16_t)bank * 0x800u
                              + (uint16_t)cir  * 0x20u);
    for (s = 0u; s < 8u; ++s) {
        uint16_t addr = (uint16_t)(base + (uint16_t)s * 0x100u);
        if (!ZX_NmiWriteBlock (addr, &pix[(uint16_t)s * 32u],
                               32u, Z80L_NMI_TIMEOUT_MS)) {
            return 0;
        }
    }

    /* Bright white ink (7) on black paper (0), bright bit set: 0x47. */
    memset (attr, 0x47, sizeof (attr));
    if (!ZX_NmiWriteBlock ((uint16_t)(0x5800u + (uint16_t)row * 32u),
                           attr, sizeof (attr), Z80L_NMI_TIMEOUT_MS)) {
        return 0;
    }
    return 1;
}

/* ===== Buffered stream reader ==================================== */

#define Z80L_INBUF_SIZE 512

typedef struct {
    FIL     *fp;
    uint8_t  buf[Z80L_INBUF_SIZE];
    UINT     pos;
    UINT     len;
    int      err;
} Z80Reader;

static void z80_reader_init (Z80Reader *r, FIL *fp) {
    r->fp = fp;
    r->pos = 0u;
    r->len = 0u;
    r->err = 0;
}

static int z80_reader_get (Z80Reader *r, uint8_t *out) {
    if (r->pos >= r->len) {
        UINT br = 0u;
        FRESULT fr = f_read (r->fp, r->buf, Z80L_INBUF_SIZE, &br);
        if ((fr != FR_OK) || (br == 0u)) {
            r->err = 1;
            return 0;
        }
        r->len = br;
        r->pos = 0u;
    }
    *out = r->buf[r->pos++];
    return 1;
}

/* ===== Body output (chunked write to ZX RAM) ====================== */

#define Z80L_OUT_CHUNK 256u

typedef struct {
    uint16_t base;            /* current ZX target address                */
    uint16_t out_pos;         /* bytes accumulated in out_buf             */
    uint8_t  out_buf[Z80L_OUT_CHUNK];
    uint8_t  guard_buf[Z80L_GUARD_SIZE];
    uint16_t guard_lo;
    uint16_t guard_hi;
    int      transport;
    int      protect_top;     /* drop writes to 0xFE80..0xFFFF (tramp live) */
    uint32_t total;
} Z80OutState;

static void z80_out_init (Z80OutState *o, int transport, int protect_top) {
    o->base = Z80L_BODY_BASE;
    o->out_pos = 0u;
    o->guard_lo = Z80L_GUARD_HI;
    o->guard_hi = Z80L_GUARD_LO;
    o->transport = transport;
    o->protect_top = protect_top;
    o->total = 0u;
}

/* Write [addr..addr+len) to ZX RAM via the active transport.
   For NMI: route any 0x8800-0x8AFF bytes into guard_buf (zxprog BSS).
   For BUSREQ tramp-first: drop writes that hit 0xFE80..0xFFFF (live tramp). */
static int z80_out_write_chunk (Z80OutState *o, uint16_t addr,
                                const uint8_t *src, uint16_t len) {
    if (o->transport == Z80L_VIA_BUSREQ) {
        if (o->protect_top) {
            uint32_t end = (uint32_t)addr + len;
            if (addr >= 0xFE80u) { return 1; }            /* fully in protected window */
            if (end > 0xFE80u) { len = (uint16_t)(0xFE80u - addr); }
        }
        return ZX_BusWriteBlock (addr, src, len);
    }
    /* NMI transport with guard region routing. */
    uint32_t end = (uint32_t)addr + len;

    if ((end <= Z80L_GUARD_LO) || (addr >= Z80L_GUARD_HI)) {
        return ZX_NmiWriteBlock (addr, src, len, Z80L_NMI_TIMEOUT_MS);
    }

    if (addr < Z80L_GUARD_LO) {
        uint16_t pre = (uint16_t)(Z80L_GUARD_LO - addr);
        if (!ZX_NmiWriteBlock (addr, src, pre, Z80L_NMI_TIMEOUT_MS)) { return 0; }
        addr = Z80L_GUARD_LO;
        src += pre;
        len = (uint16_t)(len - pre);
        end = (uint32_t)addr + len;
    }

    {
        uint16_t in_guard = (end > Z80L_GUARD_HI)
                                ? (uint16_t)(Z80L_GUARD_HI - addr)
                                : len;
        uint16_t off = (uint16_t)(addr - Z80L_GUARD_LO);
        memcpy (&o->guard_buf[off], src, in_guard);
        if (addr < o->guard_lo) { o->guard_lo = addr; }
        if ((uint32_t)addr + in_guard > o->guard_hi) {
            o->guard_hi = (uint16_t)((uint32_t)addr + in_guard);
        }
        addr = (uint16_t)(addr + in_guard);
        src += in_guard;
        len = (uint16_t)(len - in_guard);
    }

    if (len > 0u) {
        if (!ZX_NmiWriteBlock (addr, src, len, Z80L_NMI_TIMEOUT_MS)) { return 0; }
    }
    return 1;
}

/* Flush the accumulated chunk, then reset for the next contiguous run. */
static int z80_out_flush (Z80OutState *o) {
    if (o->out_pos == 0u) { return 1; }
    if (!z80_out_write_chunk (o, o->base, o->out_buf, o->out_pos)) {
        printf ("z80: write failed @0x%04X (transport=%d)\r\n",
                (unsigned)o->base, o->transport);
        return 0;
    }
    o->base = (uint16_t)(o->base + o->out_pos);
    o->out_pos = 0u;
    return 1;
}

static int z80_out_emit (Z80OutState *o, uint8_t b) {
    o->out_buf[o->out_pos++] = b;
    o->total++;
    if (o->out_pos == Z80L_OUT_CHUNK) {
        return z80_out_flush (o);
    }
    return 1;
}

/* ===== Body decompression / streaming ============================ */

/* v1 compressed format: bytes of the original 49152-byte body, except runs
   of N copies of value V are encoded as `ED ED N V` (when N > 4 or any
   sequence containing ED ED in the original).  A literal `ED` is emitted
   as-is unless followed by another `ED` (then it's a run header).
   End marker for a compressed body is `00 ED ED 00`. */
static int z80_stream_body (Z80Reader *r, Z80OutState *o, int compressed) {
    if (!compressed) {
        uint32_t i;
        for (i = 0u; i < Z80L_BODY_LEN; ++i) {
            uint8_t b;
            if (!z80_reader_get (r, &b)) { return 0; }
            if (!z80_out_emit (o, b)) { return 0; }
        }
        return z80_out_flush (o);
    }

    while (o->total < Z80L_BODY_LEN) {
        uint8_t b;
        if (!z80_reader_get (r, &b)) { return 0; }
        if (b != 0xEDu) {
            if (!z80_out_emit (o, b)) { return 0; }
            continue;
        }
        /* saw first ED \u2014 peek next */
        uint8_t b2;
        if (!z80_reader_get (r, &b2)) { return 0; }
        if (b2 != 0xEDu) {
            /* literal ED followed by something else \u2014 emit both */
            if (!z80_out_emit (o, 0xEDu)) { return 0; }
            if (!z80_out_emit (o, b2)) { return 0; }
            continue;
        }
        /* ED ED \u2014 run: next two bytes are count and value */
        uint8_t cnt, val;
        if (!z80_reader_get (r, &cnt)) { return 0; }
        if (!z80_reader_get (r, &val)) { return 0; }
        /* End marker `00 ED ED 00` is encoded as cnt=0 and the prior byte
           pair was `00 ED`, but here we just see `ED ED 00 00` which is a
           zero-length run \u2014 treat as no-op. */
        while (cnt-- > 0u) {
            if (!z80_out_emit (o, val)) { return 0; }
        }
    }
    return z80_out_flush (o);
}

/* ===== Trampoline =================================================== */

/* Fixed assembled bytes; offsets [36], [50], [59], [61], [62] are patched
   per-snapshot.  Layout matches the regblock in cart RAM at 0x3F80.

   Restore order — KEY POINT: main AF is restored LAST.  All earlier steps
   clobber A (LD A,<border>; LD A,(I); LD A,(R)), so the snapshot would
   otherwise enter user code with A holding the value of R, not h.a.

     17  LD SP,0x3F82      ; skip main AF, start at BC
     20  POP BC / DE / HL
     23  EXX
     24  POP BC' / DE' / HL'
     27  EXX
     28  EX AF,AF' ; POP AF (alt) ; EX AF,AF'
     31  POP IX / POP IY                  (SP now at 0x3F94)
     35  LD A,<border>; OUT (0xFE),A
     39  LD A,(0x3F94); LD I,A
     44  LD A,(0x3F95); LD R,A
     49  IM x
     51  LD SP,0x3F80                     ; rewind to main AF
     54  POP AF                           ; A and F finally correct
     55  LD SP,(0x3F9A)                   ; user SP
     59  EI / NOP
     60  JP <user_pc>
*/
/* Compact 47-byte trampoline (Robee Shepherd's microdrive snapshot loader
   tricks: POP AF for I, packed border+R via POP BC, alt set via EXX,
   main AF popped LAST so A enters user code uncorrupted).

   READ-ONLY on cart RAM: spins on GO read until MCU pokes 0x55 into
   sp->ram[GO_ADDR] (direct MCU memory write, no bus).  The trampoline
   never writes to cart-RAM, eliminating the flaky Z80→cart-RAM WR path.

   Regblock layout at 0x3F80 (popped low->high):
       +0  BC' / DE' / HL'    (popped to main, then EXX moves to alt)
       +6  AF'                (POP AF; EX AF,AF')
       +8  IX / IY            (not banked)
       +12 I-pair              (POP AF: lo=junk F, hi=I)
       +14 border + R          (POP BC: lo=C=border, hi=B=R-9 mod 128)
       +16 main BC / DE / HL
       +22 main AF             (popped LAST)
       +24 user SP             (loaded via LD SP,(0x3F98))

   Patch sites: [35] IM byte (46/56/5E), [51] EI/NOP placeholder (the JP
   below at offsets 51..53), see actual indices in the patch code.

   Diagnostic markers (0x5B00..0x5B01 \u2014 ZX printer buffer, never used by
   real games):
       0x5B00 = 0xAA   immediately after DI  (\"trampoline started\")
       0x5B01 = 0xBB   right after spinloop  (\"GO seen, falling through\")
   Lets the MCU distinguish: trampoline never ran / stuck in spinloop /
   ran fully but M1 didn't latch. */
static const uint8_t Z80L_TRAMPOLINE[Z80L_TRAMP_LEN] = {
    /*  0 */ 0xF3,                       /* DI                              */
    /*  1 */ 0x3E, 0xAA,                 /* LD A, 0xAA                      */
    /*  3 */ 0x06, 0x20,                 /* LD B, 32                        */
    /*  5 */ 0x32, 0x7E, 0x3F,           /* LD (0x3F7E), A   ; alive (32x) */
    /*  8 */ 0x10, 0xFB,                 /* DJNZ -5  -> back to LD (nn),A   */
    /* 10 */ 0x3A, 0xDD, 0x3F,           /* LD A, (0x3FDD)  ; GO byte       */
    /* 13 */ 0xFE, 0x55,                 /* CP 0x55                         */
    /* 15 */ 0x20, 0xF9,                 /* JR NZ, -7  (back to LD A,(GO))  */
    /* 17 */ 0x3E, 0xBB,                 /* LD A, 0xBB                      */
    /* 19 */ 0x06, 0x20,                 /* LD B, 32                        */
    /* 21 */ 0x32, 0x7F, 0x3F,           /* LD (0x3F7F), A   ; post (32x)  */
    /* 24 */ 0x10, 0xFB,                 /* DJNZ -5                         */
    /* 26 */ 0x31, 0x80, 0x3F,           /* LD SP, 0x3F80  ; regblock       */
    /* 29 */ 0xC1,                       /* POP BC          ; BC' value     */
    /* 30 */ 0xD1,                       /* POP DE          ; DE' value     */
    /* 31 */ 0xE1,                       /* POP HL          ; HL' value     */
    /* 32 */ 0xD9,                       /* EXX  -> values now in alt set   */
    /* 33 */ 0xF1,                       /* POP AF          ; AF' value     */
    /* 34 */ 0x08,                       /* EX AF, AF'                      */
    /* 35 */ 0xDD, 0xE1,                 /* POP IX                          */
    /* 37 */ 0xFD, 0xE1,                 /* POP IY                          */
    /* 39 */ 0xF1,                       /* POP AF          ; A=I, F=junk   */
    /* 40 */ 0xED, 0x47,                 /* LD I, A                         */
    /* 42 */ 0xED, 0x56,                 /* IM 1            PATCH @43       */
    /* 44 */ 0xC1,                       /* POP BC          ; C=border B=R  */
    /* 45 */ 0x79,                       /* LD A, C                         */
    /* 46 */ 0x0E, 0xFE,                 /* LD C, 0xFE                      */
    /* 48 */ 0xED, 0x79,                 /* OUT (C), A      ; border        */
    /* 50 */ 0x78,                       /* LD A, B                         */
    /* 51 */ 0xED, 0x4F,                 /* LD R, A         ; R = saved-9   */
    /* 53 */ 0xC1,                       /* POP BC          ; main BC       */
    /* 54 */ 0xD1,                       /* POP DE          ; main DE       */
    /* 55 */ 0xE1,                       /* POP HL          ; main HL       */
    /* 56 */ 0xF1,                       /* POP AF          ; main AF LAST  */
    /* 57 */ 0xED, 0x7B, 0x98, 0x3F,     /* LD SP, (0x3F98) ; user SP       */
    /* 61 */ 0xFB,                       /* EI / NOP        PATCH @61       */
    /* 62 */ 0xC3, 0x00, 0x00            /* JP user_pc      PATCH @63/64    */
};

/* Build the 26-byte regblock matching the trampoline above. */
static void z80_build_regblock (const Z80Header *h, uint8_t *rb) {
    /* +0..5: BC', DE', HL'  (popped into main, then EXX moves to alt) */
    rb[ 0] = (uint8_t)(h->bc_alt & 0xFFu); rb[ 1] = (uint8_t)(h->bc_alt >> 8);
    rb[ 2] = (uint8_t)(h->de_alt & 0xFFu); rb[ 3] = (uint8_t)(h->de_alt >> 8);
    rb[ 4] = (uint8_t)(h->hl_alt & 0xFFu); rb[ 5] = (uint8_t)(h->hl_alt >> 8);
    /* +6..7: AF' via POP AF; EX AF,AF'  (lo=F', hi=A') */
    rb[ 6] = h->f_alt;
    rb[ 7] = h->a_alt;
    /* +8..11: IX, IY (not banked, popped after EXX into the same regs) */
    rb[ 8] = (uint8_t)(h->ix & 0xFFu); rb[ 9] = (uint8_t)(h->ix >> 8);
    rb[10] = (uint8_t)(h->iy & 0xFFu); rb[11] = (uint8_t)(h->iy >> 8);
    /* +12..13: I via POP AF (lo=junk F, hi=I) */
    rb[12] = 0u;
    rb[13] = h->i;
    /* +14..15: border + R packed via POP BC (lo=C=border, hi=B=R).
       R is compensated by -9 to account for the M1 cycles between
       LD R,A (offset 42) and the user PC fetch:
         POP BC POP DE POP HL POP AF (4) + LD SP,(nn) ED-prefixed (2)
         + EI (1) + JP (1) + user M1 (1) = 9
       Bit 7 of R is preserved (LD R,A loads it but only bits 0-6 cycle). */
    rb[14] = h->border;
    rb[15] = (uint8_t)(((h->r - 9u) & 0x7Fu) | (h->r & 0x80u));
    /* +16..21: main BC, DE, HL */
    rb[16] = (uint8_t)(h->bc & 0xFFu); rb[17] = (uint8_t)(h->bc >> 8);
    rb[18] = (uint8_t)(h->de & 0xFFu); rb[19] = (uint8_t)(h->de >> 8);
    rb[20] = (uint8_t)(h->hl & 0xFFu); rb[21] = (uint8_t)(h->hl >> 8);
    /* +22..23: main AF (popped LAST so A is correct on entry to user code) */
    rb[22] = h->f;
    rb[23] = h->a;
    /* +24..25: user SP (loaded via LD SP,(0x3F98)) */
    rb[24] = (uint8_t)(h->sp & 0xFFu);
    rb[25] = (uint8_t)(h->sp >> 8);
}

static uint8_t z80_im_byte (uint8_t im) {
    switch (im) {
        case 0u:  return 0x46u;   /* IM 0 */
        case 2u:  return 0x5Eu;   /* IM 2 */
        default:  return 0x56u;   /* IM 1 (also default for unknown) */
    }
}

/* Inline NMI launch (no alive poll).  Replaces ZX_SnapshotEnter for the
   .z80 path: programs the cart-ROM launch mailbox, pre-clears the GO byte
   directly in the cart-RAM model (no Z80 write involved), triggers NMI,
   and waits a fixed settle interval.  The Z80 enters the trampoline within
   well under 1ms (NMI vector + zxprog dispatch ~30 T-states), so 10ms is
   far more than enough for it to be safely sitting in the GO-read spinloop
   before the caller starts BUSREQ traffic. */
static int z80_launch_nmi (void) {
    uint8_t lo = (uint8_t)(Z80L_TRAMP_ADDR & 0xFFu);
    uint8_t hi = (uint8_t)(Z80L_TRAMP_ADDR >> 8);
    uint8_t go_clear = (uint8_t)(Z80L_GO_VALUE ^ 0xFFu);  /* anything != 0x55 */
    uint8_t cur_seq = 0u;
    uint8_t next_seq = 0u;
    uint8_t marker_zero[2] = { 0x00u, 0x00u };

    /* Pre-clear GO byte in cart RAM (direct MCU memory write, reliable). */
    if (!ZX_CartRamWriteBlock (Z80L_GO_ADDR, &go_clear, 1u)) { return 0; }
    /* Pre-clear diagnostic markers so a missed Z80 write shows as 0x00. */
    (void)ZX_CartRamWriteBlock (Z80L_DBG_ALIVE_ADDR, marker_zero, 2u);

    /* Program the zxprog launch mailbox at 0x3F10..0x3F12. */
    if (!ZX_CartRamWriteBlock (0x3F11u, &lo, 1u))      { return 0; }
    if (!ZX_CartRamWriteBlock (0x3F12u, &hi, 1u))      { return 0; }
    if (!ZX_CartRamReadBlock  (0x3F10u, &cur_seq, 1u)) { return 0; }
    next_seq = (uint8_t)(cur_seq + 1u);
    if (!ZX_CartRamWriteBlock (0x3F10u, &next_seq, 1u)) { return 0; }

    /* Trigger NMI and wait for the trampoline alive marker (0x3F7E=0xAA).
       Z80 normally enters the trampoline within ~30us.  If the alive
       marker never appears (NMI race, or zxprog already running game
       code, etc.) retry the NMI a few times before giving up. */
    {
        unsigned attempt;
        for (attempt = 0u; attempt < 5u; ++attempt) {
            uint32_t waited;
            ZX_TriggerNMI ();
            for (waited = 0u; waited < 50u; ++waited) {
                uint8_t alive = 0u;
                Delay_Ms (1u);
                if (ZX_CartRamReadBlock (Z80L_DBG_ALIVE_ADDR, &alive, 1u)
                    && (alive == Z80L_DBG_ALIVE)) {
                    return 1;
                }
            }
            printf ("z80: NMI retry %u (alive marker not seen)\r\n",
                    attempt + 1u);
            /* Re-bump seq in case zxprog missed the previous edge. */
            (void)ZX_CartRamReadBlock  (0x3F10u, &cur_seq, 1u);
            next_seq = (uint8_t)(cur_seq + 1u);
            (void)ZX_CartRamWriteBlock (0x3F10u, &next_seq, 1u);
        }
    }
    return 0;
}

/* ===== Public API ================================================ */

static int z80_open_and_parse (const char *path, FIL *fp, Z80Header *out_hdr) {
    FRESULT fr = f_open (fp, path, FA_READ);
    if (fr != FR_OK) {
        printf ("z80: cannot open '%s' (fr=%d)\r\n", path, (int)fr);
        return Z80L_ERR_OPEN;
    }
    {
        uint8_t hdr[30];
        UINT got = 0u;
        fr = f_read (fp, hdr, 30u, &got);
        if ((fr != FR_OK) || (got != 30u)) {
            printf ("z80: short header (got %u)\r\n", (unsigned)got);
            f_close (fp);
            return Z80L_ERR_READ;
        }
        if (!z80_parse_header (hdr, out_hdr)) {
            printf ("z80: PC=0 in header \u2014 v2/v3 not supported in this build\r\n");
            f_close (fp);
            return Z80L_ERR_VERSION;
        }
    }
    return Z80L_OK;
}

int Z80_Info (const char *path) {
    static FIL fp;
    Z80Header h;
    int rc = z80_open_and_parse (path, &fp, &h);
    if (rc != Z80L_OK) { return rc; }
    f_close (&fp);

    printf ("z80: v1 snapshot %s\r\n", path);
    printf ("  PC=%04X SP=%04X  A=%02X F=%02X BC=%04X DE=%04X HL=%04X\r\n",
            h.pc, h.sp, h.a, h.f, h.bc, h.de, h.hl);
    printf ("  A'=%02X F'=%02X BC'=%04X DE'=%04X HL'=%04X\r\n",
            h.a_alt, h.f_alt, h.bc_alt, h.de_alt, h.hl_alt);
    printf ("  IX=%04X IY=%04X  I=%02X R=%02X  border=%u  IM=%u  IFF1=%u  %s\r\n",
            h.ix, h.iy, h.i, h.r, h.border, h.im, h.iff1,
            h.compressed ? "(compressed)" : "(uncompressed)");
    return Z80L_OK;
}

int Z80_LoadAndRun (const char *path, int transport) {
    static FIL fp;
    static Z80OutState out;
    Z80Header h;
    Z80Reader rd;
    int rc;
    uint8_t regblock[Z80L_REGBLOCK_LEN];
    uint8_t tramp[Z80L_TRAMP_LEN];
    uint16_t handover_addr;

    if ((transport != Z80L_VIA_NMI) && (transport != Z80L_VIA_BUSREQ)) {
        printf ("z80: invalid transport %d\r\n", transport);
        return Z80L_ERR_FORMAT;
    }

    rc = z80_open_and_parse (path, &fp, &h);
    if (rc != Z80L_OK) { return rc; }

    printf ("z80: loading via %s, PC=%04X SP=%04X border=%u IM=%u IFF=%u %s\r\n",
            (transport == Z80L_VIA_BUSREQ) ? "BUSREQ" : "NMI",
            h.pc, h.sp, h.border, h.im, h.iff1,
            h.compressed ? "compressed" : "raw");

    /* Tell zxprog to stop redrawing its bridge/wcmd status overlay so the
       snapshot's screen RAM (0x4000-0x5AFF) isn't repainted during load. */
    ZX_CartDrawSuspend ();

    /* Build trampoline + register block in MCU memory (used in both flows). */
    memcpy (tramp, Z80L_TRAMPOLINE, Z80L_TRAMP_LEN);
    tramp[43] = z80_im_byte (h.im);                /* IM x  (second byte) */
    tramp[61] = (h.iff1 != 0u) ? 0xFBu : 0x00u;    /* EI / NOP            */
    tramp[63] = (uint8_t)(h.pc & 0xFFu);            /* JP user_pc lo       */
    tramp[64] = (uint8_t)(h.pc >> 8);               /* JP user_pc hi       */

    /* If interrupts are enabled at resume and IM1 is active, the first M1
       after EI+JP may be the interrupt vector fetch at 0x0038 (before user
       PC fetch).  Arm handover there so ROMCS still drops deterministically. */
    handover_addr = h.pc;
    if ((h.iff1 != 0u) && (h.im == 1u)) {
        handover_addr = 0x0038u;
    }

    z80_build_regblock (&h, regblock);

    if (transport == Z80L_VIA_BUSREQ) {
        /* TRAMP-FIRST FLOW.  Install the launch workspace in cart RAM, then
           redirect Z80 PC into the cart-RAM trampoline before streaming the
           full 48K snapshot body by BUSREQ. */
        if (!ZX_CartRamWriteBlock (Z80L_REGBLOCK_ADDR, regblock,
                                   Z80L_REGBLOCK_LEN)) {
            printf ("z80: regblock cart install failed\r\n");
            f_close (&fp);
            return Z80L_ERR_LOAD;
        }
        if (!ZX_CartRamWriteBlock (Z80L_TRAMP_ADDR, tramp,
                                   Z80L_TRAMP_LEN)) {
            printf ("z80: trampoline cart install failed\r\n");
            f_close (&fp);
            return Z80L_ERR_LOAD;
        }

        if (!z80_launch_nmi ()) {
            printf ("z80: NMI launch failed (mailbox)\r\n");
            f_close (&fp);
            return Z80L_ERR_LAUNCH;
        }

          /* The launch workspace is outside ZX RAM, so no top-RAM hole needs
              to be protected during the snapshot body stream. */
        z80_reader_init (&rd, &fp);
          z80_out_init (&out, transport, 0);
        if (!z80_stream_body (&rd, &out, h.compressed)) {
            printf ("z80: body stream failed (BUSREQ, total=%lu)\r\n",
                    (unsigned long)out.total);
            f_close (&fp);
            return Z80L_ERR_LOAD;
        }
        f_close (&fp);
        printf ("z80: body staged (%lu bytes via BUSREQ)\r\n",
                (unsigned long)out.total);
    } else {
        /* NMI BODY-FIRST FLOW.  zxprog is the writer, so it stays alive
           through the entire body stream.  Guard 0x8800-0x8AFF (zxprog BSS)
           is buffered and flushed via BUSREQ post-launch (same scheme as
           the .tap loader). */
        z80_reader_init (&rd, &fp);
        z80_out_init (&out, transport, 0);
        if (!z80_stream_body (&rd, &out, h.compressed)) {
            printf ("z80: body stream failed (NMI, total=%lu)\r\n",
                    (unsigned long)out.total);
            f_close (&fp);
            return Z80L_ERR_LOAD;
        }
        f_close (&fp);
        printf ("z80: body staged (%lu bytes via NMI)\r\n",
                (unsigned long)out.total);

        /* Launch workspace lives in cart RAM, so the snapshot body can use
           all ZX RAM and the user's stack stays untouched. */
        if (!ZX_CartRamWriteBlock (Z80L_REGBLOCK_ADDR, regblock,
                                   Z80L_REGBLOCK_LEN)) {
            printf ("z80: regblock cart write failed\r\n");
            return Z80L_ERR_LOAD;
        }
        if (!ZX_CartRamWriteBlock (Z80L_TRAMP_ADDR, tramp,
                                   Z80L_TRAMP_LEN)) {
            printf ("z80: trampoline cart write failed\r\n");
            return Z80L_ERR_LOAD;
        }

        /* Full readback verify: the trampoline image MUST match byte-for-byte
           or we'll execute corrupt opcodes and either hang or jump to garbage
           that may happen to look like running game code with junk regs. */
        {
            uint8_t rb_back[Z80L_REGBLOCK_LEN];
            uint8_t tr_back[Z80L_TRAMP_LEN];
            int ok_rb = ZX_CartRamReadBlock (Z80L_REGBLOCK_ADDR, rb_back,
                                             Z80L_REGBLOCK_LEN);
            int ok_tr = ZX_CartRamReadBlock (Z80L_TRAMP_ADDR, tr_back,
                                             Z80L_TRAMP_LEN);
            int bad = 0;
            unsigned i;
            if (!ok_rb || !ok_tr) {
                printf ("z80: cart readback failed (rb=%d tr=%d)\r\n",
                        ok_rb, ok_tr);
                return Z80L_ERR_LOAD;
            }
            for (i = 0u; i < Z80L_REGBLOCK_LEN; ++i) {
                if (rb_back[i] != regblock[i]) {
                    printf ("z80: regblock mismatch @+%u: got %02X want %02X\r\n",
                            i, rb_back[i], regblock[i]);
                    if (++bad >= 8) break;
                }
            }
            for (i = 0u; i < Z80L_TRAMP_LEN; ++i) {
                if (tr_back[i] != tramp[i]) {
                    printf ("z80: tramp mismatch @+%u: got %02X want %02X\r\n",
                            i, tr_back[i], tramp[i]);
                    if (++bad >= 8) break;
                }
            }
            if (bad) {
                printf ("z80: cart verify FAILED (%d mismatches), aborting\r\n",
                        bad);
                return Z80L_ERR_LOAD;
            }
            printf ("z80: cart verify OK (regblock %u + tramp %u bytes)\r\n",
                    (unsigned)Z80L_REGBLOCK_LEN, (unsigned)Z80L_TRAMP_LEN);
        }

        if (!z80_launch_nmi ()) {
            printf ("z80: NMI launch failed (mailbox)\r\n");
            return Z80L_ERR_LAUNCH;
        }

        {
            uint8_t mk[2] = {0xFFu, 0xFFu};
            (void)ZX_CartRamReadBlock (Z80L_DBG_ALIVE_ADDR, mk, 2u);
            printf ("z80: post-launch markers @3F7E=%02X @3F7F=%02X "
                    "(want AA + 00 \u2014 trampoline alive, in spinloop)\r\n",
                    mk[0], mk[1]);
        }

        /* Flush deferred guard bytes via BUSREQ now that zxprog is gone.
           Guard region is 0xBE00-0xBEFF (uncontended upper RAM), reliable. */
        if (out.guard_lo < out.guard_hi) {
            uint16_t flush_len = (uint16_t)(out.guard_hi - out.guard_lo);
            printf ("z80: flushing %u deferred byte(s) @0x%04X..0x%04X\r\n",
                    (unsigned)flush_len,
                    (unsigned)out.guard_lo,
                    (unsigned)(out.guard_hi - 1u));
            if (!ZX_BusWriteBlock (out.guard_lo,
                                   &out.guard_buf[out.guard_lo - Z80L_GUARD_LO],
                                   flush_len)) {
                printf ("z80: deferred BUSREQ flush failed\r\n");
                return Z80L_ERR_LOAD;
            }
        }
    }

     /* Common tail: arm M1 handover on the real snapshot PC and release the
         trampoline spin.  Launch workspace already lives in cart RAM. */

    if (handover_addr != h.pc) {
        printf ("z80: handover armed at %04X OR %04X (IM1+IFF1)\r\n",
                (unsigned)handover_addr, (unsigned)h.pc);
    }

    /* Pre-launch sanity dump of user-stack and user-PC area (ZX RAM only:
       PC < 0x4000 is in ROM and irrelevant).  Helps spot body-load
       corruption \u2014 if these bytes are obviously wrong, the snapshot
       data didn't reach ZX RAM. */
    {
        uint8_t buf[16];
        unsigned i;
        if (ZX_BusReadBlock (h.sp, buf, 16u)) {
            printf ("z80: stack@%04X:", (unsigned)h.sp);
            for (i = 0u; i < 16u; ++i) printf (" %02X", buf[i]);
            printf ("\r\n");
        }
        if ((h.pc >= 0x4000u) && ZX_BusReadBlock (h.pc, buf, 16u)) {
            printf ("z80: code@%04X:", (unsigned)h.pc);
            for (i = 0u; i < 16u; ++i) printf (" %02X", buf[i]);
            printf ("\r\n");
        }
        printf ("z80: regs A=%02X F=%02X BC=%04X DE=%04X HL=%04X "
                "IX=%04X IY=%04X I=%02X R=%02X\r\n",
                h.a, h.f, h.bc, h.de, h.hl, h.ix, h.iy, h.i, h.r);
    }

    if (!ZX_SnapshotCommitDual (handover_addr, h.pc,
                                Z80L_GO_ADDR, Z80L_GO_VALUE, 200u)) {
        /* Markers live in cart RAM (sp->ram[]) \u2014 readable any time, no
           BUSREQ needed.  Z80 stored them via the normal ISR WR-capture
           path; if a write was missed they stay 0x00 (we pre-cleared). */
        uint8_t mark[2] = {0xFFu, 0xFFu};
        (void)ZX_CartRamReadBlock (Z80L_DBG_ALIVE_ADDR, mark, 2u);
        printf ("z80: snapshot commit failed (handover did not fire); "
                "markers @3F7E=%02X @3F7F=%02X "
                "(want AA + BB; AA only=stuck in spinloop; "
                "00=trampoline never ran / WR missed)\r\n",
                mark[0], mark[1]);
        return Z80L_ERR_LAUNCH;
    }

    {
        uint8_t mark[2] = {0xFFu, 0xFFu};
        (void)ZX_CartRamReadBlock (Z80L_DBG_ALIVE_ADDR, mark, 2u);
        printf ("z80: handover fired @%04X (armed %04X/%04X), "
                "markers @3F7E=%02X @3F7F=%02X\r\n",
                (unsigned)ZX_SnapshotHandoverFiredAddr (),
                (unsigned)handover_addr, (unsigned)h.pc,
                mark[0], mark[1]);
    }

    /* Capture ~5ms of post-handover Z80 bus activity.  ROMCS is floated,
       cart edge is purely passive; tracer ISR just samples address/data/
       control on each MREQ falling edge.  Buffer is 128 events deep \u2014
       fills in ~30us of real Z80 execution, so we get the very first
       opcode fetches after handover.  This shows whether the Z80 actually
       fetches from h.pc, follows expected control flow, or wanders. */
    ZX_BusTraceArm ();
    ZX_BusTraceSample (200000u);   /* tight poll, ~few ms wall time */
    ZX_BusTraceStop ();
    ZX_BusTraceDump ();
    printf ("z80: snapshot resumed at PC=%04X. Reset device to return.\r\n",
            (unsigned)h.pc);
    return Z80L_OK;
}
