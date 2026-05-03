/*
 *  .z80 v1 body loader.
 *
 *  This module only copies the 49152-byte snapshot body into ZX RAM
 *  (0x4000..0xFFFF) via NMI mailbox writes. It does not launch code, restore
 *  CPU state, or perform ROMCS handover.
 */

#include "z80_loader.h"

#include "debug.h"
#include "ff.h"
#include "zx_bus.h"

#define Z80L_BODY_BASE         0x4000u
#define Z80L_BODY_LEN          49152u

#define Z80L_LOADER_FLAGS_ADDR      0x3F11u
#define Z80L_LOADER_FLAG_BORDER_ANIM 0x01u


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

/* ===== Body output (chunked NMI mailbox write to ZX RAM) =========== */

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
        printf ("z80: NMI write failed @0x%04X\r\n", (unsigned)o->base);
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
    ZX_Z80State state;
    uint8_t loader_flags;
    uint8_t loader_flags_clear = 0u;
    int rc;

    rc = z80_open_and_parse (path, &fp, &h);
    if (rc != Z80L_OK) { return rc; }

    printf ("z80: copying body (%s) to RAM via NMI mailbox\r\n",
            h.compressed ? "compressed" : "raw");

    /* Enable ZX-side loading border animation only for snapshot body transfer. */
    loader_flags = Z80L_LOADER_FLAG_BORDER_ANIM;
    if (!ZX_CartRamWriteBlock (Z80L_LOADER_FLAGS_ADDR, &loader_flags, 1u)) {
        printf ("z80: failed to set loader flags\r\n");
        f_close (&fp);
        return Z80L_ERR_LOAD;
    }

    /* Stream 49152-byte body via NMI mailbox to 0x4000..0xFFFF.
       zxprog BSS is in cart RAM (0x3000..0x3FFF), never clobbered. */
    z80_reader_init (&rd, &fp);
    z80_out_init (&out);
    if (!z80_stream_body (&rd, &out, h.compressed)) {
        printf ("z80: body stream failed (%lu bytes)\r\n",
                (unsigned long)out.total);
        (void)ZX_CartRamWriteBlock (Z80L_LOADER_FLAGS_ADDR, &loader_flags_clear, 1u);
        f_close (&fp);
        return Z80L_ERR_LOAD;
    }
    f_close (&fp);
    (void)ZX_CartRamWriteBlock (Z80L_LOADER_FLAGS_ADDR, &loader_flags_clear, 1u);
    printf ("z80: body streamed (%lu bytes)\r\n", (unsigned long)out.total);

    /* Build CPU state from the snapshot header. */
    state.a      = h.a;       state.f      = h.f;
    state.a_alt  = h.a_alt;   state.f_alt  = h.f_alt;
    state.bc     = h.bc;      state.de     = h.de;      state.hl     = h.hl;
    state.bc_alt = h.bc_alt;  state.de_alt = h.de_alt;  state.hl_alt = h.hl_alt;
    state.ix     = h.ix;      state.iy     = h.iy;
    state.sp     = h.sp;      state.pc     = h.pc;
    state.i      = h.i;       state.r      = h.r;
    state.iff1   = h.iff1;
    state.im     = h.im;
    state.border = h.border;

    printf ("z80: launching PC=%04X SP=%04X IM=%u IFF1=%u\r\n",
            (unsigned)state.pc, (unsigned)state.sp,
            (unsigned)state.im, (unsigned)state.iff1);

    if (!ZX_LaunchZ80 (&state)) {
        printf ("z80: launch failed\r\n");
        return Z80L_ERR_LAUNCH;
    }

    return Z80L_OK;
}
