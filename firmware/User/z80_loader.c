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

#define Z80L_REGBLOCK_ADDR     0x3F80u   /* cart RAM: 28 bytes of state   */
#define Z80L_REGBLOCK_LEN      28u
#define Z80L_TRAMP_ADDR        0x3FA0u   /* cart RAM: trampoline code     */
#define Z80L_TRAMP_LEN         60u
#define Z80L_ALIVE_ADDR        0x3FDCu
#define Z80L_GO_ADDR           0x3FDDu
#define Z80L_ALIVE_VALUE       0xAAu
#define Z80L_GO_VALUE          0x55u

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

/* Fixed assembled bytes; offsets [37], [51], [56], [58] and [59] are patched
    per-snapshot. Layout matches the regblock in cart RAM at 0x3F80.
    The trampoline first clears its cart-RAM go-byte, then restores the saved
    register image and jumps directly to the real snapshot PC.  M1 handover is
    armed on that first user fetch, so ZX top RAM stays completely intact. */
static const uint8_t Z80L_TRAMPOLINE[Z80L_TRAMP_LEN] = {
    /* 00 */ 0xF3,                       /* DI                            */
    /* 01 */ 0xAF,                       /* XOR A                         */
     /* 02 */ 0x32, 0xDD, 0x3F,           /* LD (0x3FDD), A   ; clear go   */
    /* 05 */ 0x3E, 0xAA,                 /* LD A, 0xAA                    */
     /* 07 */ 0x32, 0xDC, 0x3F,           /* LD (0x3FDC), A   ; alive      */
     /* 10 */ 0x3A, 0xDD, 0x3F,           /* LD A, (0x3FDD)                */
    /* 13 */ 0xFE, 0x55,                 /* CP 0x55                       */
    /* 15 */ 0x20, 0xF9,                 /* JR NZ, -7   (back to LD A)    */
     /* 17 */ 0x31, 0x80, 0x3F,           /* LD SP, 0x3F80                 */
    /* 20 */ 0xF1,                       /* POP AF       (main)           */
    /* 21 */ 0xC1,                       /* POP BC       (main)           */
    /* 22 */ 0xD1,                       /* POP DE       (main)           */
    /* 23 */ 0xE1,                       /* POP HL       (main)           */
    /* 24 */ 0xD9,                       /* EXX                           */
    /* 25 */ 0xC1,                       /* POP BC'                       */
    /* 26 */ 0xD1,                       /* POP DE'                       */
    /* 27 */ 0xE1,                       /* POP HL'                       */
    /* 28 */ 0xD9,                       /* EXX                           */
    /* 29 */ 0x08,                       /* EX AF, AF'                    */
    /* 30 */ 0xF1,                       /* POP AF       (alt)            */
    /* 31 */ 0x08,                       /* EX AF, AF'                    */
    /* 32 */ 0xDD, 0xE1,                 /* POP IX                        */
    /* 34 */ 0xFD, 0xE1,                 /* POP IY                        */
    /* 36 */ 0x3E, 0x00,                 /* LD A, <border>     PATCH @37  */
    /* 38 */ 0xD3, 0xFE,                 /* OUT (0xFE), A                 */
    /* 40 */ 0x3A, 0x94, 0x3F,           /* LD A, (0x3F94)  ; I           */
    /* 43 */ 0xED, 0x47,                 /* LD I, A                       */
    /* 45 */ 0x3A, 0x95, 0x3F,           /* LD A, (0x3F95)  ; R           */
    /* 48 */ 0xED, 0x4F,                 /* LD R, A                       */
    /* 50 */ 0xED, 0x56,                 /* IM 1            PATCH @51     */
    /* 52 */ 0xED, 0x7B, 0x9A, 0x3F,     /* LD SP, (0x3F9A)               */
    /* 56 */ 0xFB,                       /* EI              PATCH @56     */
    /* 57 */ 0xC3, 0x00, 0x00            /* JP <user_pc>     PATCH @58/59 */
};

