/*
 *  .z80 v1 snapshot loader.
 *
 *  Strategy: stream the 49152-byte body from FATFS into ZX RAM via BUSREQ.
 *  Then stage the launcher in cart RAM (shared with Z80 via ROMCS window):
 *
 *      Regblock (26 bytes at 0x3F80):  all CPU registers
 *      Launcher tail (6 bytes at 0x3FF0):  IM x / EI-or-NOP / JP user_pc
 *
 *  zxprog's polling loop detects the launch trigger (0x55 at 0x3F10) and
 *  calls zx_launcher(), which restores all registers from the regblock,
 *  sets SP to the saved user SP, then jumps to the launcher tail.
 *
 *  The cart MPU ISR (RunCartWithM1Watch) is armed at user_pc (and 0x0038
 *  if IFF1+IM1).  On the matching M1 fetch, the ISR drops ROMCS and the
 *  cart disappears.  Execution continues from Spectrum ROM or ZX RAM.
 *
 *  Limitations: v1 only (PC must be non-zero in header).
 */

#include "z80_loader.h"

#include "debug.h"
#include "ff.h"
#include "zx_bus.h"

#include <string.h>

/* ===== Cart RAM addresses (must match zxprog.c) ===================== */

#define Z80L_REGBLOCK_ADDR       0x3F90u   /* 26-byte register block       */
#define Z80L_REGBLOCK_LEN        26u
/* Tail is 14 bytes (debug): LD A,3/OUT + IM + EI/NOP + LD A,4/OUT + JP */
#define Z80L_LAUNCHER_TAIL_ADDR  0x3FF0u   /* 14-byte launcher tail (debug) */
#define Z80L_LAUNCHER_TAIL_LEN   6u
#define Z80L_LAUNCHER_ALIVE_ADDR 0x3FAAu   /* alive marker (0xAA on entry) */
#define Z80L_LAUNCH_TRIGGER_ADDR 0x3F10u   /* trigger: CH32 writes 0x55    */
#define Z80L_LAUNCH_TRIGGER_GO   0x55u

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

/* ===== Body output (chunked BUSREQ write to ZX RAM) ================ */

#define Z80L_OUT_CHUNK 256u

typedef struct {
    uint16_t base;         /* current ZX target address  */
    uint16_t out_pos;      /* bytes in out_buf            */
    uint8_t  out_buf[Z80L_OUT_CHUNK];
    uint32_t total;
} Z80OutState;

static void z80_out_init (Z80OutState *o) {
    o->base    = Z80L_BODY_BASE;
    o->out_pos = 0u;
    o->total   = 0u;
}

