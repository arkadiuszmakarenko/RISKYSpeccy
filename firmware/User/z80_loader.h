#ifndef __Z80_LOADER_H
#define __Z80_LOADER_H

#include <stdint.h>

#define Z80L_OK             0
#define Z80L_ERR_OPEN       1
#define Z80L_ERR_READ       2
#define Z80L_ERR_FORMAT     3
#define Z80L_ERR_VERSION    4
#define Z80L_ERR_LOAD       5
#define Z80L_ERR_LAUNCH     6

/* Parsed snapshot metadata returned by Z80_GetFileInfo. */
typedef struct {
    uint8_t  version;       /* 1, 2, or 3                                    */
    uint8_t  compressed;    /* v1 only: 1 if body is RLE-compressed          */
    uint8_t  hw_mode;       /* v2/v3 extended header byte 2 (0 for v1)       */
    uint8_t  is_48k;        /* 1 if hardware mode maps to 48K                */
    uint8_t  is_128k;       /* 1 if hardware mode maps to 128K/+2            */
    uint8_t  page_7ffd;     /* last OUT to 0x7FFD (v2/v3 128K only, else 0) */
    uint16_t pc;            /* program counter at snapshot time              */
    uint16_t sp;            /* stack pointer at snapshot time                */
    uint32_t file_size;     /* file size in bytes (0 if stat failed)         */
} Z80FileInfo;

/* Print a header summary without touching ZX RAM. */
int Z80_Info (const char *path);

/* Read and parse the snapshot header; populate *out.  Does not load RAM. */
int Z80_GetFileInfo (const char *path, Z80FileInfo *out);

/* Stream only the v1 .z80 body to ZX RAM via NMI mailbox (0x4000..0xFFFF).
   No CPU-state restore or launch/handover is performed. */
int Z80_LoadAndRun (const char *path);

#endif
