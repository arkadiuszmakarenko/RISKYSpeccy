#include "tap_loader.h"

#include "debug.h"
#include "ff.h"
#include "zx_bus.h"

#include <string.h>

#define TAP_NMI_CHUNK     0x0100u   /* 256 bytes per NMI write — fits comfortably
                                       inside ZX_NMI_WCMD_CHUNK (512) and keeps
                                       the staging buffer small */
#define TAP_NMI_TIMEOUT   400u
#define TAP_HEADER_LEN    19u

/* zxprog's BSS+stack live at 0xBE00-0xBEFF (minimal NMI-only zxprog).
   NMI-writing into that region corrupts the Z80 program that's actually
   performing the writes, so any bytes destined for it are buffered in CH32
   RAM and BUSREQ-flushed AFTER the launch trampoline has taken over but
   BEFORE ROMCS is released. */
#define TAP_GUARD_LO       0xBE00u
#define TAP_GUARD_HI       0xBF00u  /* exclusive */
#define TAP_GUARD_SIZE     (TAP_GUARD_HI - TAP_GUARD_LO)

static uint8_t  s_guard_buf[TAP_GUARD_SIZE];
static uint16_t s_guard_lo_used;   /* lowest absolute addr touched */
static uint16_t s_guard_hi_used;   /* one past highest absolute addr touched */

static void tap_guard_reset (void) {
    s_guard_lo_used = TAP_GUARD_HI;
    s_guard_hi_used = TAP_GUARD_LO;
    /* don't bother clearing s_guard_buf — only [lo_used,hi_used) is read */
}

static void tap_guard_capture (uint16_t addr, const uint8_t *src, uint16_t len) {
    uint16_t off = (uint16_t)(addr - TAP_GUARD_LO);
    uint16_t i;
    for (i = 0u; i < len; ++i) {
        s_guard_buf[off + i] = src[i];
    }
    if (addr < s_guard_lo_used) { s_guard_lo_used = addr; }
    if ((uint32_t)addr + len > s_guard_hi_used) {
        s_guard_hi_used = (uint16_t)((uint32_t)addr + len);
    }
}

/* Write [addr..addr+len) to ZX RAM, routing any guard-region bytes into the
   deferred buffer and NMI-writing the rest. */
static int tap_write_chunk (uint16_t addr, const uint8_t *buf, uint16_t len) {
    uint32_t end = (uint32_t)addr + len;

    /* Fully outside guard region — straight NMI. */
    if ((end <= TAP_GUARD_LO) || (addr >= TAP_GUARD_HI)) {
        return ZX_NmiWriteBlock (addr, buf, len, TAP_NMI_TIMEOUT);
    }

    /* Pre-guard portion (NMI). */
    if (addr < TAP_GUARD_LO) {
        uint16_t pre = (uint16_t)(TAP_GUARD_LO - addr);
        if (!ZX_NmiWriteBlock (addr, buf, pre, TAP_NMI_TIMEOUT)) {
            return 0;
        }
        addr = TAP_GUARD_LO;
        buf += pre;
        len = (uint16_t)(len - pre);
        end = (uint32_t)addr + len;
    }

    /* Guard portion (deferred). */
    {
        uint16_t in_guard = (end > TAP_GUARD_HI)
                                ? (uint16_t)(TAP_GUARD_HI - addr)
                                : len;
        tap_guard_capture (addr, buf, in_guard);
        addr = (uint16_t)(addr + in_guard);
        buf += in_guard;
        len = (uint16_t)(len - in_guard);
    }

    /* Post-guard portion (NMI). */
    if (len > 0u) {
        if (!ZX_NmiWriteBlock (addr, buf, len, TAP_NMI_TIMEOUT)) {
            return 0;
        }
    }
    return 1;
}

/* Returns 1 on full read, 0 on short read / error, sets *eof on clean EOF */
static int tap_read_exact (FIL *fp, void *buffer, UINT count, int *eof) {
    UINT got = 0u;
    FRESULT fr = f_read (fp, buffer, count, &got);

    if (fr != FR_OK) {
        return 0;
    }
    if (got == 0u) {
        if (eof != NULL) { *eof = 1; }
        return 0;
    }
    if (got != count) {
        return 0;
    }
    return 1;
}

static const char *tap_type_name (uint8_t type) {
    switch (type) {
        case 0u: return "PROGRAM";
        case 1u: return "NUM ARRAY";
        case 2u: return "STR ARRAY";
        case 3u: return "CODE";
        default: return "UNKNOWN";
    }
}

