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

/* Transport selector for the snapshot body bulk write. */
#define Z80L_VIA_NMI    0
#define Z80L_VIA_BUSREQ 1

/* Print a header summary without touching ZX RAM. */
int Z80_Info (const char *path);

/* Stream a v1 .z80 snapshot to the Spectrum and resume execution.
   transport selects how the 49152-byte body is staged into ZX RAM:
     Z80L_VIA_NMI    -> all writes via the cart-ROM NMI mailbox (slow, but
                        works in ULA-contended pages 0x4000-0x7FFF reliably).
     Z80L_VIA_BUSREQ -> direct BUSREQ writes to ZX DRAM (fast, but contended
                        pages may flake).
   Register restore + ROMCS hand-off uses the cart-ISR M1 detection
   mechanism in zx_bus.c. */
int Z80_LoadAndRun (const char *path, int transport);

#endif
