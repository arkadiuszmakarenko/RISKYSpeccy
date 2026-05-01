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

/* Stream a v1 .z80 snapshot to the Spectrum and resume execution via the
   cart-RAM launcher (zxprog BSS is in cart RAM, no guard region needed).
   Body is written by BUSREQ; registers and launcher tail are staged in
   cart RAM at 0x3F80 / 0x3FF0 respectively. */
int Z80_LoadAndRun (const char *path);

#endif