static int z80_out_flush (Z80OutState *o) {
    if (o->out_pos == 0u) { return 1; }
    if (!ZX_BusWriteBlock (o->base, o->out_buf, o->out_pos)) {
        printf ("z80: BUSREQ write failed @0x%04X\r\n", (unsigned)o->base);
        return 0;
    }
    o->base   = (uint16_t)(o->base + o->out_pos);
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

/* ===== Trampoline / regblock for new cart-RAM launcher ============= */

/* Build the 26-byte regblock.  Layout (popped from 0x3F80 by zx_launcher):
 *   +0..5   BC', DE', HL'   (popped into main regs, EXX moves to alt)
 *   +6..7   AF'             (POP AF; EX AF,AF')
 *   +8..11  IX, IY
 *   +12..13 I-pair          (POP AF: hi=I, lo=junk F)
 *   +14..15 border, R_comp  (POP BC: lo=C=border, hi=B=R_comp)
 *   +16..21 main BC, DE, HL
 *   +22..23 main AF         (popped last so A is correct on entry)
 *   +24..25 user SP         (LD SP,(0x3F98))
 *
 * R_comp = (R_original - 12) compensated for 12 M1 cycles from LD R,A to
 * user_pc M1 fetch: POP BC/DE/HL/AF (4) + LD SP,(nn) (2) + JP (1) +
 * IM ED+byte (2) + EI/NOP (1) + JP C3 (1) + user_pc M1 (1) = 12.
 */
static void z80_build_regblock (const Z80Header *h, uint8_t *rb) {
    /* +0..5: BC', DE', HL' */
    rb[ 0] = (uint8_t)(h->bc_alt & 0xFFu); rb[ 1] = (uint8_t)(h->bc_alt >> 8);
    rb[ 2] = (uint8_t)(h->de_alt & 0xFFu); rb[ 3] = (uint8_t)(h->de_alt >> 8);
    rb[ 4] = (uint8_t)(h->hl_alt & 0xFFu); rb[ 5] = (uint8_t)(h->hl_alt >> 8);
    /* +6..7: AF' (lo=F', hi=A') */
    rb[ 6] = h->f_alt;
    rb[ 7] = h->a_alt;
    /* +8..11: IX, IY */
    rb[ 8] = (uint8_t)(h->ix & 0xFFu); rb[ 9] = (uint8_t)(h->ix >> 8);
    rb[10] = (uint8_t)(h->iy & 0xFFu); rb[11] = (uint8_t)(h->iy >> 8);
    /* +12..13: I via POP AF (lo=junk F, hi=I) */
    rb[12] = 0u;
    rb[13] = h->i;
    /* +14..15: border + R_comp via POP BC (lo=C=border, hi=B=R_comp).
       Bit 7 of R is preserved (LD R,A only cycles bits 0-6). */
    rb[14] = h->border;
    /* R_comp: 12 M1 cycles consumed from LD R,A to the first fetch of
       user_pc (ED/IM=4, EI/NOP=1, JP=1, fetch=1 ... totals ~12 M1s). */
    rb[15] = (uint8_t)(((h->r - 12u) & 0x7Fu) | (h->r & 0x80u));
    /* +16..21: main BC, DE, HL */
    rb[16] = (uint8_t)(h->bc & 0xFFu); rb[17] = (uint8_t)(h->bc >> 8);
    rb[18] = (uint8_t)(h->de & 0xFFu); rb[19] = (uint8_t)(h->de >> 8);
    rb[20] = (uint8_t)(h->hl & 0xFFu); rb[21] = (uint8_t)(h->hl >> 8);
    /* +22..23: main AF (popped last so A is correct on entry) */
    rb[22] = h->f;
    rb[23] = h->a;
    /* +24..25: user SP (launcher does LD SP,(0x3F98) then JP user_pc) */
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

int Z80_LoadAndRun (const char *path) {
    static FIL fp;
    static Z80OutState out;
    Z80Header h;
    Z80Reader rd;
    int rc;
    uint8_t regblock[Z80L_REGBLOCK_LEN];
    uint8_t tail[Z80L_LAUNCHER_TAIL_LEN];
    uint16_t handover_addr_b;

    /* Ensure the cart ROM (zxprog) is active and the Z80 starts fresh from
       0x0000.  ZX_RomcsAssert re-enables ROMCS + EXTI/NVIC and clears
       s_rom_released (which a previous failed run may have set to 1).
       Clear LAUNCH_TRIGGER (0x3F10) BEFORE reset: zxprog's BSS init does not
       cover this fixed address, so a 0x55 left by a previous run would cause
       zxprog to fire the stale launcher immediately after reset, before the
       new regblock and tail are staged.
       ZX_Z80Reset then pulses /RESET so the Z80 re-runs zxprog startup and
       is in the trigger-polling main loop before we proceed. */
    ZX_RomcsAssert ();
    {
        uint8_t zero = 0u;
        (void)ZX_CartRamWriteBlock (Z80L_LAUNCH_TRIGGER_ADDR, &zero, 1u);
    }
    ZX_Z80Reset ();

    rc = z80_open_and_parse (path, &fp, &h);
    if (rc != Z80L_OK) { return rc; }

    printf ("z80: PC=%04X SP=%04X border=%u IM=%u IFF=%u %s\r\n",
            h.pc, h.sp, h.border, h.im, h.iff1,
            h.compressed ? "compressed" : "raw");

    /* Stream 49152-byte body via BUSREQ to 0x4000..0xFFFF.
       zxprog BSS is in cart RAM (0x3000..0x3FFF), never clobbered. */
    z80_reader_init (&rd, &fp);
    z80_out_init (&out);
    if (!z80_stream_body (&rd, &out, h.compressed)) {
        printf ("z80: body stream failed (%lu bytes)\r\n",
                (unsigned long)out.total);
        f_close (&fp);
        return Z80L_ERR_LOAD;
    }
    f_close (&fp);
    printf ("z80: body streamed (%lu bytes via BUSREQ)\r\n",
            (unsigned long)out.total);

    /* Launcher tail at 0x3FF0 (6 bytes):
         ED / IM_byte   — set interrupt mode
         EI or NOP      — re-enable interrupts (or skip if IFF1=0)
         C3 / lo / hi   — JP user_pc */
    tail[0] = 0xEDu;
    tail[1] = z80_im_byte (h.im);
    tail[2] = (h.iff1 != 0u) ? 0xFBu : 0x00u;        /* EI or NOP */
    tail[3] = 0xC3u;                                  /* JP        */
    tail[4] = (uint8_t)(h.pc & 0xFFu);
    tail[5] = (uint8_t)(h.pc >> 8);
    if (!ZX_CartRamWriteBlock (Z80L_LAUNCHER_TAIL_ADDR, tail,
                               Z80L_LAUNCHER_TAIL_LEN)) {
        printf ("z80: launcher tail write failed\r\n");
        return Z80L_ERR_LOAD;
    }

    /* Build and install regblock in cart RAM at 0x3F80. */
    z80_build_regblock (&h, regblock);
    if (!ZX_CartRamWriteBlock (Z80L_REGBLOCK_ADDR, regblock,
                               Z80L_REGBLOCK_LEN)) {
        printf ("z80: regblock write failed\r\n");
        return Z80L_ERR_LOAD;
    }

    /* Pre-clear alive marker; launcher writes 0xAA there on entry. */
    {
        uint8_t zero = 0u;
        (void)ZX_CartRamWriteBlock (Z80L_LAUNCHER_ALIVE_ADDR, &zero, 1u);
    }

    /* Pre-launch diagnostics. */
    {
        uint8_t buf[16];
        unsigned i;
        printf ("z80: A=%02X F=%02X BC=%04X DE=%04X HL=%04X "
                "IX=%04X IY=%04X I=%02X R=%02X\r\n",
                h.a, h.f, h.bc, h.de, h.hl, h.ix, h.iy, h.i, h.r);
        if ((h.sp >= 0x4000u) && ZX_BusReadBlock (h.sp, buf, 8u)) {
            printf ("z80: stack@%04X:", (unsigned)h.sp);
            for (i = 0u; i < 8u; ++i) printf (" %02X", buf[i]);
            printf ("\r\n");
        }
    }

     /* Arm M1 watch only at user_pc.
         Watching 0x0038 can hand over during an interrupt preamble and route
         execution into ROM interrupt flow instead of the intended game PC. */
     handover_addr_b = 0xFFFFu;
     if ((h.iff1 != 0u) && (h.im == 1u)) {
          printf ("z80: handover armed at %04X (IM1+IFF1; 0038 watch disabled)\r\n",
                     (unsigned)h.pc);
     }

    /* ZX_SnapshotCommitDual: arms M1 watch, writes trigger (0x55) to
       LAUNCH_TRIGGER_ADDR (in cart RAM => go_in_cart path, no BUSREQ),
       waits for ISR to fire, then calls ZX_RomcsRelease(). */
    if (!ZX_SnapshotCommitDual (h.pc, handover_addr_b,
                                Z80L_LAUNCH_TRIGGER_ADDR,
                                Z80L_LAUNCH_TRIGGER_GO,
                                2000u)) {
        uint8_t alive = 0xFFu;
        uint8_t trig = 0xFFu;
        (void)ZX_CartRamReadBlock (Z80L_LAUNCHER_ALIVE_ADDR, &alive, 1u);
        (void)ZX_CartRamReadBlock (Z80L_LAUNCH_TRIGGER_ADDR, &trig, 1u);
        printf ("z80: launch failed; alive@%04X=%02X "
            "trigger@%04X=%02X "
            "(alive: 0x00=not seen, 0x5A=zxprog saw trigger, 0xAA=launcher entered)\r\n",
            (unsigned)Z80L_LAUNCHER_ALIVE_ADDR, (unsigned)alive,
            (unsigned)Z80L_LAUNCH_TRIGGER_ADDR, (unsigned)trig);
        printf ("z80: --- launch window trace (failure) ---\r\n");
        ZX_BusTraceDump ();
        return Z80L_ERR_LAUNCH;
    }

    {
        uint8_t alive = 0xFFu;
        (void)ZX_CartRamReadBlock (Z80L_LAUNCHER_ALIVE_ADDR, &alive, 1u);
        printf ("z80: handover fired @%04X; alive=%02X\r\n",
                (unsigned)ZX_SnapshotHandoverFiredAddr (), (unsigned)alive);
    }

    /* Dump the launch-window trace captured by RunCartWithM1Watch.
       This shows: trigger RD at 3F10, alive WR at 3F9A (=AA), register
       POPs from 3F80..3F99, tail M1 fetches at 3FF0..3FF3, and the
       final JP user_pc M1 that fired the handover. */
    printf ("z80: --- launch window trace ---\r\n");
    ZX_BusTraceDump ();

    /* Re-arm for post-handover: capture first instructions after ROMCS drops. */
    ZX_BusTraceArm ();
    ZX_BusTraceSample (200000u);
    ZX_BusTraceStop ();
    printf ("z80: --- post-handover trace ---\r\n");
    ZX_BusTraceDump ();

    printf ("z80: snapshot resumed at PC=%04X. Reset device to return.\r\n",
            (unsigned)h.pc);
    return Z80L_OK;
}
