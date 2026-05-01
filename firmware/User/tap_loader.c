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

/* zxprog BSS+stack live in cart RAM (0x3000..0x3FFF); no guard region
   needed — NMI writes to ZX RAM 0x4000..0xFFFF cannot corrupt cart RAM. */

/* Write [addr..addr+len) to ZX RAM via NMI. */
static int tap_write_chunk (uint16_t addr, const uint8_t *buf, uint16_t len) {
    return ZX_NmiWriteBlock (addr, buf, len, TAP_NMI_TIMEOUT);
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

    (void)load;  /* used below for write decisions */

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

    /* Restore a clean Spectrum state and jump to start_addr via the
       cart-RAM launcher (regblock + tail staged by ZX_LaunchZ80). */
    if (!ZX_LaunchZ80 (start_addr)) {
        printf ("tap: ZX_LaunchZ80 failed\r\n");
        return TAP_ERR_NMI_WRITE;
    }

    printf ("tap: ROMCS released, Z80 running @ 0x%04X. Reset device to return.\r\n",
            (unsigned)start_addr);
    return TAP_OK;
}
