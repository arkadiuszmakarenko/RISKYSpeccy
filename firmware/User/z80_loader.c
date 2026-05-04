/*
 *  .z80 v1/v2/v3 snapshot loader.
 *
 *  v1: 30-byte header, flat 49152-byte body (optional RLE compression).
 *  v2: 30-byte base header + 2-byte ext-len + 23-byte ext header + paged blocks.
 *  v3: 30-byte base header + 2-byte ext-len + 54/55-byte ext header + paged blocks.
 *
 *  Only 48K hardware modes are supported.  128K/+2/+3 snapshots are rejected.
 */

#include "z80_loader.h"

#include "debug.h"
#include "ff.h"
#include "zx_bus.h"

#define Z80L_BODY_BASE         0x4000u
#define Z80L_BODY_LEN          49152u

#define Z80L_LOADER_FLAGS_ADDR      0x3F11u
#define Z80L_LOADER_FLAG_BORDER_ANIM 0x01u


/* Header bytes used by the .z80 spec (offsets from start of file). */
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
    uint8_t  version;         /* 1, 2, or 3 */
    uint8_t  hw_mode;         /* v2/v3 extended header byte 2 */
} Z80Header;

static void z80_parse_base_header (const uint8_t *h, Z80Header *out) {
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
    out->version = (out->pc != 0u) ? 1u : 0u;  /* 0 = need ext header */
    out->hw_mode = 0u;
}

/*
 * Read v2/v3 extended header.  File must be positioned at offset 30.
 * Accepted ext-header lengths: 23 (v2), 54 or 55 (v3).
 * Sets h->version, h->pc, h->hw_mode.
 */
static int z80_read_ext_header (FIL *fp, Z80Header *h) {
    uint8_t  len_bytes[2];
    uint8_t  ext[55];
    UINT     got;
    FRESULT  fr;
    uint16_t ext_len;
    UINT     to_read;

    fr = f_read (fp, len_bytes, 2u, &got);
    if ((fr != FR_OK) || (got != 2u)) { return 0; }
    ext_len = (uint16_t)len_bytes[0] | ((uint16_t)len_bytes[1] << 8);

    if ((ext_len != 23u) && (ext_len != 54u) && (ext_len != 55u)) {
        printf ("z80: unknown ext header len %u\r\n", (unsigned)ext_len);
        return 0;
    }
    h->version = (ext_len == 23u) ? 2u : 3u;

    to_read = (UINT)ext_len;   /* ext_len <= 55, fits in ext[] */
    fr = f_read (fp, ext, to_read, &got);
    if ((fr != FR_OK) || (got != to_read)) { return 0; }

    h->pc      = (uint16_t)ext[0] | ((uint16_t)ext[1] << 8);
    h->hw_mode = ext[2];
    return 1;
}

/*
 * v2 hardware mode: 0=48K, 1=48K+IF1, 2=SamRam, 3+=128K
 * v3 hardware mode: 0=48K, 1=48K+IF1, 2=48K+MGT, 3=SamRam, 4+=128K
 * Accept only modes that map to plain 48K RAM layout.
 */
