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

/* Print a header summary without touching ZX RAM. */
int Z80_Info (const char *path);

/* Stream only the v1 .z80 body to ZX RAM via NMI mailbox (0x4000..0xFFFF).
   No CPU-state restore or launch/handover is performed. */
int Z80_LoadAndRun (const char *path);

#endif