static void tap_print_name (const uint8_t *raw) {
    char name[11];
    int i;
    for (i = 0; i < 10; ++i) {
        uint8_t c = raw[i];
        name[i] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '.';
    }
    name[10] = '\0';
    printf("\"%s\"", name);
}

/* Internal worker. If load == 0 only prints info, doesn't touch ZX RAM.
   On load == 1 writes CODE blocks via NMI and returns first CODE block's
   load address in *out_start_addr (if non-NULL). */
static int tap_process (const char *path, int load,
                        uint16_t *out_start_addr, int *out_code_count) {
    static FIL fp;                  /* large struct — keep as static */
    FRESULT fr;
    int rc = TAP_OK;
    int eof = 0;
    int code_blocks = 0;
    uint8_t header_buf[TAP_HEADER_LEN];
    uint8_t flag;
    uint8_t length_lo;
    uint8_t length_hi;
    uint16_t length;
    uint16_t pending_load_addr = 0u;
    uint16_t pending_data_len = 0u;
    int      pending_is_code = 0;
    int      first_code_addr_known = 0;
    uint16_t first_code_addr = 0u;
    uint8_t  chunk[TAP_NMI_CHUNK];

    fr = f_open (&fp, path, FA_READ);
    if (fr != FR_OK) {
        printf ("tap: cannot open '%s' (fr=%d)\r\n", path, (int)fr);
        return TAP_ERR_OPEN;
    }

    if (load) {
        tap_guard_reset();
    }

    while (1) {
        /* 16-bit length prefix */
        if (!tap_read_exact (&fp, &length_lo, 1u, &eof)) { break; }
        if (!tap_read_exact (&fp, &length_hi, 1u, NULL)) { rc = TAP_ERR_READ; break; }
        length = (uint16_t)length_lo | ((uint16_t)length_hi << 8);
        if (length < 2u) {
            printf ("tap: malformed block length %u\r\n", (unsigned)length);
            rc = TAP_ERR_FORMAT;
            break;
        }

        /* flag byte */
        if (!tap_read_exact (&fp, &flag, 1u, NULL)) { rc = TAP_ERR_READ; break; }

        if ((flag == 0x00u) && (length == TAP_HEADER_LEN)) {
            /* Header block: 17 bytes payload + 1 checksum after the flag. */
            if (!tap_read_exact (&fp, header_buf, TAP_HEADER_LEN - 1u, NULL)) {
                rc = TAP_ERR_READ;
                break;
            }
            {
                uint8_t  type     = header_buf[0];
                const uint8_t *nm = &header_buf[1];
                uint16_t data_len = (uint16_t)header_buf[11] | ((uint16_t)header_buf[12] << 8);
                uint16_t p1       = (uint16_t)header_buf[13] | ((uint16_t)header_buf[14] << 8);
                uint16_t p2       = (uint16_t)header_buf[15] | ((uint16_t)header_buf[16] << 8);

                printf ("  HDR  %-9s ", tap_type_name (type));
                tap_print_name (nm);
                printf (" len=%u p1=0x%04X p2=0x%04X\r\n",
                        (unsigned)data_len, (unsigned)p1, (unsigned)p2);

                pending_is_code   = (type == 3u);
                pending_load_addr = p1;
                pending_data_len  = data_len;
            }
        } else if (flag == 0xFFu) {
            /* Data block: length-2 bytes of payload then checksum. */
            uint16_t data_len = (uint16_t)(length - 2u);
            uint16_t remaining = data_len;
            uint16_t addr = pending_load_addr;

            printf ("  DATA len=%u%s",
                    (unsigned)data_len,
                    pending_is_code ? "" : " (skipped)");

            if (pending_is_code && load) {
                printf (" -> 0x%04X", (unsigned)pending_load_addr);
                if (!first_code_addr_known) {
                    first_code_addr = pending_load_addr;
                    first_code_addr_known = 1;
                }
            }
            printf ("\r\n");

            if (pending_is_code && (data_len != pending_data_len)) {
                printf ("  WARN data len %u != header len %u\r\n",
                        (unsigned)data_len, (unsigned)pending_data_len);
            }

            while (remaining > 0u) {
                uint16_t take = (remaining > TAP_NMI_CHUNK)
                                    ? (uint16_t)TAP_NMI_CHUNK
                                    : remaining;
                if (!tap_read_exact (&fp, chunk, take, NULL)) {
                    rc = TAP_ERR_READ;
                    break;
                }
                if (pending_is_code && load) {
                    if (!tap_write_chunk (addr, chunk, take)) {
                        printf ("tap: write failed @0x%04X\r\n", (unsigned)addr);
                        rc = TAP_ERR_NMI_WRITE;
                        break;
                    }
                }
                addr = (uint16_t)(addr + take);
                remaining = (uint16_t)(remaining - take);
            }
            if (rc != TAP_OK) { break; }

            /* Checksum byte */
            {
                uint8_t cks;
                if (!tap_read_exact (&fp, &cks, 1u, NULL)) {
                    rc = TAP_ERR_READ;
                    break;
                }
                if (pending_is_code) { ++code_blocks; }
            }
            pending_is_code = 0;
        } else {
            /* Unknown / oddly-sized block — consume and skip. */
            uint16_t skip = (uint16_t)(length - 1u);
            printf ("  SKIP flag=0x%02X len=%u\r\n", (unsigned)flag, (unsigned)length);
            while (skip > 0u) {
                uint16_t take = (skip > TAP_NMI_CHUNK) ? TAP_NMI_CHUNK : skip;
                if (!tap_read_exact (&fp, chunk, take, NULL)) {
                    rc = TAP_ERR_READ;
                    break;
                }
                skip = (uint16_t)(skip - take);
            }
            if (rc != TAP_OK) { break; }
        }
    }

    f_close (&fp);

    if (rc == TAP_OK) {
        if (load && (code_blocks == 0)) {
            printf ("tap: no CODE blocks found\r\n");
            rc = TAP_ERR_NO_CODE;
        } else {
            if (out_start_addr != NULL) {
                *out_start_addr = first_code_addr_known ? first_code_addr : 0u;
            }
            if (out_code_count != NULL) {
                *out_code_count = code_blocks;
            }
        }
    }

    return rc;
}