static int z80_is_48k (const Z80Header *h) {
    if (h->version == 1u) { return 1; }
    if (h->version == 2u) { return (h->hw_mode <= 1u); }
    if (h->version == 3u) { return (h->hw_mode <= 2u); }
    return 0;
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
    uint32_t total;        /* bytes emitted in current block/pass */
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

/*
 * v1 compressed format: bytes are stored raw except runs of N copies of V
 * are encoded as `ED ED N V`.  End marker for a compressed v1 body is
 * the 4-byte sequence 00 ED ED 00 (a zero-length run).
 * v2/v3 per-block compression uses the same scheme but spans exactly one
 * 16384-byte page; block_len == 0xFFFF means uncompressed (always 16384 bytes).
 */

/* --- v1 flat body ------------------------------------------------ */
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

/* --- v2/v3 paged block streaming --------------------------------- */

#define Z80L_BLOCK_LEN  16384u
#define Z80L_BLOCK_RAW  0xFFFFu

/* Map .z80 page number to 48K ZX RAM base address. Returns 0 for unknown. */
static uint16_t z80_page_to_addr (uint8_t page) {
    switch (page) {
        case 4u: return 0x8000u;   /* RAM page 2 */
        case 5u: return 0xC000u;   /* RAM page 5 (top) */
        case 8u: return 0x4000u;   /* RAM page 5 in 48K = screen RAM */
        default: return 0x0000u;   /* ROM or unsupported page */
    }
}

/*
 * Decompress (or raw-copy) one 16384-byte page from r into ZX RAM at o->base.
 * block_len == 0xFFFF  → raw 16384-byte block.
 * block_len < 0xFFFF   → RLE-compressed data occupying block_len bytes,
 *                         expanding to exactly 16384 output bytes.
 */
static int z80_stream_block (Z80Reader *r, Z80OutState *o, uint16_t block_len) {
    o->out_pos = 0u;
    o->total   = 0u;

    if (block_len == Z80L_BLOCK_RAW) {
        uint16_t i;
        for (i = 0u; i < Z80L_BLOCK_LEN; ++i) {
            uint8_t b;
            if (!z80_reader_get (r, &b)) { return 0; }
            if (!z80_out_emit (o, b)) { return 0; }
        }
        return z80_out_flush (o);
    }

    /* Compressed: decompress until exactly 16384 output bytes. */
    while (o->total < Z80L_BLOCK_LEN) {
        uint8_t b;
        if (!z80_reader_get (r, &b)) { return 0; }
        if (b != 0xEDu) {
            if (!z80_out_emit (o, b)) { return 0; }
            continue;
        }
        {
            uint8_t b2;
            if (!z80_reader_get (r, &b2)) { return 0; }
            if (b2 != 0xEDu) {
                if (!z80_out_emit (o, 0xEDu)) { return 0; }
                if (!z80_out_emit (o, b2))    { return 0; }
                continue;
            }
        }
        {
            uint8_t cnt, val;
            if (!z80_reader_get (r, &cnt)) { return 0; }
            if (!z80_reader_get (r, &val)) { return 0; }
            while (cnt-- > 0u) {
                if (o->total >= Z80L_BLOCK_LEN) { break; }
                if (!z80_out_emit (o, val)) { return 0; }
            }
        }
    }
    return z80_out_flush (o);
}

/*
 * Skip `byte_count` bytes from the reader (used to discard unknown pages).
 */
static int z80_skip_bytes (Z80Reader *r, uint16_t byte_count) {
    uint16_t i;
    for (i = 0u; i < byte_count; ++i) {
        uint8_t dummy;
        if (!z80_reader_get (r, &dummy)) { return 0; }
    }
    return 1;
}

/*
 * Process all paged blocks from a v2/v3 body.
 * Reads block headers (len_lo, len_hi, page) until EOF and dispatches
 * known 48K pages (4, 5, 8) to ZX RAM; unknown pages are skipped.
 * Returns 1 if all three 48K pages were loaded successfully.
 */
static int z80_stream_v2v3_body (Z80Reader *r, Z80OutState *o) {
    uint8_t got_page4 = 0u;
    uint8_t got_page5 = 0u;
    uint8_t got_page8 = 0u;

    for (;;) {
        uint8_t  h0, h1, h2;
        uint16_t block_len;
        uint8_t  page;
        uint16_t zx_addr;

        /* Read 3-byte block header; first byte EOF = normal end of blocks. */
        if (!z80_reader_get (r, &h0)) { break; }
        if (!z80_reader_get (r, &h1)) {
            printf ("z80: truncated block header\r\n");
            return 0;
        }
        if (!z80_reader_get (r, &h2)) {
            printf ("z80: truncated block header\r\n");
            return 0;
        }

        block_len = (uint16_t)h0 | ((uint16_t)h1 << 8);
        page      = h2;
        zx_addr   = z80_page_to_addr (page);

        if (zx_addr == 0x0000u) {
            /* ROM or unknown page: skip its bytes */
            uint16_t skip = (block_len == Z80L_BLOCK_RAW) ? Z80L_BLOCK_LEN : block_len;
            printf ("z80: skipping page %u (%u bytes)\r\n",
                    (unsigned)page, (unsigned)skip);
            if (!z80_skip_bytes (r, skip)) { return 0; }
            continue;
        }

        o->base = zx_addr;
        if (!z80_stream_block (r, o, block_len)) {
            printf ("z80: block load failed (page %u → 0x%04X)\r\n",
                    (unsigned)page, (unsigned)zx_addr);
            return 0;
        }
        printf ("z80: page %u → 0x%04X\r\n", (unsigned)page, (unsigned)zx_addr);

        if (page == 4u) { got_page4 = 1u; }
        else if (page == 5u) { got_page5 = 1u; }
        else if (page == 8u) { got_page8 = 1u; }
    }

    if ((got_page4 == 0u) || (got_page5 == 0u) || (got_page8 == 0u)) {
        printf ("z80: missing pages (4=%u 5=%u 8=%u)\r\n",
                (unsigned)got_page4, (unsigned)got_page5, (unsigned)got_page8);
        return 0;
    }
    return 1;
}

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
        z80_parse_base_header (hdr, out_hdr);
    }

    if (out_hdr->version == 0u) {
        if (!z80_read_ext_header (fp, out_hdr)) {
            printf ("z80: failed to read extended header\r\n");
            f_close (fp);
            return Z80L_ERR_VERSION;
        }
        if (!z80_is_48k (out_hdr)) {
            printf ("z80: hw_mode %u not supported (128K/SamRam?)\r\n",
                    (unsigned)out_hdr->hw_mode);
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

    printf ("z80: v%u snapshot %s\r\n", (unsigned)h.version, path);
    printf ("  PC=%04X SP=%04X  A=%02X F=%02X BC=%04X DE=%04X HL=%04X\r\n",
            h.pc, h.sp, h.a, h.f, h.bc, h.de, h.hl);
    printf ("  A'=%02X F'=%02X BC'=%04X DE'=%04X HL'=%04X\r\n",
            h.a_alt, h.f_alt, h.bc_alt, h.de_alt, h.hl_alt);
    printf ("  IX=%04X IY=%04X  I=%02X R=%02X  border=%u  IM=%u  IFF1=%u",
            h.ix, h.iy, h.i, h.r, h.border, h.im, h.iff1);
    if (h.version == 1u) {
        printf ("  %s\r\n", h.compressed ? "(compressed)" : "(uncompressed)");
    } else {
        printf ("  hw_mode=%u (paged)\r\n", (unsigned)h.hw_mode);
    }
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

    printf ("z80: copying body (v%u, %s) to RAM via NMI mailbox\r\n",
            (unsigned)h.version,
            (h.version == 1u) ? (h.compressed ? "compressed" : "raw") : "paged");

    /* Enable ZX-side loading border animation only for snapshot body transfer. */
    loader_flags = Z80L_LOADER_FLAG_BORDER_ANIM;
    if (!ZX_CartRamWriteBlock (Z80L_LOADER_FLAGS_ADDR, &loader_flags, 1u)) {
        printf ("z80: failed to set loader flags\r\n");
        f_close (&fp);
        return Z80L_ERR_LOAD;
    }

    /* Stream body via NMI mailbox.
       v1: flat 49152-byte body to 0x4000..0xFFFF.
       v2/v3: series of 16384-byte pages dispatched to their ZX addresses. */
    z80_reader_init (&rd, &fp);
    z80_out_init (&out);
    {
        int body_ok;
        if (h.version == 1u) {
            body_ok = z80_stream_body (&rd, &out, h.compressed);
        } else {
            body_ok = z80_stream_v2v3_body (&rd, &out);
        }
        if (!body_ok) {
            printf ("z80: body stream failed\r\n");
            (void)ZX_CartRamWriteBlock (Z80L_LOADER_FLAGS_ADDR, &loader_flags_clear, 1u);
            f_close (&fp);
            return Z80L_ERR_LOAD;
        }
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