/* Build the 28-byte register block (POP-order pairs) in cart RAM. */
static void z80_build_regblock (const Z80Header *h, uint8_t *rb) {
    /* main AF, BC, DE, HL */
    rb[ 0] = h->f;       rb[ 1] = h->a;
    rb[ 2] = (uint8_t)(h->bc & 0xFFu); rb[ 3] = (uint8_t)(h->bc >> 8);
    rb[ 4] = (uint8_t)(h->de & 0xFFu); rb[ 5] = (uint8_t)(h->de >> 8);
    rb[ 6] = (uint8_t)(h->hl & 0xFFu); rb[ 7] = (uint8_t)(h->hl >> 8);
    /* alt BC, DE, HL */
    rb[ 8] = (uint8_t)(h->bc_alt & 0xFFu); rb[ 9] = (uint8_t)(h->bc_alt >> 8);
    rb[10] = (uint8_t)(h->de_alt & 0xFFu); rb[11] = (uint8_t)(h->de_alt >> 8);
    rb[12] = (uint8_t)(h->hl_alt & 0xFFu); rb[13] = (uint8_t)(h->hl_alt >> 8);
    /* alt AF */
    rb[14] = h->f_alt;   rb[15] = h->a_alt;
    /* IX, IY */
    rb[16] = (uint8_t)(h->ix & 0xFFu); rb[17] = (uint8_t)(h->ix >> 8);
    rb[18] = (uint8_t)(h->iy & 0xFFu); rb[19] = (uint8_t)(h->iy >> 8);
    /* I, R */
    rb[20] = h->i;
    rb[21] = h->r;
    /* unused, kept for layout symmetry */
    rb[22] = 0u; rb[23] = 0u; rb[24] = 0u; rb[25] = 0u;
    /* user_sp (PC is reached by direct JP, not by stack-pop RET) */
    rb[26] = (uint8_t)(h->sp & 0xFFu);
    rb[27] = (uint8_t)(h->sp >> 8);
}

static uint8_t z80_im_byte (uint8_t im) {
    switch (im) {
        case 0u:  return 0x46u;   /* IM 0 */
        case 2u:  return 0x5Eu;   /* IM 2 */
        default:  return 0x56u;   /* IM 1 (also default for unknown) */
    }
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
    tramp[37] = h.border;
    tramp[51] = z80_im_byte (h.im);
    tramp[56] = (h.iff1 != 0u) ? 0xFBu : 0x00u;   /* EI / NOP */
    tramp[58] = (uint8_t)(h.pc & 0xFFu);
    tramp[59] = (uint8_t)(h.pc >> 8);
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

        if (!ZX_SnapshotEnter (Z80L_TRAMP_ADDR, Z80L_ALIVE_ADDR,
                               Z80L_ALIVE_VALUE, 1000u)) {
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

        /* Quick ground-truth verify from cart RAM. */
        {
            uint8_t v[6];
            uint8_t tmp[4];
            memset (v, 0, sizeof (v));
            if (ZX_CartRamReadBlock ((uint16_t)(Z80L_REGBLOCK_ADDR + 26u), tmp, 2u)) {
                v[0] = tmp[0]; v[1] = tmp[1];
            }
            if (ZX_CartRamReadBlock ((uint16_t)(Z80L_TRAMP_ADDR + 56u), tmp, 4u)) {
                v[2] = tmp[0]; v[3] = tmp[1]; v[4] = tmp[2]; v[5] = tmp[3];
            }
            printf ("z80: cart verify SP=%02X %02X JP=%02X %02X %02X %02X (want %02X %02X %02X %02X %02X %02X)\r\n",
                    v[0], v[1], v[2], v[3], v[4], v[5],
                    regblock[26], regblock[27], tramp[56], tramp[57], tramp[58], tramp[59]);
        }

        if (!ZX_SnapshotEnter (Z80L_TRAMP_ADDR, Z80L_ALIVE_ADDR,
                               Z80L_ALIVE_VALUE, 1000u)) {
            return Z80L_ERR_LAUNCH;
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

     if (!ZX_SnapshotCommit (h.pc, Z80L_GO_ADDR,
                            Z80L_GO_VALUE, 200u)) {
        printf ("z80: snapshot commit failed (handover did not fire)\r\n");
        return Z80L_ERR_LAUNCH;
    }

    printf ("z80: snapshot resumed at PC=%04X. Reset device to return.\r\n",
            (unsigned)h.pc);
    return Z80L_OK;
}