int Tap_Info (const char *path) {
    int rc;
    printf ("tap: %s\r\n", path);
    rc = tap_process (path, 0, NULL, NULL);
    return rc;
}

int Tap_Load (const char *path, uint16_t *out_start_addr, int *out_block_count) {
    int rc;
    printf ("tap: loading %s\r\n", path);
    rc = tap_process (path, 1, out_start_addr, out_block_count);
    return rc;
}

int Tap_LoadAndRun (const char *path, uint16_t start_addr) {
    uint16_t auto_start = 0u;
    int blocks = 0;
    int rc = Tap_Load (path, &auto_start, &blocks);

    if (rc != TAP_OK) {
        return rc;
    }

    if (start_addr == 0u) {
        start_addr = auto_start;
    }
    if (start_addr == 0u) {
        printf ("tap: no start address (use 'taprun <path> <hex>')\r\n");
        return TAP_ERR_NO_CODE;
    }

    printf ("tap: %d code block(s), launching @ 0x%04X\r\n", blocks, (unsigned)start_addr);

    /* Phase 1: install trampoline, redirect Z80 PC, wait for trampoline alive. */
    if (!ZX_LaunchPrepare (start_addr)) {
        printf ("tap: launch prepare failed (trampoline did not come alive)\r\n");
        ZX_RomcsAssert();
        return TAP_ERR_NMI_WRITE;
    }

    /* Phase 2: Z80 is now in the trampoline spin-loop, zxprog is gone — safe to
       BUSREQ-write the deferred bytes that landed in zxprog's BSS region. */
    if (s_guard_lo_used < s_guard_hi_used) {
        uint16_t flush_len = (uint16_t)(s_guard_hi_used - s_guard_lo_used);
        printf ("tap: flushing %u deferred byte(s) @0x%04X..0x%04X\r\n",
                (unsigned)flush_len,
                (unsigned)s_guard_lo_used,
                (unsigned)(s_guard_hi_used - 1u));
        if (!ZX_BusWriteBlock (s_guard_lo_used,
                               &s_guard_buf[s_guard_lo_used - TAP_GUARD_LO],
                               flush_len)) {
            printf ("tap: deferred BUSREQ write failed\r\n");
            ZX_RomcsAssert();
            return TAP_ERR_NMI_WRITE;
        }
    }

    /* Phase 3: write go-flag, drop ROMCS, release bus. */
    if (!ZX_LaunchCommit()) {
        printf ("tap: launch commit failed\r\n");
        ZX_RomcsAssert();
        return TAP_ERR_NMI_WRITE;
    }

    printf ("tap: ROMCS released, Z80 running game. Reset device to return.\r\n");
    return TAP_OK;
}
